#include "include/job_control.hpp"

#include "include/shell_utils.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace shell {
namespace {

volatile sig_atomic_t sigchld_pending = 0;

extern "C" void on_sigchld(int) {
    sigchld_pending = 1;
}

void install_handler(int signal_number, void (*handler)(int), int flags = 0) {
    struct sigaction action {};
    action.sa_handler = handler;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = flags;
    ::sigaction(signal_number, &action, nullptr);
}

void ignore_signal(int signal_number) {
    install_handler(signal_number, SIG_IGN);
}

void default_signal(int signal_number) {
    install_handler(signal_number, SIG_DFL);
}

int decode_wait_status(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    if (WIFSTOPPED(status)) {
        return 128 + WSTOPSIG(status);
    }
    return 0;
}

const char* state_name(JobState state) {
    switch (state) {
        case JobState::Running:
            return "Running";
        case JobState::Stopped:
            return "Stopped";
        case JobState::Done:
            return "Done";
    }
    return "Unknown";
}

std::optional<int> parse_job_id(const std::string& spec) {
    if (spec.empty() || spec == "%+") {
        return std::nullopt;
    }

    std::string text = spec;
    if (!text.empty() && text.front() == '%') {
        text.erase(text.begin());
    }
    if (text.empty()) {
        return std::nullopt;
    }

    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value <= 0) {
        return -1;
    }
    return static_cast<int>(value);
}

}  // namespace

JobControl::JobControl() {
    terminal_fd_ = STDIN_FILENO;
    interactive_ = ::isatty(terminal_fd_) != 0;

    struct sigaction chld_action {};
    chld_action.sa_handler = on_sigchld;
    ::sigemptyset(&chld_action.sa_mask);
    chld_action.sa_flags = 0;
    ::sigaction(SIGCHLD, &chld_action, nullptr);

    if (!interactive_) {
        return;
    }

    shell_pgid_ = ::getpgrp();
    while (::tcgetpgrp(terminal_fd_) != shell_pgid_) {
        ::kill(-shell_pgid_, SIGTTIN);
        shell_pgid_ = ::getpgrp();
    }

    // The interactive shell itself must not be suspended or terminated by
    // terminal-generated job-control signals. Foreground jobs receive these
    // signals directly because we hand them terminal ownership with tcsetpgrp.
    ignore_signal(SIGINT);
    ignore_signal(SIGQUIT);
    ignore_signal(SIGTSTP);
    ignore_signal(SIGTTIN);
    ignore_signal(SIGTTOU);

    const pid_t shell_pid = ::getpid();
    if (::setpgid(shell_pid, shell_pid) == -1 && errno != EACCES && errno != EPERM) {
        // Non-fatal: the shell may already be its own process-group leader.
    }
    shell_pgid_ = ::getpgrp();
    ::tcsetpgrp(terminal_fd_, shell_pgid_);
    ::tcgetattr(terminal_fd_, &shell_terminal_modes_);
}

void JobControl::reset_child_signal_handlers() {
    default_signal(SIGINT);
    default_signal(SIGQUIT);
    default_signal(SIGTSTP);
    default_signal(SIGTTIN);
    default_signal(SIGTTOU);
    default_signal(SIGCHLD);
}

int JobControl::add_job(pid_t pgid,
                        const std::vector<pid_t>& pids,
                        pid_t last_pid,
                        std::string command) {
    Job job;
    job.id = next_job_id_++;
    job.pgid = pgid;
    job.last_pid = last_pid;
    job.command = std::move(command);
    job.processes.reserve(pids.size());
    for (pid_t pid : pids) {
        job.processes.push_back(ProcessRecord{pid, ProcessState::Running, 0});
    }
    jobs_.push_back(std::move(job));
    return jobs_.back().id;
}

Job* JobControl::find_job(int id) {
    const auto it = std::find_if(jobs_.begin(), jobs_.end(),
                                 [id](const Job& job) { return job.id == id; });
    return it == jobs_.end() ? nullptr : &*it;
}

