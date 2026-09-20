#pragma once

#include "command.hpp"

#include <string>
#include <vector>

namespace shell {

struct TraceConfig {
    bool show_help{false};
    bool summary{false};
    std::vector<std::string> program_args;
};

struct TraceParseResult {
    TraceConfig config;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

TraceParseResult parse_trace(const Command& command);
std::string trace_usage();

// Executes PROGRAM under ptrace. Intended to run inside the shell's already
// forked command child, so tracing never changes the long-lived shell process.
int execute_trace_child(const Command& command, int output_fd, int error_fd);

}  // namespace shell
