#pragma once

#include <optional>
#include <string>
#include <sys/types.h>
#include <termios.h>
#include <vector>

namespace shell {

enum class ProcessState {
    Running,
    Stopped,
    Done,
};

enum class JobState {
    Running,
    Stopped,
    Done,
};

struct ProcessRecord {
    pid_t pid{-1};
    ProcessState state{ProcessState::Running};
    int wait_status{0};
};

struct Job {
    int id{0};
    pid_t pgid{-1};
    std::vector<ProcessRecord> processes;
    pid_t last_pid{-1};
    std::string command;
    JobState state{JobState::Running};
    int last_status{0};
    bool state_changed{false};
    termios terminal_modes{};
    bool has_terminal_modes{false};
};

class JobControl {
public:
    JobControl();

    [[nodiscard]] bool interactive() const noexcept { return interactive_; }

    int add_job(pid_t pgid,
                const std::vector<pid_t>& pids,
                pid_t last_pid,
                std::string command);

    int wait_foreground(int job_id, bool continue_job);
    int continue_background(const std::string& spec, int output_fd, int error_fd);
    int foreground(const std::string& spec, int output_fd, int error_fd);

    void reap_background(bool notify);
    std::string render_jobs() const;

    static void reset_child_signal_handlers();

private:
    Job* find_job(int id);
    const Job* find_job(int id) const;
    Job* resolve_job(const std::string& spec);
    void update_process(pid_t pid, int status);
    void recompute_state(Job& job);
    void give_terminal_to(pid_t pgid, const termios* modes = nullptr);
    void reclaim_terminal();
    void notify_and_cleanup_done_jobs();

    bool interactive_{false};
    int terminal_fd_{0};
    pid_t shell_pgid_{-1};
    termios shell_terminal_modes_{};
    int next_job_id_{1};
    std::vector<Job> jobs_;
};

}  // namespace shell
