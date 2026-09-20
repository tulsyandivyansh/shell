#!/usr/bin/env bash
set -euo pipefail

shell_bin="${1:?usage: parser_regression.sh /path/to/shell}"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

input_file="$tmpdir/input.txt"
out_file="$tmpdir/out.txt"
err_file="$tmpdir/err.txt"
stdout_file="$tmpdir/stdout.txt"
stderr_file="$tmpdir/stderr.txt"
printf 'alpha\nbeta\ngamma\n' >"$input_file"

cat >"$tmpdir/commands.txt" <<CMDS
echo no_space|wc -w
echo alpha>$out_file;cat<$out_file
/bin/true&&echo and-ran
/bin/false&&echo and-should-not-run
/bin/false||echo or-ran
/bin/true||echo or-should-not-run
echo first;echo second
grep beta<$input_file
echo foo2>$out_file
cat<$out_file
echo 'a|b && c;d'
parser_missing_command 2>$err_file||echo recovered
cat<$err_file
echo hi |
echo "unterminated
exit 0
CMDS

"$shell_bin" <"$tmpdir/commands.txt" >"$stdout_file" 2>"$stderr_file"

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

assert_absent() {
    local unexpected="$1"
    if grep -Fqx -- "$unexpected" "$stdout_file"; then
        echo "unexpected output: $unexpected" >&2
        cat "$stdout_file" >&2
        exit 1
    fi
}

assert_line "1"
assert_line "alpha"
assert_line "and-ran"
assert_line "or-ran"
assert_line "first"
assert_line "second"
assert_line "beta"
assert_line "foo2"
assert_line "a|b && c;d"
assert_line "recovered"
assert_line "parser_missing_command: command not found"
assert_absent "and-should-not-run"
assert_absent "or-should-not-run"

grep -Fq "syntax error: expected command after pipe" "$stderr_file"
grep -Fq "syntax error: unmatched double quote" "$stderr_file"

echo "parser regression tests passed"
