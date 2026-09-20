#include "include/resource_control.hpp"

#include <cassert>
#include <string>

using shell::Command;
using shell::parse_resource_run;

int main() {
    {
        Command cmd;
        cmd.args = {"run", "--max-memory", "64M", "--max-fds", "32",
                    "--cpu-time", "2", "--core-size", "0", "--nice", "10",
                    "--", "/bin/echo", "hello"};
        const auto parsed = parse_resource_run(cmd);
        assert(parsed.ok());
        assert(parsed.config.has_max_memory);
        assert(parsed.config.max_memory_bytes == 64ULL * 1024ULL * 1024ULL);
        assert(parsed.config.has_max_fds && parsed.config.max_fds == 32);
        assert(parsed.config.has_cpu_time && parsed.config.cpu_time_seconds == 2);
        assert(parsed.config.has_core_size && parsed.config.core_size_bytes == 0);
        assert(parsed.config.has_nice && parsed.config.nice_value == 10);
        assert(parsed.config.program_args.size() == 2);
        assert(parsed.config.program_args[0] == "/bin/echo");
        assert(parsed.config.program_args[1] == "hello");
    }
    {
        Command cmd;
        cmd.args = {"run", "--max-memory", "1GiB", "/bin/true"};
        const auto parsed = parse_resource_run(cmd);
        assert(parsed.ok());
        assert(parsed.config.max_memory_bytes == 1024ULL * 1024ULL * 1024ULL);
    }
    {
        Command cmd;
        cmd.args = {"run", "--max-fds", "0", "/bin/true"};
        assert(!parse_resource_run(cmd).ok());
    }
    {
        Command cmd;
        cmd.args = {"run", "--nice", "20", "/bin/true"};
        assert(!parse_resource_run(cmd).ok());
    }
    {
        Command cmd;
        cmd.args = {"run", "--"};
        assert(!parse_resource_run(cmd).ok());
    }
    {
        Command cmd;
        cmd.args = {"run", "--help"};
        const auto parsed = parse_resource_run(cmd);
        assert(parsed.ok());
        assert(parsed.config.show_help);
    }
    return 0;
}
