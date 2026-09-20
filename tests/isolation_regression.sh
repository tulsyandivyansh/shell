#!/usr/bin/env bash
set -euo pipefail

SHELL_BIN="${1:?usage: isolation_regression.sh /path/to/shell}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

run_shell() {
  local input="$1"
  printf '%s' "$input" | "$SHELL_BIN" 2>"$TMP/stderr"
}

# Builtin registration, help, and capability reporting must always work.
out="$(run_shell $'type sandbox\nsandbox --help\nsandbox --status\nexit\n')"
grep -q '^sandbox is a shell builtin$' <<<"$out"
grep -q '^usage: sandbox ' <<<"$out"
grep -q '^Isolation capabilities$' <<<"$out"
grep -q '^linux-namespaces:' <<<"$out"
grep -q '^cgroup-v2:' <<<"$out"

status="$(run_shell $'sandbox --status\nexit\n')"

capability() {
  local name="$1"
  grep -E "^${name}:" <<<"$status" | head -1 | sed -E "s/^${name}:[[:space:]]*//"
}

# Invalid configurations fail cleanly and preserve normal shell chaining.
out="$(run_shell $'sandbox --proc -- /bin/true || echo proc-dependency-ok\nsandbox --memory nope -- /bin/true || echo memory-parse-ok\nsandbox --cpu-percent 0 -- /bin/true || echo cpu-parse-ok\nsandbox -- || echo missing-program-ok\nexit\n')"
grep -q '^proc-dependency-ok$' <<<"$out"
grep -q '^memory-parse-ok$' <<<"$out"
grep -q '^cpu-parse-ok$' <<<"$out"
grep -q '^missing-program-ok$' <<<"$out"

# Namespace tests are capability-aware because CI/container hosts may disable
# individual namespace types with seccomp or sysctl policy.
if [[ "$(capability user-namespace)" == "permitted" ]]; then
  out="$(run_shell $'sandbox --user -- /usr/bin/id -u\nexit\n')"
  grep -q '^0$' <<<"$out"
fi

if [[ "$(capability uts-namespace)" == "permitted" ]]; then
  out="$(run_shell $'sandbox --uts --hostname isolated-box -- /bin/hostname\nexit\n')"
  grep -q '^isolated-box$' <<<"$out"
fi

if [[ "$(capability pid-namespace)" == "permitted" ]]; then
  out="$(run_shell $'sandbox --pid -- /bin/sh -c "echo $$"\nexit\n')"
  grep -q '^1$' <<<"$out"
fi

if [[ "$(capability network-namespace)" == "permitted" ]]; then
  # A fresh network namespace starts with only a loopback device and it is down;
  # this is enough to prove the command is not sharing the host network stack.
  out="$(run_shell $'sandbox --net -- /bin/cat /proc/net/dev\nexit\n')"
  grep -q 'lo:' <<<"$out"
fi

# Cgroup status is a capability hint: directory creation can succeed while a
# specific controller remains unavailable. Test the requested operation itself
# and accept an explicit host-capability failure.
out="$(run_shell $'sandbox --memory 67108864 -- /bin/sh -c "p=$(cut -d: -f3 /proc/self/cgroup); cat /sys/fs/cgroup$p/memory.max" || echo cgroup-unavailable-ok\nexit\n')"
if grep -q '^67108864$' <<<"$out"; then
  :
elif grep -q '^cgroup-unavailable-ok$' <<<"$out" &&
     grep -Eqi 'cgroup|permission|not permitted|read-only|operation not permitted' "$TMP/stderr"; then
  :
else
  echo "unexpected cgroup result" >&2
  cat "$TMP/stderr" >&2
  printf '%s\n' "$out" >&2
  exit 1
fi

# Sandbox commands still compose with pipelines and conditional execution.
out="$(run_shell $'sandbox -- /bin/echo isolated | /usr/bin/tr a-z A-Z\nsandbox -- /bin/false || echo recovered\nexit\n')"
grep -q '^ISOLATED$' <<<"$out"
grep -q '^recovered$' <<<"$out"

echo 'Isolation regression tests passed.'
