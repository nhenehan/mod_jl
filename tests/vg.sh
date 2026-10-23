#!/usr/bin/env bash
# Runs a command under valgrind memcheck, one log per process in $VG_LOG_DIR.
# Definite and indirect leaks are errors; still-reachable memory (exit paths,
# Rust thread-locals) is not. tests/vg-check.sh reads the logs afterwards,
# since some callers ignore the exit status.
#
#   VG_LOG_DIR=DIR tests/vg.sh CMD [ARGS...]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${VG_LOG_DIR:?VG_LOG_DIR must be set}"
mkdir -p "$VG_LOG_DIR"
exec valgrind --tool=memcheck --error-exitcode=99 \
    --leak-check=full --errors-for-leak-kinds=definite,indirect --show-leak-kinds=definite,indirect \
    --track-origins=yes --suppressions="$here/valgrind.supp" \
    --log-file="$VG_LOG_DIR/%p.log" "$@"
