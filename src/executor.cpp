#include "include/executor.hpp"

#include "include/builtins.hpp"
#include "include/shell_utils.hpp"
#include "include/resource_control.hpp"
#include "include/sandbox.hpp"
#include "include/trace.hpp"
#include "include/unique_fd.hpp"

#include <cerrno>
#include <cctype>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace shell {
namespace {

struct PipePair {
    UniqueFd read_end;
    UniqueFd write_end;
};

UniqueFd open_output_redirect(const std::string& path, bool append) {
    const int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    return UniqueFd(::open(path.c_str(), flags, 0644));
}

UniqueFd open_input_redirect(const std::string& path) {
    return UniqueFd(::open(path.c_str(), O_RDONLY));
}

void duplicate_or_exit(int from, int to, const char* label) {
    if (from == to) {
        return;
    }
    if (::dup2(from, to) == -1) {
        write_all(STDERR_FILENO, std::string("Failed to redirect ") + label + "\n");
        ::_exit(1);
    }
}

void close_all_pipes(std::vector<PipePair>& pipes) {
    for (auto& pipe : pipes) {
        pipe.read_end.reset();
        pipe.write_end.reset();
    }
}

[[noreturn]] void exec_external_child(const Command& command) {
    const std::string executable = find_executable(command.args[0]);
    if (executable.empty()) {
        write_all(STDERR_FILENO, command.args[0] + ": command not found\n");
        ::_exit(127);
    }

    std::vector<char*> argv;
    argv.reserve(command.args.size() + 1);
    for (const auto& arg : command.args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    ::execv(executable.c_str(), argv.data());
    write_all(STDERR_FILENO, "Error executing " + command.args[0] + "\n");
    ::_exit(126);
}

bool should_execute(ChainOperator connector, int previous_status) {
    switch (connector) {
        case ChainOperator::Always:
            return true;
        case ChainOperator::And:
            return previous_status == 0;
        case ChainOperator::Or:
            return previous_status != 0;
    }
    return true;
}

bool is_job_builtin(const std::string& name) {
    return name == "jobs" || name == "fg" || name == "bg";
}


bool requires_child_builtin_execution(const Command& command) {
    if (command.args.empty()) {
        return false;
    }
    if (command.args.front() == "run" || command.args.front() == "sandbox" ||
        command.args.front() == "trace") {
        return true;
    }
    if (command.args.front() != "pinfo") {
        return false;
    }
    for (const std::string& arg : command.args) {
        if (arg == "--watch" || arg == "-w") {
            return true;
        }
    }
    return false;
}

std::string quote_for_display(const std::string& value) {
    if (value.empty()) {
        return "''";
    }

    bool needs_quotes = false;
    for (char c : value) {
        if (std::isspace(static_cast<unsigned char>(c)) ||
            c == '|' || c == '&' || c == ';' || c == '<' || c == '>') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return value;
    }

    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += '\'';
    return quoted;
}

std::string format_command(const Command& command) {
    std::string text;
    for (const std::string& arg : command.args) {
        if (!text.empty()) {
            text += ' ';
        }
        text += quote_for_display(arg);
    }
    if (command.has_input_redirect) {
        text += " < " + quote_for_display(command.input_file);
    }
    if (command.has_output_redirect) {
        text += command.append_output ? " >> " : " > ";
        text += quote_for_display(command.output_file);
    }
    if (command.has_error_redirect) {
        text += command.append_error ? " 2>> " : " 2> ";
        text += quote_for_display(command.error_file);
    }
    return text;
}

std::string format_pipeline(const Pipeline& pipeline) {
    std::string text;
    for (std::size_t i = 0; i < pipeline.commands.size(); ++i) {
        if (i > 0) {
            text += " | ";
        }
        text += format_command(pipeline.commands[i]);
    }
    return text;
}

void configure_child_io(const Command& command,
                        std::size_t index,
                        std::size_t command_count,
                        std::vector<PipePair>& pipes) {
    int input_fd = STDIN_FILENO;
    int output_fd = STDOUT_FILENO;
    int error_fd = STDERR_FILENO;
    UniqueFd command_input;
    UniqueFd command_output;
    UniqueFd command_error;

    if (command.has_input_redirect) {
        command_input = open_input_redirect(command.input_file);
        if (!command_input) {
            write_all(STDERR_FILENO, "Error opening input file\n");
            ::_exit(1);
        }
        input_fd = command_input.get();
    } else if (index > 0) {
        input_fd = pipes[index - 1].read_end.get();
    }

    if (command.has_output_redirect) {
        command_output = open_output_redirect(command.output_file, command.append_output);
        if (!command_output) {
            write_all(STDERR_FILENO, "Error opening output file\n");
            ::_exit(1);
        }
        output_fd = command_output.get();
    } else if (index + 1 < command_count) {
        output_fd = pipes[index].write_end.get();
    }

    if (command.has_error_redirect) {
        command_error = open_output_redirect(command.error_file, command.append_error);
        if (!command_error) {
            write_all(STDERR_FILENO, "Error opening error file\n");
            ::_exit(1);
        }
        error_fd = command_error.get();
    }

    duplicate_or_exit(input_fd, STDIN_FILENO, "stdin");
    duplicate_or_exit(output_fd, STDOUT_FILENO, "stdout");
    duplicate_or_exit(error_fd, STDERR_FILENO, "stderr");

    close_all_pipes(pipes);
}

}  // namespace

void Executor::reap_background_jobs(bool notify) {
    jobs_.reap_background(notify);
}

ExecutionResult Executor::execute(const ExecutionPlan& plan) {
    ExecutionResult last;
    bool have_status = false;

    for (const auto& step : plan.steps) {
        if (have_status && !should_execute(step.connector, last.status)) {
            continue;
        }

        last = execute_pipeline(step.pipeline);
        have_status = true;
        if (last.exit_requested) {
            return last;
        }
    }

    return last;
}

int Executor::execute_job_builtin(const Command& command, int output_fd, int error_fd) {
    const std::string& name = command.args.front();
    const std::string spec = command.args.size() > 1 ? command.args[1] : std::string{};

    if (name == "jobs") {
        jobs_.reap_background(false);
        write_all(output_fd, jobs_.render_jobs());
        return 0;
    }
    if (name == "fg") {
        return jobs_.foreground(spec, output_fd, error_fd);
    }
    if (name == "bg") {
        return jobs_.continue_background(spec, output_fd, error_fd);
    }
    return 1;
}

ExecutionResult Executor::execute_parent_builtin(const Command& command) {
    UniqueFd input;
    UniqueFd output;
    UniqueFd error;
    int input_fd = STDIN_FILENO;
    int output_fd = STDOUT_FILENO;
    int error_fd = STDERR_FILENO;

    if (command.has_input_redirect) {
        input = open_input_redirect(command.input_file);
        if (!input) {
            std::cerr << "Error opening input file\n";
            return {false, 0, 1};
        }
        input_fd = input.get();
    }
    if (command.has_output_redirect) {
        output = open_output_redirect(command.output_file, command.append_output);
        if (!output) {
            std::cerr << "Error opening output file\n";
            return {false, 0, 1};
        }
        output_fd = output.get();
    }
    if (command.has_error_redirect) {
        error = open_output_redirect(command.error_file, command.append_error);
        if (!error) {
            std::cerr << "Error opening error file\n";
            return {false, 0, 1};
        }
        error_fd = error.get();
    }

    (void)input_fd;

    if (is_job_builtin(command.args.front())) {
        return {false, 0, execute_job_builtin(command, output_fd, error_fd)};
    }

    const BuiltinResult builtin = execute_builtin(
        command, output_fd, error_fd, BuiltinContext::ParentShell);
    return ExecutionResult{builtin.exit_requested, builtin.exit_code, builtin.status};
}

ExecutionResult Executor::execute_pipeline(const Pipeline& pipeline) {
    if (pipeline.commands.empty()) {
        return {};
    }

    // Stateful built-ins execute in the parent only for a foreground,
    // single-command pipeline. Background built-ins run in a child, matching
    // normal shell subshell semantics and protecting parent shell state.
    if (!pipeline.background && pipeline.commands.size() == 1 &&
        !pipeline.commands.front().args.empty() &&
        is_builtin(pipeline.commands.front().args.front()) &&
        !requires_child_builtin_execution(pipeline.commands.front())) {
        return execute_parent_builtin(pipeline.commands.front());
    }

    std::vector<PipePair> pipes;
    if (pipeline.commands.size() > 1) {
        pipes.reserve(pipeline.commands.size() - 1);
        for (std::size_t i = 0; i + 1 < pipeline.commands.size(); ++i) {
            int fds[2];
            if (::pipe(fds) == -1) {
                std::cerr << "Failed to create pipe\n";
                return {false, 0, 1};
            }
            pipes.push_back(PipePair{UniqueFd(fds[0]), UniqueFd(fds[1])});
        }
    }

    std::vector<pid_t> pids;
    pids.reserve(pipeline.commands.size());
    pid_t pgid = 0;

    for (std::size_t i = 0; i < pipeline.commands.size(); ++i) {
        const Command& command = pipeline.commands[i];
        if (command.args.empty()) {
            continue;
        }

        const pid_t pid = ::fork();
        if (pid == -1) {
            std::cerr << "Failed to fork process\n";
            close_all_pipes(pipes);
            if (pgid > 0) {
                ::kill(-pgid, SIGTERM);
            }
            return {false, 0, 1};
        }

        if (pid == 0) {
            const pid_t child_pgid = pgid == 0 ? ::getpid() : pgid;
            if (::setpgid(0, child_pgid) == -1) {
                ::_exit(1);
            }

            JobControl::reset_child_signal_handlers();
            configure_child_io(command, i, pipeline.commands.size(), pipes);

            if (is_job_builtin(command.args[0])) {
                if (command.args[0] == "jobs") {
                    write_all(STDOUT_FILENO, jobs_.render_jobs());
                    ::_exit(0);
                }
                write_all(STDERR_FILENO,
                          command.args[0] + ": job control unavailable in child context\n");
                ::_exit(1);
            }

            if (command.args[0] == "run") {
                ::_exit(execute_resource_run_child(command, STDOUT_FILENO, STDERR_FILENO));
            }
            if (command.args[0] == "sandbox") {
                ::_exit(execute_sandbox_child(command, STDOUT_FILENO, STDERR_FILENO));
            }
            if (command.args[0] == "trace") {
                ::_exit(execute_trace_child(command, STDOUT_FILENO, STDERR_FILENO));
            }

            if (is_builtin(command.args[0])) {
                const BuiltinResult r = execute_builtin(
                    command, STDOUT_FILENO, STDERR_FILENO, BuiltinContext::ChildProcess);
                ::_exit(r.exit_requested ? r.exit_code : r.status);
            }

            exec_external_child(command);
        }

        if (pgid == 0) {
            pgid = pid;
        }
        if (::setpgid(pid, pgid) == -1 && errno != EACCES && errno != ESRCH) {
            std::cerr << "Failed to assign process group\n";
        }
        pids.push_back(pid);
    }

    close_all_pipes(pipes);
    if (pids.empty()) {
        return {false, 0, 1};
    }

    const std::string description = format_pipeline(pipeline);
    const int job_id = jobs_.add_job(pgid, pids, pids.back(), description);

    if (pipeline.background) {
        write_all(STDOUT_FILENO,
                  "[" + std::to_string(job_id) + "] " + std::to_string(pgid) + "\n");
        return {false, 0, 0};
    }

    const int status = jobs_.wait_foreground(job_id, false);
    return {false, 0, status};
}

}  // namespace shell
