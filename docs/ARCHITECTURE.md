# Architecture

## Overview

The project is organized around one rule: parsing describes *what* to execute, while the executor/job-control layers decide *how* Linux processes are created and managed.

```text
                         interactive input
                                |
                                v
                         +--------------+
                         |   Readline   |
                         +------+-------+
                                |
                                v
+-----------+   tokens   +------+-------+   plan   +----------------+
|   Lexer   +----------->|    Parser    +--------->|    Executor    |
+-----------+            +--------------+          +-------+--------+
                                                         |
                 +----------------+----------------------+----------------+
                 |                |                      |                |
                 v                v                      v                v
          +-------------+   +-----------+         +-------------+   +-----------+
          |  Built-ins  |   | JobControl|         | ProcessInfo |   |  Runtime  |
          +-------------+   +-----+-----+         +------+------+   | extensions|
                                  |                      |          +-----+-----+
                                  v                      v                |
                        process groups / tty         /proc/<pid>           |
                                  |                                       |
                                  +------------------+--------------------+
                                                     |
                                                     v
                                             Linux kernel APIs
```

## Parsing layer

The lexer recognizes words, quotes/escapes, pipes, list operators, background execution, and redirections. The parser converts tokens into `ExecutionPlan -> PipelineStep -> Pipeline -> Command` structures. It never forks or performs I/O.

This separation makes malformed input testable without launching processes and gives the parser a clean fuzzing boundary.

## Executor

The executor applies shell control-flow semantics (`&&`, `||`, `;`), constructs pipes/redirections, and launches pipeline stages. File descriptors are owned through the move-only `UniqueFd` RAII wrapper so parent and child paths have explicit ownership.

Stateful built-ins execute in the parent only when semantics require it. Built-ins in pipelines/background jobs run in children so commands such as `cd / | cat` cannot mutate the parent shell.

## Job control

Each launched pipeline is one process group. `JobControl` tracks the job's processes and running/stopped/done state, transfers terminal ownership with `tcsetpgrp()`, and handles foreground/background transitions.

The asynchronous `SIGCHLD` handler performs only signal-safe notification; normal C++ state is updated later from the main execution path/event hook.

## Linux introspection and controls

`ProcessInfo` reads `/proc/<pid>` directly for process metadata. `ResourceControl` applies `setrlimit()` and priority changes only in the child before `exec`, preventing constraints from leaking into the long-lived shell.

`Sandbox` layers Linux namespaces and delegated cgroup-v2 controls. Its capability checks are explicit because namespace and cgroup availability depends on kernel/security policy.

## Syscall tracing

`Trace` runs the target under `ptrace`, uses syscall stops to render entry/exit information, and preserves the tracee's real exit status. Trace diagnostics go to stderr so traced stdout remains usable in pipelines.

## Hardening boundary

The lexer/parser is fuzzed independently of process creation. End-to-end stress tests then exercise the lifecycle boundary where unit tests are weakest: repeated `fork`, `pipe`, process-group creation, asynchronous child exit, and descriptor cleanup.
