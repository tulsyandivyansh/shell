# Unix Shell & Linux Process Runtime

A C++23 Linux shell that grew into a systems-programming runtime for exploring command parsing, POSIX process semantics, terminal job control, `/proc`, resource limits, namespaces/cgroups, and `ptrace`.

This project explores how a Unix shell can coordinate processes and expose Linux operating-system facilities through a focused, modular C++ implementation.

> **Platform:** Linux. Several features intentionally depend on Linux-specific interfaces such as procfs, cgroups v2, namespaces, and `ptrace`.

## What it demonstrates

| Area | Implementation |
| --- | --- |
| Modern C++ | C++23, modular components, move-only RAII file descriptors, explicit ownership |
| Parsing | custom lexer/parser, quoting/escaping, pipelines, `&&`, `||`, `;`, `&`, redirections |
| Processes / IPC | `fork`, `exec`, `pipe`, `dup2`, `waitpid`, multi-stage pipelines |
| Unix job control | process groups, `tcsetpgrp`, `SIGCHLD`, `SIGINT`, `SIGTSTP`, `jobs`, `fg`, `bg` |
| Linux observability | direct `/proc/<pid>` parsing and live process inspection |
| Resource control | `setrlimit`, `getrlimit`, `setpriority`, child-only execution limits |
| Isolation | user/PID/mount/network/UTS namespaces and capability-aware cgroup-v2 controls |
| Debugging | syscall entry/exit tracing and summaries with `ptrace` |
| Reliability | PTY integration tests, lifecycle stress tests, parser fuzzing, ASan/UBSan, CI |

## Example session

```console
$ echo hello | tr a-z A-Z
HELLO

$ sleep 30 &
[1] 18472
$ jobs
[1]+ Running    sleep 30
$ fg %1
^Z
[1]+ Stopped    sleep 30
$ bg %1

$ pinfo 18472
Process 18472 (sleep)
...

$ run --max-memory 64M --max-fds 32 -- /bin/sh -c 'ulimit -n'
32

$ sandbox --user --pid --mount --proc --hostname demo -- /bin/sh

$ trace --summary -- /bin/true
Syscall summary
...
```

Availability of namespace and cgroup operations depends on the host's kernel/security policy. `sandbox --status` reports what the current environment actually permits; restricted capabilities are surfaced rather than silently ignored.

## Architecture

```text
Readline
   |
   v
Lexer -> Parser -> ExecutionPlan -> Executor
                                  |
          +-----------------------+------------------------+
          |                       |                        |
          v                       v                        v
      Built-ins              JobControl             Linux extensions
                                  |                        |
                           process groups        +---------+---------+
                           terminal / signals    |         |         |
                                                v         v         v
                                              /proc    sandbox    ptrace
                                                |    namespaces/    |
                                                |      cgroups      |
                                                +---------+---------+
                                                          |
                                                          v
                                                    Linux kernel
```

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the component boundaries and design decisions.

## Core commands

Alongside external programs and normal shell operators, the runtime includes:

- `jobs`, `fg`, `bg` — job control
- `pinfo [PID]` / `pinfo --watch ...` — `/proc`-based process inspection
- `limits` — current soft/hard resource limits
- `run [limits] -- PROGRAM ...` — child-only `setrlimit`/priority controls
- `sandbox [options] -- PROGRAM ...` — namespace/cgroup isolation
- `sandbox --status` — runtime capability report
- `trace -- PROGRAM ...` — live syscall tracing
- `trace --summary -- PROGRAM ...` — syscall count/error summary

Built-ins also include `echo`, `pwd`, `cd`, `type`, `history`, and `exit`.

## Build

Dependencies:

- Linux
- C++23 compiler (GCC or Clang)
- CMake 3.20+
- GNU Readline development headers/library
- Ninja for the supplied presets
- Python 3 for PTY/stress integration tests

On Ubuntu/Debian:

```sh
sudo apt-get install build-essential cmake ninja-build libreadline-dev python3
```

Then:

```sh
cmake --preset dev
cmake --build --preset dev
./build/dev/shell
```

A conventional build also works:

```sh
cmake -S . -B build
cmake --build build
./build/shell
```

## Tests and hardening

Run the complete test suite:

```sh
ctest --preset dev
```

The final suite covers:

- component smoke tests
- capability-focused shell regression suites
- real pseudo-terminal tests for job control / `Ctrl+C` / `Ctrl+Z`
- `pinfo --watch` and trace terminal integration
- a deterministic **30,000-input parser fuzz smoke test**
- a lifecycle stress test with hundreds of forks/pipelines/background jobs
- file-descriptor stability and child-reaping checks through `/proc`

Sanitizer build:

```sh
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
```

Longer parser fuzzing with Clang/libFuzzer:

```sh
CXX=clang++ cmake --preset fuzz
cmake --build --preset fuzz
./build/fuzz/parser_fuzzer -runs=100000 -max_len=1024
```

See [`docs/TESTING.md`](docs/TESTING.md) for details.

## CI

GitHub Actions runs:

- GCC + Clang with warnings treated as errors
- the complete CTest suite
- a Clang ASan/UBSan build
- a bounded libFuzzer campaign

The CMake warning set includes `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`.

## Deliberate limitations

This is a systems-engineering project rather than a drop-in Bash replacement. It intentionally does not attempt full POSIX/Bash expansion semantics (variables, globbing, command substitution, here-documents, functions, scripting language features), a complete container runtime, or a complete `strace` implementation.

Those boundaries keep the code focused on the operating-system mechanisms the project is designed to demonstrate.
