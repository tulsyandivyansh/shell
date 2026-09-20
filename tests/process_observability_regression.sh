#!/usr/bin/env bash
set -euo pipefail

shell_bin="${1:?usage: process_observability_regression.sh /path/to/shell}"
tmpdir="$(mktemp -d)"
target=""
cleanup() {
    if [[ -n "$target" ]]; then
        kill "$target" 2>/dev/null || true
        wait "$target" 2>/dev/null || true
    fi
    rm -rf "$tmpdir"
}
trap cleanup EXIT

sleep 30 &
target=$!

stdout_file="$tmpdir/stdout.txt"
stderr_file="$tmpdir/stderr.txt"
redirected="$tmpdir/pinfo.txt"

cat >"$tmpdir/commands.txt" <<CMDS
type pinfo
pinfo $target
pinfo $target | grep "Process $target"
pinfo $target > "$redirected"
pinfo --watch $target --interval 10 --count 2
pinfo --count 2 $target || echo count-requires-watch
pinfo 99999999 || echo missing-ok
pinfo abc || echo invalid-ok
exit 0
CMDS

"$shell_bin" <"$tmpdir/commands.txt" >"$stdout_file" 2>"$stderr_file"

assert_stdout() {
    local expected="$1"
    if ! grep -Fq -- "$expected" "$stdout_file"; then
        echo "missing expected stdout: $expected" >&2
        echo "--- stdout ---" >&2
        cat "$stdout_file" >&2
        echo "--- stderr ---" >&2
        cat "$stderr_file" >&2
        exit 1
    fi
}

assert_stdout "pinfo is a shell builtin"
assert_stdout "Process $target (sleep)"
assert_stdout "Process group:"
assert_stdout "Threads:"
assert_stdout "CPU affinity:"
assert_stdout "Voluntary ctx:"
assert_stdout "Resident:"
assert_stdout "Open FDs:"
assert_stdout "Command line:"
assert_stdout "=== sample 1 ==="
assert_stdout "=== sample 2 ==="
assert_stdout "count-requires-watch"
assert_stdout "missing-ok"
assert_stdout "invalid-ok"

grep -Fq "Process $target (sleep)" "$redirected"
grep -Fq "pinfo: --count requires --watch" "$stderr_file"
grep -Fq "pinfo: process 99999999 does not exist" "$stderr_file"
grep -Fq "pinfo: invalid PID: abc" "$stderr_file"

echo "process-observability regression tests passed"
