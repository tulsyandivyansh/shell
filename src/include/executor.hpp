#pragma once

#include "command.hpp"
#include "job_control.hpp"

namespace shell {

struct ExecutionResult {
    bool exit_requested{false};
    int exit_code{0};
    int status{0};
};

class Executor {
public:
    Executor() = default;

    ExecutionResult execute(const ExecutionPlan& plan);
    void reap_background_jobs(bool notify = true);

private:
    ExecutionResult execute_pipeline(const Pipeline& pipeline);
    ExecutionResult execute_parent_builtin(const Command& command);
    int execute_job_builtin(const Command& command, int output_fd, int error_fd);

    JobControl jobs_;
};

}  // namespace shell
