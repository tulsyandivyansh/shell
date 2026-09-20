#!/usr/bin/env bash
set -euo pipefail

SHELL_BIN="${1:?usage: resource_control_regression.sh /path/to/shell}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

run_shell() {
  local input="$1"
  printf '%s' "$input" | "$SHELL_BIN" 2>"$TMP/stderr"
}

# limits introspection exposes the resources managed by the runtime.
out="$(run_shell $'limits\nexit\n')"
grep -q 'address-space: soft=' <<<"$out"
grep -q 'open-files: soft=' <<<"$out"
grep -q 'cpu-time: soft=' <<<"$out"
grep -q 'core-size: soft=' <<<"$out"
grep -q 'nice: ' <<<"$out"

# Each soft limit is visible to the exec'd process.
out="$(run_shell $'run --max-fds 32 -- /bin/sh -c "ulimit -n"\nrun --max-memory 64M -- /bin/sh -c "ulimit -v"\nrun --cpu-time 2 -- /bin/sh -c "ulimit -t"\nrun --core-size 0 -- /bin/sh -c "ulimit -c"\nexit\n')"
grep -qx '32' <<<"$(grep -E '^[0-9]+$' <<<"$out" | sed -n '1p')"
grep -qx '65536' <<<"$(grep -E '^[0-9]+$' <<<"$out" | sed -n '2p')"
grep -qx '2' <<<"$(grep -E '^[0-9]+$' <<<"$out" | sed -n '3p')"
grep -qx '0' <<<"$(grep -E '^[0-9]+$' <<<"$out" | sed -n '4p')"

# RLIMIT_NPROC is inherited (enforcement is UID/platform-dependent, especially as root).
out="$(run_shell $'run --max-procs 16 -- /bin/cat /proc/self/limits\nexit\n')"
grep -E 'Max processes[[:space:]]+16[[:space:]]+' <<<"$out" >/dev/null

# Nice priority is applied in the child only.
out="$(run_shell $'run --nice 10 -- /bin/sh -c "ps -o ni= -p $$"\nexit\n')"
grep -E '(^|[[:space:]])10([[:space:]]|$)' <<<"$out" >/dev/null

# `run` composes with pipelines and shell status chaining.
out="$(run_shell $'run --max-fds 32 -- /bin/sh -c "ulimit -n" | cat\nrun --max-fds 32 -- definitely_not_a_command || echo recovered\nexit\n')"
grep -q '^32$' <<<"$out"
grep -q '^recovered$' <<<"$out"

# Child limits must not leak into the parent shell or later children.
out="$(run_shell $'/bin/sh -c "ulimit -n"\nrun --max-fds 32 -- /bin/sh -c "ulimit -n"\n/bin/sh -c "ulimit -n"\nexit\n')"
nums="$(grep -E '^[0-9]+$' <<<"$out")"
first="$(sed -n '1p' <<<"$nums")"
second="$(sed -n '2p' <<<"$nums")"
third="$(sed -n '3p' <<<"$nums")"
[[ "$second" == "32" ]]
[[ "$first" == "$third" ]]
[[ "$first" != "32" ]]

# Invalid resource specifications fail cleanly and participate in `||`.
out="$(run_shell $'run --max-memory nope -- /bin/true || echo bad-memory\nrun --max-fds 0 -- /bin/true || echo bad-fds\nrun --nice 99 -- /bin/true || echo bad-nice\nexit\n')"
grep -q '^bad-memory$' <<<"$out"
grep -q '^bad-fds$' <<<"$out"
grep -q '^bad-nice$' <<<"$out"

# A request above the current hard FD limit is rejected rather than changing the hard limit.
out="$(run_shell $'run --max-fds 999999999 -- /bin/true || echo hard-limit-protected\nexit\n')"
grep -q '^hard-limit-protected$' <<<"$out"

echo 'Resource-control regression tests passed.'
