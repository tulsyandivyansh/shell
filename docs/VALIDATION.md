# Validation snapshot

This snapshot records the verification performed for the current project state.

## Standard / warnings-as-errors

- GCC 14 build with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`: passed
- Clang 17 build with the same warning policy and `-Werror`: passed
- CTest: **17 / 17 passed**

The 17 tests include smoke tests, capability-focused regression suites, three PTY integration tests, the deterministic parser fuzz smoke test, and lifecycle stress coverage.

## Sanitizers

Clang 17 with AddressSanitizer + UndefinedBehaviorSanitizer:

```text
17 / 17 tests passed
```

The sanitizer run included the PTY and stress suites.

## libFuzzer

A bounded parser campaign was run with:

```sh
./parser_fuzzer -runs=20000 -max_len=512
```

Result: 20,000 executions completed without a crash or sanitizer finding.

## Host-dependent isolation

The validation environment supported namespace testing but did not provide a writable delegated cgroup-v2 subtree. Accordingly, cgroup tests validated the explicit unavailable/failure path rather than claiming enforcement that the host could not provide.
