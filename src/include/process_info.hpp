#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>

namespace shell {

struct ProcessInfo {
    pid_t pid{-1};
    pid_t ppid{-1};
    pid_t pgid{-1};
    pid_t session_id{-1};
    pid_t foreground_pgid{-1};

    std::string name;
    std::string state;
    std::string command_line;
    std::string executable;
    std::string cpu_allowed_list;

    long threads{0};
    long priority{0};
    long nice_value{0};

    double user_cpu_seconds{0.0};
    double system_cpu_seconds{0.0};

    std::optional<std::uint64_t> virtual_memory_kb;
    std::optional<std::uint64_t> resident_memory_kb;
    std::optional<std::uint64_t> peak_memory_kb;

    std::optional<std::size_t> open_file_descriptors;
    std::optional<std::uint64_t> voluntary_context_switches;
    std::optional<std::uint64_t> involuntary_context_switches;
};

struct ProcessInfoResult {
    std::optional<ProcessInfo> info;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return info.has_value(); }
};

ProcessInfoResult inspect_process(pid_t pid);
std::string render_process_info(const ProcessInfo& info);

}  // namespace shell
