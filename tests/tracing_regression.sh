#!/usr/bin/env bash
set -euo pipefail

SHELL_BIN="${1:?usage: tracing_regression.sh /path/to/shell}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

run_shell() {
  local input="$1"
  printf '%s' "$input" | "$SHELL_BIN" 2>"$TMP/stderr"
}

# Registration/help and parser failures.
out="$(run_shell $'type trace\ntrace --help\ntrace /bin/true || echo missing-separator-ok\ntrace -- || echo missing-program-ok\nexit\n')"
grep -q '^trace is a shell builtin$' <<<"$out"
grep -q '^usage: trace ' <<<"$out"
grep -q '^missing-separator-ok$' <<<"$out"
grep -q '^missing-program-ok$' <<<"$out"

# A live trace writes tracing diagnostics to stderr while preserving target stdout.
printf 'trace -- /bin/echo hello\nexit\n' | "$SHELL_BIN" >"$TMP/live.out" 2>"$TMP/live.trace"
grep -q '^hello$' "$TMP/live.out"
grep -Eq 'execve\(|write\(|exit_group\(' "$TMP/live.trace"

# Program stdout remains usable as pipeline input.
out="$(run_shell $'trace -- /bin/echo pipeline | /usr/bin/tr a-z A-Z\nexit\n')"
grep -q '^PIPELINE$' <<<"$out"
grep -Eq 'execve\(|write\(' "$TMP/stderr"

# Summary mode reports syscall counts and errors without live per-call lines.
printf 'trace --summary -- /bin/true\nexit\n' | "$SHELL_BIN" >"$TMP/summary.out" 2>"$TMP/summary.trace"
grep -q '^Syscall summary$' "$TMP/summary.trace"
grep -q 'total' "$TMP/summary.trace"
grep -Eq 'execve|mmap|close|exit_group' "$TMP/summary.trace"

# Exit status comes from the traced program, so conditionals retain shell semantics.
out="$(run_shell $'trace -- /bin/false || echo recovered\ntrace -- /bin/true && echo success\nexit\n')"
grep -q '^recovered$' <<<"$out"
grep -q '^success$' <<<"$out"

# A missing target still reports the standard command-not-found status through trace.
out="$(run_shell $'trace -- definitely-not-a-real-command || echo missing-target-ok\nexit\n')"
grep -q '^missing-target-ok$' <<<"$out"
grep -q 'command not found' "$TMP/stderr"

echo 'Tracing regression tests passed.'
