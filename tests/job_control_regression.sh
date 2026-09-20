#!/usr/bin/env bash
set -euo pipefail

shell_bin="${1:?usage: job_control_regression.sh /path/to/shell}"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

workdir="$tmpdir/work"
mkdir -p "$workdir"
stdout_file="$tmpdir/stdout.txt"
stderr_file="$tmpdir/stderr.txt"

cat >"$tmpdir/commands.txt" <<CMDS
cd "$workdir"
pwd
cd / &
sleep 0.1
pwd
type jobs
type fg
type bg
sleep 1 & echo after-background
jobs
fg %999 || echo fg-missing
exit 0
CMDS

"$shell_bin" <"$tmpdir/commands.txt" >"$stdout_file" 2>"$stderr_file"

assert_contains() {
    local expected="$1"
    if ! grep -Fq -- "$expected" "$stdout_file"; then
        echo "missing expected output: $expected" >&2
        echo "--- stdout ---" >&2
        cat "$stdout_file" >&2
        echo "--- stderr ---" >&2
        cat "$stderr_file" >&2
        exit 1
    fi
}

# Background builtins execute in a child and must not mutate parent state.
if [[ "$(grep -Fxc -- "$workdir" "$stdout_file")" -lt 2 ]]; then
    echo "background cd mutated parent shell state" >&2
    cat "$stdout_file" >&2
    exit 1
fi

assert_contains "jobs is a shell builtin"
assert_contains "fg is a shell builtin"
assert_contains "bg is a shell builtin"
assert_contains "after-background"
assert_contains "Running"
assert_contains "sleep 1"
assert_contains "fg-missing"
grep -Fq "fg: no such job" "$stderr_file"

echo "job-control regression tests passed"
