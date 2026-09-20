#include "include/trace.hpp"

#include <cassert>

using shell::Command;
using shell::parse_trace;

int main() {
    {
        Command c;
        c.args = {"trace", "--", "/bin/echo", "hello"};
        auto r = parse_trace(c);
        assert(r.ok());
        assert(!r.config.summary);
        assert(r.config.program_args.size() == 2);
        assert(r.config.program_args[0] == "/bin/echo");
    }
    {
        Command c;
        c.args = {"trace", "-c", "--", "/bin/true"};
        auto r = parse_trace(c);
        assert(r.ok());
        assert(r.config.summary);
    }
    {
        Command c;
        c.args = {"trace", "/bin/true"};
        auto r = parse_trace(c);
        assert(!r.ok());
    }
    {
        Command c;
        c.args = {"trace", "--"};
        auto r = parse_trace(c);
        assert(!r.ok());
    }
    {
        Command c;
        c.args = {"trace", "--help"};
        auto r = parse_trace(c);
        assert(r.ok());
        assert(r.config.show_help);
    }
    return 0;
}
