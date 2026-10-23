#!/usr/bin/env bash
# Runs httpd with conf/httpd.conf.in, as the invoking user, on a free port.
#
#   tests/httpd/run-httpd.sh start WORK MODULE [SECRET]   prints the port; httpd runs in the background
#   tests/httpd/run-httpd.sh stop WORK                    stops it; fails on crashes or sanitizer reports
#
# APACHE2 names the binary (default: found on PATH or in /usr/sbin).
# JL_T_TCP_PORT is the port of the worker reached over TCP (jtcp).
# JL_T_CONF names another config template (default conf/httpd.conf.in), and
# JL_T_J1_OPTS adds settings to its j1 worker (@J1OPTS@), and JL_T_MPM names
# the MPM where a template asks (@MPM@, default event), and JL_T_WATCHDOG
# its JlWatchdogInterval (@WATCHDOG@, default 0).
# JL_HTTPD_X=1 runs one process (apache2 -X), for gdb; JL_ASAN=1 also preloads
# libasan, for a module built with -DJL_SANITIZE=ON.
# The functions httpd_start and httpd_stop can be used by sourcing this file.
set -uo pipefail

_here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

_free_port() {
    local p
    for _ in $(seq 1 50); do
        p=$((20000 + RANDOM % 40000))
        [[ -z $(ss -Htln "sport = :$p" 2>/dev/null) ]] && { echo "$p"; return 0; }
    done
    return 1
}

# httpd_start WORK MODULE SECRET: sets HTTPD_PORT and HTTPD_PID.
httpd_start() {
    local work=$1 module=$2 secret=${3:-}
    local bin=${APACHE2:-$(command -v apache2 || command -v httpd || echo /usr/sbin/apache2)}
    mkdir -p "$work/run" "$work/htdocs"
    rm -f "$work"/asan.* "$work"/ubsan.*
    local env=() args=(-f "$work/httpd.conf" -DFOREGROUND)
    if [[ ${JL_HTTPD_X:-0} == 1 || ${JL_ASAN:-0} == 1 ]]; then
        args=(-X -f "$work/httpd.conf")
    fi
    if [[ ${JL_ASAN:-0} == 1 ]]; then
        env=(LD_PRELOAD="$(gcc -print-file-name=libasan.so)"
            ASAN_OPTIONS="detect_leaks=0:log_path=$work/asan"
            UBSAN_OPTIONS="print_stacktrace=1:log_path=$work/ubsan")
    fi
    for attempt in 1 2 3; do
        HTTPD_PORT=$(_free_port) || { echo "no free port" >&2; return 1; }
        sed -e "s|@WORK@|$work|g" -e "s|@PORT@|$HTTPD_PORT|g" -e "s|@MODULE@|$module|g" -e "s|@SECRET@|$secret|g" \
            -e "s|@TCPPORT@|${JL_T_TCP_PORT:-1}|g" -e "s|@J1OPTS@|${JL_T_J1_OPTS:-}|g" \
            -e "s|@MPM@|${JL_T_MPM:-event}|g" -e "s|@WATCHDOG@|${JL_T_WATCHDOG:-0}|g" \
            "${JL_T_CONF:-$_here/conf/httpd.conf.in}" >"$work/httpd.conf"
        : >"$work/error_log"
        # A process group of its own: on shutdown, prefork sends SIGTERM to
        # its whole group, which would otherwise be the caller's.
        setsid env "${env[@]}" "$bin" "${args[@]}" >>"$work/httpd.out" 2>&1 &
        HTTPD_PID=$!
        for _ in $(seq 1 100); do
            kill -0 "$HTTPD_PID" 2>/dev/null || break
            [[ $(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$HTTPD_PORT/__ready") != 000 ]] && return 0
            sleep 0.1
        done
        kill -9 "$HTTPD_PID" 2>/dev/null
        wait "$HTTPD_PID" 2>/dev/null
        grep -q "could not bind" "$work/error_log" "$work/httpd.out" 2>/dev/null && continue
        echo "httpd did not start:" >&2
        cat "$work/httpd.out" "$work/error_log" >&2
        return 1
    done
    return 1
}

# httpd_stop WORK: stops httpd; fails if it crashed or a sanitizer reported.
httpd_stop() {
    local work=$1 pid=${HTTPD_PID:-$(cat "$work/run/httpd.pid" 2>/dev/null)} bad=0
    if [[ -n $pid ]]; then
        kill -TERM "$pid" 2>/dev/null
        for _ in $(seq 1 100); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        kill -0 "$pid" 2>/dev/null && { echo "httpd did not stop" >&2; kill -9 "$pid"; bad=1; }
        wait "$pid" 2>/dev/null
    fi
    if grep -E "exit signal|AH00052|Segmentation" "$work/error_log" >&2; then
        bad=1
    fi
    for f in "$work"/asan.* "$work"/ubsan.*; do
        [[ -e $f ]] || continue
        echo "--- $f" >&2
        cat "$f" >&2
        bad=1
    done
    return $bad
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    case ${1:-} in
    start)
        httpd_start "$2" "$3" "${4:-}" && echo "$HTTPD_PORT"
        ;;
    stop)
        httpd_stop "$2"
        ;;
    *)
        echo "usage: $0 start WORK MODULE [SECRET] | stop WORK" >&2
        exit 2
        ;;
    esac
fi
