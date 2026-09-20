#include "include/builtins.hpp"

#include "include/history.hpp"
#include "include/process_info.hpp"
#include "include/resource_control.hpp"
#include "include/sandbox.hpp"
#include "include/trace.hpp"
#include "include/shell_utils.hpp"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace shell {
namespace {

BuiltinResult result(bool exit_requested = false, int exit_code = 0, int status = 0) {
    return BuiltinResult{true, exit_requested, exit_code, status};
}

void write_error(int error_fd, const std::string& message) {
    write_all(error_fd, message);
}

BuiltinResult builtin_echo(const Command& command, int output_fd) {
    std::string output;
    for (std::size_t i = 1; i < command.args.size(); ++i) {
        if (i > 1) {
            output += ' ';
        }
        output += process_echo_escapes(command.args[i]);
    }
    output += '\n';
    write_all(output_fd, output);
    return result();
}

BuiltinResult builtin_type(const Command& command, int output_fd) {
    if (command.args.size() < 2) {
        write_all(output_fd, "type: missing argument\n");
        return result(false, 0, 2);
    }

    const std::string& target = command.args[1];
    if (is_builtin(target)) {
        write_all(output_fd, target + " is a shell builtin\n");
    } else if (const std::string path = find_executable(target); !path.empty()) {
        write_all(output_fd, target + " is " + path + "\n");
    } else {
        write_all(output_fd, target + ": not found\n");
        return result(false, 0, 1);
    }
    return result();
}

BuiltinResult builtin_pwd(int output_fd, int error_fd) {
    char cwd[PATH_MAX];
    if (::getcwd(cwd, sizeof(cwd)) == nullptr) {
        write_error(error_fd, "pwd: error getting current directory\n");
        return result(false, 0, 1);
    }

    write_all(output_fd, std::string(cwd) + "\n");
    return result();
}

std::string expand_home(const std::string& path, int error_fd, bool& ok) {
    ok = true;
    if (path == "~" || path.rfind("~/", 0) == 0) {
        const char* home = std::getenv("HOME");
        if (home == nullptr) {
            write_error(error_fd, "cd: HOME environment variable not set\n");
            ok = false;
            return {};
        }
        return path == "~" ? std::string(home) : std::string(home) + path.substr(1);
    }
    return path;
}

BuiltinResult builtin_cd(const Command& command, int error_fd) {
    std::string requested;
    if (command.args.size() < 2) {
        const char* home = std::getenv("HOME");
        if (home == nullptr) {
            write_error(error_fd, "cd: HOME environment variable not set\n");
            return result(false, 0, 1);
        }
        requested = home;
    } else {
        requested = command.args[1];
    }

    bool ok = true;
    const std::string resolved = expand_home(requested, error_fd, ok);
    if (!ok) {
        return result(false, 0, 1);
    }

    struct stat info {};
    if (::stat(resolved.c_str(), &info) != 0) {
        write_error(error_fd, "cd: " + requested + ": No such file or directory\n");
        return result(false, 0, 1);
    }
    if (!S_ISDIR(info.st_mode)) {
        write_error(error_fd, "cd: " + requested + ": Not a directory\n");
        return result(false, 0, 1);
    }
    if (::access(resolved.c_str(), X_OK) != 0) {
        write_error(error_fd, "cd: " + requested + ": Permission denied\n");
        return result(false, 0, 1);
    }
    if (::chdir(resolved.c_str()) != 0) {
        write_error(error_fd, "cd: " + requested + ": Failed to change directory\n");
        return result(false, 0, 1);
    }
    return result();
}

BuiltinResult builtin_history(const Command& command, int output_fd) {
    if (command.args.size() > 2) {
        const std::string& option = command.args[1];
        const std::string& filename = command.args[2];
        if (option == "-r") {
            history::load_from_file(filename);
            return result();
        }
        if (option == "-w") {
            history::write_to_file(filename);
            return result();
        }
        if (option == "-a") {
            history::append_new_to_file(filename);
            return result();
        }
    }

    int max_entries = -1;
    if (command.args.size() > 1 && !command.args[1].empty() && command.args[1][0] != '-') {
        try {
            max_entries = std::stoi(command.args[1]);
        } catch (...) {
            max_entries = -1;
        }
    }

    write_all(output_fd, history::render(max_entries));
    return result();
}



bool parse_positive_long(const std::string& text, long& value) {
    if (text.empty()) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        const long parsed = std::stol(text, &consumed, 10);
        if (consumed != text.size() || parsed <= 0) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

BuiltinResult builtin_pinfo(const Command& command, int output_fd, int error_fd) {
    pid_t pid = ::getpid();
    bool watch = false;
    bool pid_explicit = false;
    long interval_ms = 1000;
    long count = 1;
    bool count_explicit = false;

    std::size_t i = 1;
    while (i < command.args.size()) {
        const std::string& arg = command.args[i];
        if (arg == "--help" || arg == "-h") {
            write_all(output_fd,
                      "usage: pinfo [PID|self]\n"
                      "       pinfo --watch [PID|self] [--interval MS] [--count N]\n");
            return result();
        }
        if (arg == "--watch" || arg == "-w") {
            watch = true;
            if (!count_explicit) {
                count = 0;  // continuous unless --count is provided
            }
            ++i;
            continue;
        }
        if (arg == "--interval") {
            if (i + 1 >= command.args.size() ||
                !parse_positive_long(command.args[i + 1], interval_ms) ||
                interval_ms > 60000) {
                write_error(error_fd, "pinfo: --interval expects 1..60000 milliseconds\n");
                return result(false, 0, 2);
            }
            i += 2;
            continue;
        }
        if (arg == "--count") {
            if (i + 1 >= command.args.size() ||
                !parse_positive_long(command.args[i + 1], count)) {
                write_error(error_fd, "pinfo: --count expects a positive integer\n");
                return result(false, 0, 2);
            }
            count_explicit = true;
            i += 2;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            write_error(error_fd, "pinfo: unknown option: " + arg + "\n");
            return result(false, 0, 2);
        }

        if (pid_explicit) {
            write_error(error_fd, "pinfo: multiple PIDs are not supported\n");
            return result(false, 0, 2);
        }
        pid_explicit = true;
        if (arg == "self") {
            pid = ::getpid();
        } else {
            long parsed_pid = 0;
            if (!parse_positive_long(arg, parsed_pid) ||
                parsed_pid > std::numeric_limits<pid_t>::max()) {
                write_error(error_fd, "pinfo: invalid PID: " + arg + "\n");
                return result(false, 0, 2);
            }
            pid = static_cast<pid_t>(parsed_pid);
        }
        ++i;
    }

    if (!watch && count_explicit) {
        write_error(error_fd, "pinfo: --count requires --watch\n");
        return result(false, 0, 2);
    }
    if (!watch && interval_ms != 1000) {
        write_error(error_fd, "pinfo: --interval requires --watch\n");
        return result(false, 0, 2);
    }

    long sample = 0;
    while (count == 0 || sample < count) {
        const ProcessInfoResult inspected = inspect_process(pid);
        if (!inspected.ok()) {
            write_error(error_fd, "pinfo: " + inspected.error + "\n");
            return result(false, 0, 1);
        }

        if (watch) {
            write_all(output_fd, "=== sample " + std::to_string(sample + 1) + " ===\n");
        }
        write_all(output_fd, render_process_info(*inspected.info));
        ++sample;

        if (count != 0 && sample >= count) {
            break;
        }

        timespec delay{};
        delay.tv_sec = interval_ms / 1000;
        delay.tv_nsec = (interval_ms % 1000) * 1000000L;
        while (::nanosleep(&delay, &delay) == -1 && errno == EINTR) {
        }
        if (watch) {
            write_all(output_fd, "\n");
        }
    }

    return result();
}

BuiltinResult builtin_exit(const Command& command, BuiltinContext context, int error_fd) {
    int exit_code = 0;
    if (command.args.size() > 1) {
        try {
            exit_code = std::stoi(command.args[1]);
        } catch (...) {
            write_error(error_fd, "exit: numeric argument required\n");
            exit_code = 2;
        }
    }

    if (context == BuiltinContext::ChildProcess) {
        return result(true, exit_code);
    }
    return result(true, exit_code);
}

}  // namespace

BuiltinResult execute_builtin(
    const Command& command,
    int output_fd,
    int error_fd,
    BuiltinContext context) {
    if (command.args.empty()) {
        return {};
    }

    const std::string& name = command.args.front();
    if (name == "echo") {
        return builtin_echo(command, output_fd);
    }
    if (name == "type") {
        return builtin_type(command, output_fd);
    }
    if (name == "pwd") {
        return builtin_pwd(output_fd, error_fd);
    }
    if (name == "cd") {
        return builtin_cd(command, error_fd);
    }
    if (name == "history") {
        return builtin_history(command, output_fd);
    }
    if (name == "pinfo") {
        return builtin_pinfo(command, output_fd, error_fd);
    }
    if (name == "limits") {
        write_all(output_fd, render_current_limits());
        return result();
    }
    if (name == "run") {
        // The executor always routes `run` through a child process so limits
        // cannot alter the long-lived shell. This is a defensive fallback.
        if (context == BuiltinContext::ParentShell) {
            write_error(error_fd, "run: internal error: resource command must execute in child context\n");
            return result(false, 0, 1);
        }
        const int status = execute_resource_run_child(command, output_fd, error_fd);
        return result(false, 0, status);
    }
    if (name == "sandbox") {
        // Namespace/cgroup changes must never affect the long-lived shell.
        if (context == BuiltinContext::ParentShell) {
            write_error(error_fd, "sandbox: internal error: isolation command must execute in child context\n");
            return result(false, 0, 1);
        }
        const int status = execute_sandbox_child(command, output_fd, error_fd);
        return result(false, 0, status);
    }
    if (name == "trace") {
        // ptrace ownership and the traced program must stay outside the parent shell.
        if (context == BuiltinContext::ParentShell) {
            write_error(error_fd, "trace: internal error: tracer must execute in child context\n");
            return result(false, 0, 1);
        }
        const int status = execute_trace_child(command, output_fd, error_fd);
        return result(false, 0, status);
    }
    if (name == "exit") {
        return builtin_exit(command, context, error_fd);
    }
    return {};
}

}  // namespace shell
