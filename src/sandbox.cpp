#include "include/sandbox.hpp"

#include "include/shell_utils.hpp"
#include "include/unique_fd.hpp"

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sched.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace shell {
namespace {

constexpr long CGROUP2_SUPER_MAGIC_VALUE = 0x63677270;

bool parse_unsigned(const std::string& text,
                    unsigned long long& value,
                    bool allow_zero = false) {
    if (text.empty() || text.front() == '-') {
        return false;
    }
    try {
        std::size_t consumed = 0;
        const unsigned long long parsed = std::stoull(text, &consumed, 10);
        if (consumed != text.size() || (!allow_zero && parsed == 0)) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_size(std::string text, unsigned long long& bytes) {
    if (text.empty() || text.front() == '-') {
        return false;
    }

    unsigned long long multiplier = 1;
    auto ends_with = [&](const std::string& suffix) {
        return text.size() >= suffix.size() &&
               text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    if (ends_with("KiB")) { multiplier = 1024ULL; text.resize(text.size() - 3); }
    else if (ends_with("MiB")) { multiplier = 1024ULL * 1024ULL; text.resize(text.size() - 3); }
    else if (ends_with("GiB")) { multiplier = 1024ULL * 1024ULL * 1024ULL; text.resize(text.size() - 3); }
    else if (ends_with("K") || ends_with("k")) { multiplier = 1024ULL; text.pop_back(); }
    else if (ends_with("M") || ends_with("m")) { multiplier = 1024ULL * 1024ULL; text.pop_back(); }
    else if (ends_with("G") || ends_with("g")) { multiplier = 1024ULL * 1024ULL * 1024ULL; text.pop_back(); }
    else if (ends_with("B") || ends_with("b")) { text.pop_back(); }

    unsigned long long base = 0;
    if (!parse_unsigned(text, base, true)) {
        return false;
    }
    if (base > std::numeric_limits<unsigned long long>::max() / multiplier) {
        return false;
    }
    bytes = base * multiplier;
    return true;
}

bool option_value(const Command& command,
                  std::size_t& index,
                  const std::string& option,
                  std::string& value,
                  std::string& error) {
    if (index + 1 >= command.args.size()) {
        error = "sandbox: " + option + " requires a value";
        return false;
    }
    value = command.args[++index];
    return true;
}

bool write_text_file(const std::filesystem::path& path,
                     const std::string& data,
                     std::string& error) {
    std::ofstream stream(path);
    if (!stream) {
        error = path.string() + ": " + std::strerror(errno);
        return false;
    }
    stream << data;
    if (!stream) {
        error = path.string() + ": write failed";
        return false;
    }
    return true;
}

std::optional<std::string> current_cgroup_path() {
    std::ifstream input("/proc/self/cgroup");
    std::string line;
    while (std::getline(input, line)) {
        // cgroup v2 line: 0::/some/path
        const auto marker = line.find("::");
        if (marker != std::string::npos) {
            return line.substr(marker + 2);
        }
    }
    return std::nullopt;
}

std::filesystem::path cgroup_mount_root() {
    return "/sys/fs/cgroup";
}

bool probe_cgroup_delegation(const std::filesystem::path& parent) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto probe = parent / (".myshell-probe-" + std::to_string(::getpid()) +
                                 "-" + std::to_string(nonce));
    std::error_code ec;
    const bool created = std::filesystem::create_directory(probe, ec);
    if (!created) {
        return false;
    }
    std::filesystem::remove(probe, ec);
    return true;
}

bool probe_unshare_flags(int flags) {
#ifdef __linux__
    const pid_t child = ::fork();
    if (child == -1) {
        return false;
    }
    if (child == 0) {
        ::_exit(::unshare(flags) == 0 ? 0 : 1);
    }

    int status = 0;
    while (::waitpid(child, &status, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
    (void)flags;
    return false;
#endif
}

bool probe_namespace_flag(int flag) {
#ifdef __linux__
    if (probe_unshare_flags(flag)) {
        return true;
    }
    // An unprivileged process may be unable to create UTS/PID/mount/network
    // namespaces directly while still being permitted to create them together
    // with a fresh user namespace. Probe that path as well because it matches
    // the sandbox runtime's privilege model.
    if (flag != CLONE_NEWUSER && probe_unshare_flags(CLONE_NEWUSER | flag)) {
        return true;
    }
#else
    (void)flag;
#endif
    return false;
}

class CgroupScope {
public:
    CgroupScope() = default;
    CgroupScope(const CgroupScope&) = delete;
    CgroupScope& operator=(const CgroupScope&) = delete;

    ~CgroupScope() { cleanup(); }

    bool create(const SandboxConfig& cfg, std::string& error) {
        if (!cfg.has_memory_max && !cfg.has_pids_max && !cfg.has_cpu_percent) {
            return true;
        }

        struct statfs fs {};
        const auto root = cgroup_mount_root();
        if (::statfs(root.c_str(), &fs) == -1 || fs.f_type != CGROUP2_SUPER_MAGIC_VALUE) {
            error = "cgroup v2 is not mounted";
            return false;
        }

        const auto base_path = current_cgroup_path();
        if (!base_path) {
            error = "could not determine current cgroup";
            return false;
        }

        parent_ = root / base_path->substr(base_path->starts_with('/') ? 1 : 0);
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = parent_ / ("myshell-" + std::to_string(::getpid()) + "-" + std::to_string(nonce));

        std::error_code ec;
        if (!std::filesystem::create_directory(path_, ec)) {
            error = "create cgroup " + path_.string() + ": " + ec.message();
            path_.clear();
            return false;
        }

        auto require_controller_file = [&](const char* file, const char* controller) {
            if (std::filesystem::exists(path_ / file)) {
                return true;
            }
            error = std::string(controller) +
                    " controller is not delegated to the shell's cgroup";
            cleanup();
            return false;
        };

        if (cfg.has_memory_max && !require_controller_file("memory.max", "memory")) return false;
        if (cfg.has_pids_max && !require_controller_file("pids.max", "pids")) return false;
        if (cfg.has_cpu_percent && !require_controller_file("cpu.max", "cpu")) return false;

        std::string write_error;
        if (cfg.has_memory_max &&
            !write_text_file(path_ / "memory.max", std::to_string(cfg.memory_max_bytes), write_error)) {
            error = write_error;
            cleanup();
            return false;
        }
        if (cfg.has_pids_max &&
            !write_text_file(path_ / "pids.max", std::to_string(cfg.pids_max), write_error)) {
            error = write_error;
            cleanup();
            return false;
        }
        if (cfg.has_cpu_percent) {
            constexpr unsigned long long period_us = 100000;
            const unsigned long long quota_us =
                (period_us * static_cast<unsigned long long>(cfg.cpu_percent)) / 100ULL;
            if (!write_text_file(path_ / "cpu.max",
                                 std::to_string(quota_us) + " " + std::to_string(period_us),
                                 write_error)) {
                error = write_error;
                cleanup();
                return false;
            }
        }
        return true;
    }

    bool attach(pid_t pid, std::string& error) const {
        if (path_.empty()) {
            return true;
        }
        return write_text_file(path_ / "cgroup.procs", std::to_string(pid), error);
    }

    void cleanup() noexcept {
        if (path_.empty()) {
            return;
        }

        // If the launched program left descendants behind, a cgroup-backed
        // sandbox owns those descendants as well. cgroup.kill is available on
        // modern cgroup-v2 kernels; ignore failure so older kernels still get
        // best-effort directory cleanup.
        if (std::filesystem::exists(path_ / "cgroup.kill")) {
            std::string ignored;
            (void)write_text_file(path_ / "cgroup.kill", "1", ignored);
        }

        std::error_code ec;
        for (int attempt = 0; attempt < 20; ++attempt) {
            ec.clear();
            if (std::filesystem::remove(path_, ec) || !std::filesystem::exists(path_, ec)) {
                break;
            }
            ::usleep(1000);
        }
        path_.clear();
    }

private:
    std::filesystem::path parent_;
    std::filesystem::path path_;
};

bool map_current_user_to_root(int error_fd) {
    const uid_t uid = ::getuid();
    const gid_t gid = ::getgid();

    if (::unshare(CLONE_NEWUSER) == -1) {
        write_all(error_fd, std::string("sandbox: unshare user namespace: ") +
                                std::strerror(errno) + "\n");
        return false;
    }

    std::string error;
    // Required before an unprivileged process may write gid_map on Linux.
    if (std::filesystem::exists("/proc/self/setgroups")) {
        if (!write_text_file("/proc/self/setgroups", "deny", error)) {
            write_all(error_fd, "sandbox: " + error + "\n");
            return false;
        }
    }
    if (!write_text_file("/proc/self/uid_map",
                         "0 " + std::to_string(uid) + " 1\n", error)) {
        write_all(error_fd, "sandbox: " + error + "\n");
        return false;
    }
    if (!write_text_file("/proc/self/gid_map",
                         "0 " + std::to_string(gid) + " 1\n", error)) {
        write_all(error_fd, "sandbox: " + error + "\n");
        return false;
    }

    if (::setresgid(0, 0, 0) == -1 || ::setresuid(0, 0, 0) == -1) {
        write_all(error_fd, std::string("sandbox: setresuid/setresgid: ") +
                                std::strerror(errno) + "\n");
        return false;
    }
    return true;
}

int wait_status_to_exit_code(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

[[noreturn]] void exec_program(const std::vector<std::string>& args, int error_fd) {
    const std::string executable = find_executable(args.front());
    if (executable.empty()) {
        write_all(error_fd, args.front() + ": command not found\n");
        ::_exit(127);
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    ::execv(executable.c_str(), argv.data());
    write_all(error_fd, "sandbox: exec " + args.front() + ": " + std::strerror(errno) + "\n");
    ::_exit(126);
}

int run_inside_namespaces(const SandboxConfig& cfg, int error_fd) {
    int flags = 0;
    if (cfg.mount_namespace) flags |= CLONE_NEWNS;
    if (cfg.network_namespace) flags |= CLONE_NEWNET;
    if (cfg.uts_namespace) flags |= CLONE_NEWUTS;
    if (cfg.pid_namespace) flags |= CLONE_NEWPID;

    const bool needs_privileged_namespace = flags != 0;
    // UID 0 inside a container does not necessarily carry CAP_SYS_ADMIN. Match
    // the capability probe: if direct namespace creation is denied, fall back
    // to an unprivileged user namespace and map the caller to namespace-root.
    const bool direct_namespaces_permitted =
        !needs_privileged_namespace || probe_unshare_flags(flags);
    const bool use_user_namespace =
        cfg.user_namespace || (needs_privileged_namespace && !direct_namespaces_permitted);

    if (use_user_namespace && !map_current_user_to_root(error_fd)) {
        return 1;
    }

    if (flags != 0 && ::unshare(flags) == -1) {
        write_all(error_fd, std::string("sandbox: unshare namespaces: ") +
                                std::strerror(errno) + "\n");
        return 1;
    }

    if (cfg.mount_namespace) {
        if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == -1) {
            write_all(error_fd, std::string("sandbox: make mounts private: ") +
                                    std::strerror(errno) + "\n");
            return 1;
        }
    }

    if (cfg.uts_namespace && !cfg.hostname.empty()) {
        if (::sethostname(cfg.hostname.c_str(), cfg.hostname.size()) == -1) {
            write_all(error_fd, std::string("sandbox: sethostname: ") +
                                    std::strerror(errno) + "\n");
            return 1;
        }
    }

    // CLONE_NEWPID only affects children created after unshare(), so create an
    // inner init process when PID isolation is requested.
    if (cfg.pid_namespace) {
        const pid_t init_pid = ::fork();
        if (init_pid == -1) {
            write_all(error_fd, std::string("sandbox: fork pid-namespace init: ") +
                                    std::strerror(errno) + "\n");
            return 1;
        }
        if (init_pid > 0) {
            int status = 0;
            while (::waitpid(init_pid, &status, 0) == -1 && errno == EINTR) {
            }
            return wait_status_to_exit_code(status);
        }

        // Do not allow the namespace-init process to outlive its supervisor if
        // the outer shell job is interrupted or terminated unexpectedly.
        const pid_t supervisor = ::getppid();
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) == -1 || ::getppid() != supervisor) {
            ::_exit(1);
        }

        if (cfg.mount_proc) {
            if (!cfg.mount_namespace) {
                write_all(error_fd, "sandbox: --proc requires --mount (or --all)\n");
                ::_exit(2);
            }
            if (::mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) == -1) {
                write_all(error_fd, std::string("sandbox: mount /proc: ") +
                                        std::strerror(errno) + "\n");
                ::_exit(1);
            }
        }

        exec_program(cfg.program_args, error_fd);
    }

    if (cfg.mount_proc) {
        write_all(error_fd, "sandbox: --proc requires --pid\n");
        return 2;
    }

    exec_program(cfg.program_args, error_fd);
}

}  // namespace

std::string sandbox_usage() {
    return
        "usage: sandbox [OPTIONS] -- PROGRAM [ARGS...]\n"
        "       sandbox --status\n\n"
        "Create Linux namespace isolation around PROGRAM. Optional cgroup v2\n"
        "limits are applied when a writable delegated cgroup is available.\n\n"
        "Namespace options:\n"
        "  --user             isolate user/group IDs\n"
        "  --pid              isolate process IDs\n"
        "  --mount            isolate the mount table\n"
        "  --net, --no-network isolate the network stack\n"
        "  --uts              isolate hostname/domain name\n"
        "  --hostname NAME    set hostname (implies --uts)\n"
        "  --proc             mount a fresh /proc (requires --pid and --mount)\n"
        "  --all              enable user, pid, mount, net, uts, and /proc\n\n"
        "cgroup v2 options:\n"
        "  --memory SIZE      memory.max (B/K/M/G suffixes)\n"
        "  --pids N           pids.max\n"
        "  --cpu-percent N    cpu.max as N% of one CPU (1..1000)\n\n"
        "Other:\n"
        "  --status           report namespace/cgroup capabilities\n"
        "  -h, --help         show this help\n\n"
        "This is an educational isolation runtime, not a hardened security sandbox.\n";
}

SandboxParseResult parse_sandbox(const Command& command) {
    SandboxParseResult result;
    if (command.args.empty() || command.args.front() != "sandbox") {
        result.error = "sandbox: internal parse error";
        return result;
    }

    std::size_t i = 1;
    bool explicit_separator = false;
    for (; i < command.args.size(); ++i) {
        const std::string& arg = command.args[i];
        if (arg == "--") {
            explicit_separator = true;
            ++i;
            break;
        }
        if (arg == "-h" || arg == "--help") {
            result.config.show_help = true;
            ++i;
            break;
        }
        if (arg == "--status") {
            result.config.show_status = true;
            ++i;
            continue;
        }
        if (arg.empty() || arg.front() != '-') {
            break;
        }

        if (arg == "--user") result.config.user_namespace = true;
        else if (arg == "--pid") result.config.pid_namespace = true;
        else if (arg == "--mount") result.config.mount_namespace = true;
        else if (arg == "--net" || arg == "--no-network") result.config.network_namespace = true;
        else if (arg == "--uts") result.config.uts_namespace = true;
        else if (arg == "--proc") result.config.mount_proc = true;
        else if (arg == "--all") {
            result.config.user_namespace = true;
            result.config.pid_namespace = true;
            result.config.mount_namespace = true;
            result.config.network_namespace = true;
            result.config.uts_namespace = true;
            result.config.mount_proc = true;
        } else if (arg == "--hostname") {
            std::string value;
            if (!option_value(command, i, arg, value, result.error)) return result;
            if (value.empty() || value.size() > HOST_NAME_MAX) {
                result.error = "sandbox: invalid hostname";
                return result;
            }
            result.config.hostname = value;
            result.config.uts_namespace = true;
        } else if (arg == "--memory") {
            std::string value;
            if (!option_value(command, i, arg, value, result.error)) return result;
            if (!parse_size(value, result.config.memory_max_bytes)) {
                result.error = "sandbox: invalid --memory value: " + value;
                return result;
            }
            result.config.has_memory_max = true;
        } else if (arg == "--pids") {
            std::string value;
            if (!option_value(command, i, arg, value, result.error)) return result;
            if (!parse_unsigned(value, result.config.pids_max)) {
                result.error = "sandbox: invalid --pids value: " + value;
                return result;
            }
            result.config.has_pids_max = true;
        } else if (arg == "--cpu-percent") {
            std::string value;
            unsigned long long parsed = 0;
            if (!option_value(command, i, arg, value, result.error)) return result;
            if (!parse_unsigned(value, parsed) || parsed > 1000) {
                result.error = "sandbox: --cpu-percent expects 1..1000";
                return result;
            }
            result.config.cpu_percent = static_cast<unsigned int>(parsed);
            result.config.has_cpu_percent = true;
        } else {
            result.error = "sandbox: unknown option: " + arg;
            return result;
        }
    }

    if (result.config.show_help) {
        if (i < command.args.size()) result.error = "sandbox: --help does not accept a program";
        return result;
    }
    if (result.config.show_status) {
        if (i < command.args.size()) result.error = "sandbox: --status does not accept a program";
        return result;
    }
    if (result.config.mount_proc &&
        (!result.config.pid_namespace || !result.config.mount_namespace)) {
        result.error = "sandbox: --proc requires both --pid and --mount";
        return result;
    }
    if (explicit_separator && i >= command.args.size()) {
        result.error = "sandbox: missing program after --";
        return result;
    }
    if (i >= command.args.size()) {
        result.error = "sandbox: missing program";
        return result;
    }

    result.config.program_args.assign(command.args.begin() + static_cast<std::ptrdiff_t>(i),
                                      command.args.end());
    return result;
}

IsolationCapabilities detect_isolation_capabilities() {
    IsolationCapabilities out;
#ifdef __linux__
    out.linux_namespaces = std::filesystem::exists("/proc/self/ns");
    if (out.linux_namespaces) {
        out.user_namespace_available = probe_namespace_flag(CLONE_NEWUSER);
        out.pid_namespace_available = probe_namespace_flag(CLONE_NEWPID);
        out.mount_namespace_available = probe_namespace_flag(CLONE_NEWNS);
        out.network_namespace_available = probe_namespace_flag(CLONE_NEWNET);
        out.uts_namespace_available = probe_namespace_flag(CLONE_NEWUTS);
    }

    struct statfs fs {};
    const auto root = cgroup_mount_root();
    if (::statfs(root.c_str(), &fs) == 0 && fs.f_type == CGROUP2_SUPER_MAGIC_VALUE) {
        out.cgroup_v2 = true;
        out.cgroup_root = root.string();

        if (const auto current = current_cgroup_path()) {
            const auto current_dir = root / current->substr(current->starts_with('/') ? 1 : 0);
            // access(W_OK) is misleading for root on a read-only cgroup mount.
            // Probe delegation by actually creating and removing an empty child.
            out.cgroup_writable = probe_cgroup_delegation(current_dir);
        }
    }
#endif
    return out;
}

std::string render_isolation_status() {
    const IsolationCapabilities caps = detect_isolation_capabilities();
    std::ostringstream out;
    out << "Isolation capabilities\n";
    out << "linux-namespaces: " << (caps.linux_namespaces ? "kernel support" : "unavailable") << '\n';
    out << "user-namespace: " << (caps.user_namespace_available ? "permitted" : "unavailable") << '\n';
    out << "pid-namespace: " << (caps.pid_namespace_available ? "permitted" : "unavailable") << '\n';
    out << "mount-namespace: " << (caps.mount_namespace_available ? "permitted" : "unavailable") << '\n';
    out << "network-namespace: " << (caps.network_namespace_available ? "permitted" : "unavailable") << '\n';
    out << "uts-namespace: " << (caps.uts_namespace_available ? "permitted" : "unavailable") << '\n';
    out << "cgroup-v2: " << (caps.cgroup_v2 ? "mounted" : "unavailable") << '\n';
    if (caps.cgroup_v2) {
        out << "cgroup-write: " << (caps.cgroup_writable ? "available" : "unavailable") << '\n';
        out << "cgroup-root: " << caps.cgroup_root << '\n';
    }
    return out.str();
}

int execute_sandbox_child(const Command& command, int output_fd, int error_fd) {
    const SandboxParseResult parsed = parse_sandbox(command);
    if (!parsed.ok()) {
        write_all(error_fd, parsed.error + "\n");
        return 2;
    }
    const SandboxConfig& cfg = parsed.config;
    if (cfg.show_help) {
        write_all(output_fd, sandbox_usage());
        return 0;
    }
    if (cfg.show_status) {
        write_all(output_fd, render_isolation_status());
        return 0;
    }

#ifndef __linux__
    write_all(error_fd, "sandbox: Linux namespaces/cgroups are required\n");
    return 1;
#else
    CgroupScope cgroup;
    std::string cgroup_error;
    if (!cgroup.create(cfg, cgroup_error)) {
        write_all(error_fd, "sandbox: " + cgroup_error + "\n");
        return 1;
    }

    int sync_pipe[2];
    if (::pipe(sync_pipe) == -1) {
        write_all(error_fd, std::string("sandbox: pipe: ") + std::strerror(errno) + "\n");
        return 1;
    }
    UniqueFd ready_read(sync_pipe[0]);
    UniqueFd ready_write(sync_pipe[1]);

    const pid_t worker = ::fork();
    if (worker == -1) {
        write_all(error_fd, std::string("sandbox: fork: ") + std::strerror(errno) + "\n");
        return 1;
    }

    if (worker == 0) {
        const pid_t supervisor = ::getppid();
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) == -1 || ::getppid() != supervisor) {
            ::_exit(1);
        }
        ready_write.reset();
        char token = 0;
        ssize_t n = 0;
        do {
            n = ::read(ready_read.get(), &token, 1);
        } while (n == -1 && errno == EINTR);
        ready_read.reset();
        if (n != 1) {
            ::_exit(1);
        }
        ::_exit(run_inside_namespaces(cfg, error_fd));
    }

    ready_read.reset();
    if (!cgroup.attach(worker, cgroup_error)) {
        write_all(error_fd, "sandbox: " + cgroup_error + "\n");
        ::kill(worker, SIGKILL);
        ready_write.reset();
        int ignored = 0;
        ::waitpid(worker, &ignored, 0);
        return 1;
    }

    const char token = 1;
    if (::write(ready_write.get(), &token, 1) != 1) {
        write_all(error_fd, "sandbox: failed to release sandbox worker\n");
        ::kill(worker, SIGKILL);
    }
    ready_write.reset();

    int status = 0;
    while (::waitpid(worker, &status, 0) == -1) {
        if (errno == EINTR) continue;
        write_all(error_fd, std::string("sandbox: waitpid: ") + std::strerror(errno) + "\n");
        return 1;
    }
    return wait_status_to_exit_code(status);
#endif
}

}  // namespace shell