const Job* JobControl::find_job(int id) const {
    const auto it = std::find_if(jobs_.begin(), jobs_.end(),
                                 [id](const Job& job) { return job.id == id; });
    return it == jobs_.end() ? nullptr : &*it;
}

Job* JobControl::resolve_job(const std::string& spec) {
    const std::optional<int> parsed = parse_job_id(spec);
    if (parsed.has_value()) {
        if (*parsed < 0) {
            return nullptr;
        }
        return find_job(*parsed);
    }

    for (auto it = jobs_.rbegin(); it != jobs_.rend(); ++it) {
        if (it->state != JobState::Done) {
            return &*it;
        }
    }
    return nullptr;
}

void JobControl::recompute_state(Job& job) {
    const JobState old_state = job.state;
    bool any_running = false;
    bool any_stopped = false;
    bool all_done = true;

    for (const ProcessRecord& process : job.processes) {
        if (process.state == ProcessState::Running) {
            any_running = true;
            all_done = false;
        } else if (process.state == ProcessState::Stopped) {
            any_stopped = true;
            all_done = false;
        }
    }

    if (all_done) {
        job.state = JobState::Done;
    } else if (any_running) {
        job.state = JobState::Running;
    } else if (any_stopped) {
        job.state = JobState::Stopped;
    }

    if (job.state != old_state) {
        job.state_changed = true;
    }
}

void JobControl::update_process(pid_t pid, int status) {
    for (Job& job : jobs_) {
        auto it = std::find_if(job.processes.begin(), job.processes.end(),
                               [pid](const ProcessRecord& process) {
                                   return process.pid == pid;
                               });
        if (it == job.processes.end()) {
            continue;
        }

        it->wait_status = status;
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            it->state = ProcessState::Done;
        } else if (WIFSTOPPED(status)) {
            it->state = ProcessState::Stopped;
        } else if (WIFCONTINUED(status)) {
            it->state = ProcessState::Running;
        }

        if (pid == job.last_pid && !WIFCONTINUED(status)) {
            job.last_status = decode_wait_status(status);
        }
        recompute_state(job);
        return;
    }
}

void JobControl::give_terminal_to(pid_t pgid, const termios* modes) {
    if (!interactive_) {
        return;
    }
    ::tcsetpgrp(terminal_fd_, pgid);
    if (modes != nullptr) {
        ::tcsetattr(terminal_fd_, TCSADRAIN, modes);
    }
}

void JobControl::reclaim_terminal() {
    if (!interactive_) {
        return;
    }
    ::tcsetpgrp(terminal_fd_, shell_pgid_);
    ::tcsetattr(terminal_fd_, TCSADRAIN, &shell_terminal_modes_);
}

int JobControl::wait_foreground(int job_id, bool continue_job) {
    Job* job = find_job(job_id);
    if (job == nullptr) {
        return 1;
    }

    give_terminal_to(job->pgid, job->has_terminal_modes ? &job->terminal_modes : nullptr);

    if (continue_job) {
        for (ProcessRecord& process : job->processes) {
            if (process.state == ProcessState::Stopped) {
                process.state = ProcessState::Running;
            }
        }
        job->state = JobState::Running;
        job->state_changed = false;
        if (::kill(-job->pgid, SIGCONT) == -1) {
            reclaim_terminal();
            return 1;
        }
    }

    while (job->state == JobState::Running) {
        int status = 0;
        const pid_t pid = ::waitpid(-job->pgid, &status, WUNTRACED | WCONTINUED);
        if (pid > 0) {
            update_process(pid, status);
            job = find_job(job_id);
            if (job == nullptr) {
                break;
            }
            continue;
        }
        if (pid == -1 && errno == EINTR) {
            continue;
        }
        if (pid == -1 && errno == ECHILD) {
            break;
        }
        if (pid == -1) {
            break;
        }
    }

    job = find_job(job_id);
    if (job != nullptr && interactive_ && job->state == JobState::Stopped) {
        if (::tcgetattr(terminal_fd_, &job->terminal_modes) == 0) {
            job->has_terminal_modes = true;
        }
    }
    reclaim_terminal();

    job = find_job(job_id);
    if (job == nullptr) {
        return 1;
    }

    const int status = job->state == JobState::Stopped
        ? (job->last_status != 0 ? job->last_status : 128 + SIGTSTP)
        : job->last_status;

    if (job->state == JobState::Stopped) {
        write_all(STDOUT_FILENO,
                  "[" + std::to_string(job->id) + "] Stopped\t" + job->command + "\n");
        job->state_changed = false;
        return status;
    }

    if (job->state == JobState::Done) {
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                                   [job_id](const Job& item) { return item.id == job_id; }),
                    jobs_.end());
    }
    return status;
}

