#include "include/sandbox.hpp"

#include <cassert>
#include <string>

using shell::Command;
using shell::parse_sandbox;

int main() {
    {
        Command c;
        c.args = {"sandbox", "--user", "--pid", "--mount", "--proc", "--hostname", "box", "--", "/bin/true"};
        auto r = parse_sandbox(c);
        assert(r.ok());
        assert(r.config.user_namespace);
        assert(r.config.pid_namespace);
        assert(r.config.mount_namespace);
        assert(r.config.mount_proc);
        assert(r.config.uts_namespace);
        assert(r.config.hostname == "box");
        assert(r.config.program_args.size() == 1);
        assert(r.config.program_args[0] == "/bin/true");
    }
    {
        Command c;
        c.args = {"sandbox", "--memory", "128M", "--pids", "32", "--cpu-percent", "150", "--", "/bin/true"};
        auto r = parse_sandbox(c);
        assert(r.ok());
        assert(r.config.has_memory_max && r.config.memory_max_bytes == 128ULL * 1024ULL * 1024ULL);
        assert(r.config.has_pids_max && r.config.pids_max == 32);
        assert(r.config.has_cpu_percent && r.config.cpu_percent == 150);
    }
    {
        Command c;
        c.args = {"sandbox", "--proc", "--", "/bin/true"};
        auto r = parse_sandbox(c);
        assert(!r.ok());
    }
    {
        Command c;
        c.args = {"sandbox", "--cpu-percent", "0", "--", "/bin/true"};
        auto r = parse_sandbox(c);
        assert(!r.ok());
    }
    {
        Command c;
        c.args = {"sandbox", "--all", "--", "/bin/echo", "ok"};
        auto r = parse_sandbox(c);
        assert(r.ok());
        assert(r.config.user_namespace && r.config.pid_namespace && r.config.mount_namespace);
        assert(r.config.network_namespace && r.config.uts_namespace && r.config.mount_proc);
    }
    return 0;
}
