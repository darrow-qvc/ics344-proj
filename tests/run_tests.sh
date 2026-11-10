#!/usr/bin/env bash
# run_tests.sh - golden-output regression suite for MiniSh.
#
# Each tests/cases/NAME.sh is fed to the shell on stdin from inside a fresh
# scratch directory.  stdout+stderr are captured, "status=N" is appended,
# and the result is diffed against tests/cases/NAME.expected.
#
#   tests/run_tests.sh build/minish            run all cases
#   tests/run_tests.sh build/minish 03_redir   run cases matching a pattern
#   BASH_PARITY=1 tests/run_tests.sh build/minish
#        also run every case not tagged "# nobash" through bash and check
#        that minish's output matches bash's (after normalising the shell
#        name in diagnostics).  This is the "output matches bash" metric.
#   UPDATE=1 tests/run_tests.sh build/minish   regenerate .expected files
set -u
SHELL_BIN=$(readlink -f "${1:?usage: run_tests.sh path/to/minish [pattern]}")
PATTERN=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)
CASES=$HERE/cases
pass=0 fail=0 failed=()

run_case() {   # $1 = shell binary, $2 = case file, $3 = output file
    local work; work=$(mktemp -d)
    ( cd "$work" && HOME=/ "$1" < "$2" > "$3" 2>&1; echo "status=$?" >> "$3" )
    rm -rf "$work"
}

normalise_bash() {
    # bash prefixes "bash: line N: " (scripts) or "bash: "; map to minish
    sed -E -e 's#^(/[^ :]*/)?bash: (line [0-9]+: )?#minish: #' "$1"
}

for case in "$CASES"/*.sh; do
    name=$(basename "$case" .sh)
    [[ -n $PATTERN && $name != *$PATTERN* ]] && continue
    expected=$CASES/$name.expected
    out=$(mktemp)
    run_case "$SHELL_BIN" "$case" "$out"
    if [[ ${UPDATE:-0} = 1 ]]; then
        cp "$out" "$expected"; echo "updated $name"; rm -f "$out"; continue
    fi
    if diff -u "$expected" "$out" > "$out.diff"; then
        pass=$((pass+1)); printf 'PASS %s\n' "$name"
    else
        fail=$((fail+1)); failed+=("$name"); printf 'FAIL %s\n' "$name"; cat "$out.diff"
    fi
    if [[ ${BASH_PARITY:-0} = 1 ]] && ! grep -q '^# nobash' "$case"; then
        bout=$(mktemp)
        run_case /bin/bash "$case" "$bout"
        if diff -u <(normalise_bash "$bout") "$out" > "$out.bdiff"; then
            pass=$((pass+1)); printf 'PASS %s (bash parity)\n' "$name"
        else
            fail=$((fail+1)); failed+=("$name/bash"); printf 'FAIL %s (bash parity)\n' "$name"; cat "$out.bdiff"
        fi
        rm -f "$bout" "$out.bdiff"
    fi
    rm -f "$out" "$out.diff"
done

echo "----"
echo "passed: $pass  failed: $fail"
[[ $fail -gt 0 ]] && { printf '  %s\n' "${failed[@]}"; exit 1; }
exit 0