int JobControl::foreground(const std::string& spec, int output_fd, int error_fd) {
    reap_background(false);
    Job* job = resolve_job(spec);
    if (job == nullptr || job->state == JobState::Done) {
        write_all(error_fd, "fg: no such job\n");
        return 1;
    }

    const int id = job->id;
    const bool should_continue = job->state == JobState::Stopped;
    write_all(output_fd, job->command + "\n");
    return wait_foreground(id, should_continue);
}

int JobControl::continue_background(const std::string& spec, int output_fd, int error_fd) {
    reap_background(false);
    Job* job = resolve_job(spec);
    if (job == nullptr || job->state == JobState::Done) {
        write_all(error_fd, "bg: no such job\n");
        return 1;
    }

    if (job->state == JobState::Stopped) {
        if (::kill(-job->pgid, SIGCONT) == -1) {
            write_all(error_fd, "bg: failed to continue job\n");
            return 1;
        }
        for (ProcessRecord& process : job->processes) {
            if (process.state == ProcessState::Stopped) {
                process.state = ProcessState::Running;
            }
        }
        job->state = JobState::Running;
        job->state_changed = false;
    }

    write_all(output_fd,
              "[" + std::to_string(job->id) + "] " + job->command + " &\n");
    return 0;
}

void JobControl::notify_and_cleanup_done_jobs() {
    std::vector<int> done_ids;
    for (Job& job : jobs_) {
        if (!job.state_changed) {
            continue;
        }
        if (job.state == JobState::Done) {
            write_all(STDOUT_FILENO,
                      "[" + std::to_string(job.id) + "] Done\t" + job.command + "\n");
            done_ids.push_back(job.id);
        } else if (job.state == JobState::Stopped) {
            write_all(STDOUT_FILENO,
                      "[" + std::to_string(job.id) + "] Stopped\t" + job.command + "\n");
            job.state_changed = false;
        } else {
            job.state_changed = false;
        }
    }

    if (!done_ids.empty()) {
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                                   [&](const Job& job) {
                                       return std::find(done_ids.begin(), done_ids.end(), job.id) != done_ids.end();
                                   }),
                    jobs_.end());
    }
}

void JobControl::reap_background(bool notify) {
    // The async handler only flips a sig_atomic_t flag. All waitpid calls and
    // C++ job-table mutation happen here at a normal execution safe point.
    if (sigchld_pending != 0) {
        sigchld_pending = 0;
        while (true) {
            int status = 0;
            const pid_t pid = ::waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
            if (pid > 0) {
                update_process(pid, status);
                continue;
            }
            if (pid == -1 && errno == EINTR) {
                continue;
            }
            break;
        }
    }

    if (notify) {
        notify_and_cleanup_done_jobs();
    }
}

std::string JobControl::render_jobs() const {
    std::string output;
    int current_id = -1;
    for (auto it = jobs_.rbegin(); it != jobs_.rend(); ++it) {
        if (it->state != JobState::Done) {
            current_id = it->id;
            break;
        }
    }

    for (const Job& job : jobs_) {
        if (job.state == JobState::Done) {
            continue;
        }
        output += "[" + std::to_string(job.id) + "]";
        output += job.id == current_id ? "+ " : "  ";
        output += state_name(job.state);
        output += "\t";
        output += job.command;
        output += "\n";
    }
    return output;
}

}  // namespace shell
