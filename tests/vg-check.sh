#!/usr/bin/env bash
# Fails if any valgrind log in DIR reports an error, printing those logs.
#
#   tests/vg-check.sh DIR
set -uo pipefail
dir=$1
shopt -s nullglob
logs=("$dir"/*.log)
if ((${#logs[@]} == 0)); then
    echo "FAIL no valgrind logs in $dir"
    exit 1
fi
bad=()
for f in "${logs[@]}"; do
    grep -q 'ERROR SUMMARY: [1-9]' "$f" && bad+=("$f")
done
if ((${#bad[@]})); then
    echo "FAIL valgrind errors in ${#bad[@]} of ${#logs[@]} processes"
    for f in "${bad[@]}"; do
        echo "--- $f"
        cat "$f"
    done
    exit 1
fi
echo "ok   valgrind clean in ${#logs[@]} processes"
