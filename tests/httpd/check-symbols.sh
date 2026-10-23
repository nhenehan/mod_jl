#!/usr/bin/env bash
# The module exports jl_module and nothing else, needs nothing it does not get
# from httpd, APR or the C runtime, and has no static TLS (which would make
# dlopen fail with "cannot allocate memory in static TLS block").
#
#   tests/httpd/check-symbols.sh mod_jl.so
set -uo pipefail
so=$1
fails=0
defined=$(nm -D --defined-only "$so" | awk '{print $3}' | sort -u)
if [[ $defined != jl_module ]]; then
    echo "FAIL exported symbols other than jl_module:"
    echo "$defined" | grep -vx jl_module | head -20
    fails=1
fi
# Undefined symbols must come from a library the module itself needs (libc,
# libm, libgcc_s ...) or from the httpd process (ap_*, apr_*, apu_*).
provided=$(ldd "$so" | awk '{for (i = 1; i <= NF; i++) if ($i ~ /^\//) print $i}' | xargs -r nm -D --defined-only 2>/dev/null |
    awk '{print $3}' | sed 's/@.*//' | sort -u)
odd=$(nm -D --undefined-only "$so" | awk '{print $2}' | sed 's/@.*//' | sort -u |
    grep -vE '^(ap_|apr_|apu_|_ITM_|__gmon_start__)' | comm -23 - <(echo "$provided") || true)
if [[ -n $odd ]]; then
    echo "FAIL undefined symbols not from httpd/APR/libc:"
    echo "$odd" | head -20
    fails=1
fi
if readelf -d "$so" | grep -q STATIC_TLS; then
    echo "FAIL the module uses static TLS"
    fails=1
fi
((fails == 0)) && echo "ok   $so exports only jl_module"
exit $fails
