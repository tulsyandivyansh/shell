#include "include/resource_control.hpp"

#include "include/shell_utils.hpp"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

namespace shell {
namespace {

bool parse_unsigned(const std::string& text, unsigned long long& value, bool allow_zero) {
    if (text.empty() || text[0] == '-') {
        return false;
    }
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(text, &consumed, 10);
        if (consumed != text.size() || (!allow_zero && parsed == 0)) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_size(const std::string& text, unsigned long long& bytes) {
    if (text.empty() || text[0] == '-') {
        return false;
    }

    std::size_t split = 0;
    while (split < text.size() && text[split] >= '0' && text[split] <= '9') {
        ++split;
    }
    if (split == 0) {
        return false;
    }

    unsigned long long value = 0;
    if (!parse_unsigned(text.substr(0, split), value, true)) {
        return false;
    }

    std::string suffix = text.substr(split);
    for (char& c : suffix) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }

    unsigned long long multiplier = 1;
    if (suffix.empty() || suffix == "B") {
        multiplier = 1;
    } else if (suffix == "K" || suffix == "KB" || suffix == "KIB") {
        multiplier = 1024ULL;
    } else if (suffix == "M" || suffix == "MB" || suffix == "MIB") {
        multiplier = 1024ULL * 1024ULL;
    } else if (suffix == "G" || suffix == "GB" || suffix == "GIB") {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else {
        return false;
    }

    if (value > std::numeric_limits<unsigned long long>::max() / multiplier) {
        return false;
    }
    bytes = value * multiplier;
    return true;
}

bool parse_nice_value(const std::string& text, int& value) {
    try {
        std::size_t consumed = 0;
        const long parsed = std::stol(text, &consumed, 10);
        if (consumed != text.size() || parsed < -20 || parsed > 19) {
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

std::string limit_value(rlim_t value) {
    if (value == RLIM_INFINITY) {
        return "unlimited";
    }
    return std::to_string(static_cast<unsigned long long>(value));
}

std::string format_limit_line(const char* name, int resource, const char* unit) {
    rlimit limit{};
    if (::getrlimit(resource, &limit) == -1) {
        return std::string(name) + ": unavailable\n";
    }
    return std::string(name) + ": soft=" + limit_value(limit.rlim_cur) +
           " hard=" + limit_value(limit.rlim_max) + " " + unit + "\n";
}

bool apply_soft_limit(int resource,
                      unsigned long long requested,
                      const char* label,
                      int error_fd) {
    if (requested >= static_cast<unsigned long long>(RLIM_INFINITY)) {
        write_all(error_fd, std::string("run: ") + label + " limit is too large\n");
        return false;
    }

    rlimit current{};
    if (::getrlimit(resource, &current) == -1) {
        write_all(error_fd, std::string("run: getrlimit(") + label + "): " +
                                std::strerror(errno) + "\n");
        return false;
    }

    if (current.rlim_max != RLIM_INFINITY &&
        requested > static_cast<unsigned long long>(current.rlim_max)) {
        write_all(error_fd, std::string("run: requested ") + label +
                                " limit exceeds hard limit\n");
        return false;
    }

    const rlim_t value = static_cast<rlim_t>(requested);
    rlimit updated{value, current.rlim_max};
    if (::setrlimit(resource, &updated) == -1) {
        write_all(error_fd, std::string("run: setrlimit(") + label + "): " +
                                std::strerror(errno) + "\n");
        return false;
    }
    return true;
}

bool option_value(const Command& command,
                  std::size_t& i,
                  const std::string& option,
                  std::string& value,
                  std::string& error) {
    if (i + 1 >= command.args.size()) {
        error = "run: " + option + " requires a value";
        return false;
    }
    value = command.args[++i];
    return true;
}

}  // namespace

std::string resource_run_usage() {
    return
        "usage: run [OPTIONS] -- PROGRAM [ARGS...]\n"
        "       run [OPTIONS] PROGRAM [ARGS...]\n"
        "\n"
        "Run PROGRAM in a child process with per-process resource controls.\n"
        "\n"
        "Options:\n"
        "  --max-memory SIZE   address-space limit (B/K/M/G suffixes)\n"
        "  --max-fds N         maximum open file descriptors\n"
        "  --cpu-time SEC      CPU-time limit in seconds\n"
        "  --max-procs N       maximum processes for this real user (RLIMIT_NPROC)\n"
        "  --core-size SIZE    maximum core-file size (B/K/M/G; 0 disables cores)\n"
        "  --nice N            scheduling nice value from -20 to 19\n"
        "  -h, --help          show this help\n";
}

ResourceRunParseResult parse_resource_run(const Command& command) {
    ResourceRunParseResult out;
    if (command.args.empty() || command.args[0] != "run") {
        out.error = "run: internal parse error";
        return out;
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
            out.config.show_help = true;
            ++i;
            break;
        }
        if (arg.empty() || arg[0] != '-') {
            break;
        }

        std::string value;
        if (arg == "--max-memory") {
            if (!option_value(command, i, arg, value, out.error) ||
                !parse_size(value, out.config.max_memory_bytes)) {
                if (out.error.empty()) out.error = "run: invalid --max-memory value: " + value;
                return out;
            }
            out.config.has_max_memory = true;
        } else if (arg == "--max-fds") {
            if (!option_value(command, i, arg, value, out.error) ||
                !parse_unsigned(value, out.config.max_fds, false)) {
                if (out.error.empty()) out.error = "run: invalid --max-fds value: " + value;
                return out;
            }
            out.config.has_max_fds = true;
        } else if (arg == "--cpu-time") {
            if (!option_value(command, i, arg, value, out.error) ||
                !parse_unsigned(value, out.config.cpu_time_seconds, false)) {
                if (out.error.empty()) out.error = "run: invalid --cpu-time value: " + value;
                return out;
            }
            out.config.has_cpu_time = true;
        } else if (arg == "--max-procs") {
            if (!option_value(command, i, arg, value, out.error) ||
                !parse_unsigned(value, out.config.max_procs, false)) {
                if (out.error.empty()) out.error = "run: invalid --max-procs value: " + value;
                return out;
            }
            out.config.has_max_procs = true;
        } else if (arg == "--core-size") {
            if (!option_value(command, i, arg, value, out.error) ||
                !parse_size(value, out.config.core_size_bytes)) {
                if (out.error.empty()) out.error = "run: invalid --core-size value: " + value;
                return out;
            }
            out.config.has_core_size = true;
        } else if (arg == "--nice") {
            if (!option_value(command, i, arg, value, out.error) ||
                !parse_nice_value(value, out.config.nice_value)) {
                if (out.error.empty()) out.error = "run: --nice expects an integer from -20 to 19";
                return out;
            }
            out.config.has_nice = true;
        } else {
            out.error = "run: unknown option: " + arg;
            return out;
        }
    }

    if (out.config.show_help) {
        if (i < command.args.size()) {
            out.error = "run: --help does not accept a program";
        }
        return out;
    }

    if (explicit_separator && i >= command.args.size()) {
        out.error = "run: missing program after --";
        return out;
    }
    if (i >= command.args.size()) {
        out.error = "run: missing program";
        return out;
    }

    out.config.program_args.assign(command.args.begin() + static_cast<std::ptrdiff_t>(i),
                                   command.args.end());
    return out;
}

std::string render_current_limits() {
    std::string out;
    out += "Current process resource limits\n";
    out += format_limit_line("address-space", RLIMIT_AS, "bytes");
    out += format_limit_line("open-files", RLIMIT_NOFILE, "fds");
    out += format_limit_line("cpu-time", RLIMIT_CPU, "seconds");
#ifdef RLIMIT_NPROC
    out += format_limit_line("processes", RLIMIT_NPROC, "processes");
#endif
    out += format_limit_line("core-size", RLIMIT_CORE, "bytes");
    errno = 0;
    const int priority = ::getpriority(PRIO_PROCESS, 0);
    if (priority == -1 && errno != 0) {
        out += "nice: unavailable\n";
    } else {
        out += "nice: " + std::to_string(priority) + "\n";
    }
    return out;
}

int execute_resource_run_child(const Command& command, int output_fd, int error_fd) {
    const ResourceRunParseResult parsed = parse_resource_run(command);
    if (!parsed.ok()) {
        write_all(error_fd, parsed.error + "\n");
        return 2;
    }
    const ResourceRunConfig& cfg = parsed.config;
    if (cfg.show_help) {
        write_all(output_fd, resource_run_usage());
        return 0;
    }

    if (cfg.has_max_memory &&
        !apply_soft_limit(RLIMIT_AS, cfg.max_memory_bytes, "address-space", error_fd)) {
        return 1;
    }
    if (cfg.has_max_fds &&
        !apply_soft_limit(RLIMIT_NOFILE, cfg.max_fds, "open-files", error_fd)) {
        return 1;
    }
    if (cfg.has_cpu_time &&
        !apply_soft_limit(RLIMIT_CPU, cfg.cpu_time_seconds, "cpu-time", error_fd)) {
        return 1;
    }
#ifdef RLIMIT_NPROC
    if (cfg.has_max_procs &&
        !apply_soft_limit(RLIMIT_NPROC, cfg.max_procs, "processes", error_fd)) {
        return 1;
    }
#else
    if (cfg.has_max_procs) {
        write_all(error_fd, "run: process limits are not supported on this platform\n");
        return 1;
    }
#endif
    if (cfg.has_core_size &&
        !apply_soft_limit(RLIMIT_CORE, cfg.core_size_bytes, "core-size", error_fd)) {
        return 1;
    }

    if (cfg.has_nice) {
        if (::setpriority(PRIO_PROCESS, 0, cfg.nice_value) == -1) {
            write_all(error_fd, std::string("run: setpriority: ") + std::strerror(errno) + "\n");
            return 1;
        }
    }

    const std::string executable = find_executable(cfg.program_args.front());
    if (executable.empty()) {
        write_all(error_fd, cfg.program_args.front() + ": command not found\n");
        return 127;
    }

    std::vector<char*> argv;
    argv.reserve(cfg.program_args.size() + 1);
    for (const auto& arg : cfg.program_args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    ::execv(executable.c_str(), argv.data());
    write_all(error_fd,
              "run: exec " + cfg.program_args.front() + ": " + std::strerror(errno) + "\n");
    return 126;
}

}  // namespace shell
