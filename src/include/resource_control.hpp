#pragma once

#include "command.hpp"

#include <string>
#include <vector>

namespace shell {

struct ResourceRunConfig {
    bool show_help{false};
    bool has_max_memory{false};
    unsigned long long max_memory_bytes{0};
    bool has_max_fds{false};
    unsigned long long max_fds{0};
    bool has_cpu_time{false};
    unsigned long long cpu_time_seconds{0};
    bool has_max_procs{false};
    unsigned long long max_procs{0};
    bool has_core_size{false};
    unsigned long long core_size_bytes{0};
    bool has_nice{false};
    int nice_value{0};
    std::vector<std::string> program_args;
};

struct ResourceRunParseResult {
    ResourceRunConfig config;
    std::string error;

    [[nodiscard]] bool ok() const { return error.empty(); }
};

ResourceRunParseResult parse_resource_run(const Command& command);
std::string resource_run_usage();
std::string render_current_limits();

// Applies the requested limits and replaces the current child process with the
// target program. Returns only when validation/application/exec fails.
int execute_resource_run_child(const Command& command, int output_fd, int error_fd);

}  // namespace shell
