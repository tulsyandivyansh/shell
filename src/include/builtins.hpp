#pragma once

#include "command.hpp"

namespace shell {

enum class BuiltinContext {
    ParentShell,
    ChildProcess,
};

struct BuiltinResult {
    bool handled{false};
    bool exit_requested{false};
    int exit_code{0};
    int status{0};
};

BuiltinResult execute_builtin(
    const Command& command,
    int output_fd,
    int error_fd,
    BuiltinContext context);

}  // namespace shell
