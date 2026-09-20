# Testing and verification

## Normal suite

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The suite contains four layers.

### Smoke tests

Small C++ executables validate parser structures and argument parsing for resource-control, sandbox, tracing, and `/proc` components.

### Regression tests

Shell scripts execute real command sequences and protect redirection, pipelines, conditional execution, job-control built-ins, process inspection, resource limits, isolation, and syscall tracing.

### PTY tests

Python pseudo-terminal tests validate behavior that redirected stdin cannot reproduce correctly:

- foreground terminal ownership
- `Ctrl+C` / `Ctrl+Z`
- `fg` / `bg`
- live `pinfo --watch`
- interrupting a traced process without killing the shell

### Hardening tests

`parser_fuzz_smoke` runs 30,000 deterministic generated inputs. `lifecycle_stress` repeats process and pipeline creation, checks FD stability through `/proc`, and verifies that background children are reaped.

## Sanitizers

```sh
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
```

This enables AddressSanitizer and UndefinedBehaviorSanitizer across the shell and test executables.

## libFuzzer

Requires Clang:

```sh
CXX=clang++ cmake --preset fuzz
cmake --build --preset fuzz
./build/fuzz/parser_fuzzer -runs=100000 -max_len=1024
```

A short bounded fuzz run is also executed in CI.

## Capability-dependent Linux tests

Namespaces, cgroups, and ptrace can be restricted by containers or CI security policies. Tests first detect the host's capabilities and distinguish an unavailable kernel permission from an implementation failure. They never claim cgroup enforcement occurred when the cgroup tree is read-only or not delegated.
