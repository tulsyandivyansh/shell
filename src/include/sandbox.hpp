#pragma once

#include "command.hpp"

#include <optional>
#include <string>
#include <vector>

namespace shell {

struct SandboxConfig {
    bool show_help{false};
    bool show_status{false};
    bool user_namespace{false};
    bool pid_namespace{false};
    bool mount_namespace{false};
    bool network_namespace{false};
    bool uts_namespace{false};
    bool mount_proc{false};
    std::string hostname;

    bool has_memory_max{false};
    unsigned long long memory_max_bytes{0};
    bool has_pids_max{false};
    unsigned long long pids_max{0};
    bool has_cpu_percent{false};
    unsigned int cpu_percent{0};

    std::vector<std::string> program_args;
};

struct SandboxParseResult {
    SandboxConfig config;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

struct IsolationCapabilities {
    bool linux_namespaces{false};
    bool user_namespace_available{false};
    bool pid_namespace_available{false};
    bool mount_namespace_available{false};
    bool network_namespace_available{false};
    bool uts_namespace_available{false};
    bool cgroup_v2{false};
    bool cgroup_writable{false};
    std::string cgroup_root;
};

SandboxParseResult parse_sandbox(const Command& command);
std::string sandbox_usage();
IsolationCapabilities detect_isolation_capabilities();
std::string render_isolation_status();

// Runs PROGRAM under the requested namespace/cgroup isolation. This function is
// intended to execute inside the shell's already-forked job child, so the
// interactive parent shell is never moved into a namespace or cgroup.
int execute_sandbox_child(const Command& command, int output_fd, int error_fd);

}  // namespace shell
