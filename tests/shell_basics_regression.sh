#!/usr/bin/env bash
set -euo pipefail

shell_bin="${1:?usage: shell_basics_regression.sh /path/to/shell}"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

start_dir="$tmpdir/start"
mkdir -p "$start_dir"
commands="$tmpdir/commands.txt"
stdout_file="$tmpdir/stdout.txt"
stderr_file="$tmpdir/stderr.txt"
redirected_out="$tmpdir/redirected.txt"
redirected_err="$tmpdir/redirected.err"

cat >"$commands" <<CMDS
echo hello world
echo "quoted value"
printf 'abc\\n' | wc -l
cd "$start_dir"
pwd
cd / | cat
pwd
echo first > "$redirected_out"
echo second >> "$redirected_out"
cat "$redirected_out"
shell_command_that_does_not_exist 2> "$redirected_err"
cat "$redirected_err"
type echo
exit 0
CMDS

"$shell_bin" <"$commands" >"$stdout_file" 2>"$stderr_file"

assert_line() {
    local expected="$1"
    if ! grep -Fqx -- "$expected" "$stdout_file"; then
        echo "missing expected output line: $expected" >&2
        echo "--- stdout ---" >&2
        cat "$stdout_file" >&2
        echo "--- stderr ---" >&2
        cat "$stderr_file" >&2
        exit 1
    fi
}

assert_line "hello world"
assert_line "quoted value"
assert_line "1"

# A builtin used in a pipeline must execute in a child and must not mutate
# the parent shell's working directory. We therefore expect the same cwd
# before and after `cd / | cat`.
if [[ "$(grep -Fxc -- "$start_dir" "$stdout_file")" -lt 2 ]]; then
    echo "pipeline builtin changed parent shell state" >&2
    cat "$stdout_file" >&2
    exit 1
fi

if [[ "$(cat "$redirected_out")" != $'first\nsecond' ]]; then
    echo "stdout redirection/append regression" >&2
    cat "$redirected_out" >&2
    exit 1
fi

if ! grep -Fq "shell_command_that_does_not_exist: command not found" "$redirected_err"; then
    echo "stderr redirection regression" >&2
    cat "$redirected_err" >&2
    exit 1
fi

assert_line "shell_command_that_does_not_exist: command not found"
assert_line "echo is a shell builtin"

echo "shell basics regression tests passed"
