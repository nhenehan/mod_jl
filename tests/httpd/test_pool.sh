#!/usr/bin/env bash
# The connection pool and CPing. httpd with conf/pool.conf.in in front of one
# JlServ worker (pool_app.jl), which is stopped, restarted and killed.
#
#   tests/httpd/test_pool.sh MODULE SRCDIR
#
# JULIA and APACHE2 override the binaries. JL_ASAN=1 runs httpd as one
# process with libasan preloaded (a module built with -DJL_SANITIZE=ON).
# JL_T_KEEP=1 keeps the work directory.
set -uo pipefail
module=$1
src=$2
julia=${JULIA:-julia}
here=$(cd "$(dirname "$0")" && pwd)
source "$here/run-httpd.sh"
export JL_T_CONF=$here/conf/pool.conf.in

work=$(mktemp -d "${TMPDIR:-/tmp}/jl-p.XXXXXX")
secret=t-$RANDOM$RANDOM
pids=()
cleanup() {
    [[ -n ${HTTPD_PID:-} ]] && kill -9 "$HTTPD_PID" 2>/dev/null
    for p in "${pids[@]}"; do kill -9 "$p" 2>/dev/null; done
    if ((fails)) || [[ ${JL_T_KEEP:-0} == 1 ]]; then
        echo "kept $work"
    else
        rm -rf "$work"
    fi
}
fails=0
trap cleanup EXIT

check() { # name, command...
    local name=$1
    shift
    if "$@"; then echo "ok   $name"; else echo "FAIL $name"; fails=$((fails + 1)); fi
}
between() { awk -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(v >= lo && v <= hi) }'; }
log_count() { grep -c -- "$1" "$work/error_log"; }
j1_conns() { ss -xH state connected 2>/dev/null | grep -c "$work/j1.sock"; }

# start_worker N: a jlserv on j1.sock, logging to jlserv-N.log; sets WPID once
# it listens.
start_worker() {
    local log=$work/jlserv-$1.log
    JL_SECRET=$secret "$julia" --startup-file=no --project="$src/jl_serv/JlServ" -t 4,1 \
        "$src/jl_serv/JlServ/bin/jlserv" "$here/pool_app.jl" --unix "$work/j1.sock" --name "j$1" >"$log" 2>&1 &
    WPID=$!
    pids+=("$WPID")
    for _ in $(seq 1 1200); do
        grep -q listening "$log" 2>/dev/null && return 0
        kill -0 "$WPID" 2>/dev/null || break
        sleep 0.1
    done
    echo "FAIL jlserv $1 did not start"
    cat "$log"
    exit 1
}

start_worker 1
w1=$WPID
httpd_start "$work" "$module" "$secret" || { fails=1; exit 1; }
B="http://127.0.0.1:$HTTPD_PORT"
C=(curl -sS --max-time 60)
code() { "${C[@]}" -o /dev/null -w '%{http_code}' "$@"; }
burst() { # URL: 8 at once, so 8 connections open
    local b=()
    for _ in $(seq 1 8); do
        "${C[@]}" "$1" >/dev/null &
        b+=($!)
    done
    wait "${b[@]}"
}
echo "# httpd on port $HTTPD_PORT, worker pid $w1"

# ===== Connections are kept =====
for _ in 1 2 3 4 5; do "${C[@]}" "$B/j1/hello" >/dev/null; done
check "sequential requests: one connection" test "$(log_count 'jl: j1: connected')" = 1
# Fewer client connections than httpd threads: past that, the event MPM
# closes idle keep-alive connections, which wrk counts as read errors.
wrk -t4 -c12 -d5s "$B/j1/hello" >"$work/wrk.out" 2>&1 &
wrk_pid=$!
sleep 1.5
n1=$(j1_conns)
sleep 2.5
n2=$(j1_conns)
wait "$wrk_pid"
echo "# under wrk: $n1, then $n2 connections; $(grep 'Requests/sec' "$work/wrk.out")"
check "under wrk: the connection count is stable" test "$n1" = "$n2"
check "under wrk: at most one connection per thread" test "$(log_count 'jl: j1: connected')" -le 16
check "under wrk: every response 2xx" bash -c '! grep -qE "Non-2xx|Socket errors" "$0"' "$work/wrk.out"
check "under wrk: every request logged 200" \
    bash -c '! grep "/j1/hello" "$0" | awk "{ print \$(NF - 2) }" | grep -qv "^200$"' "$work/access_log"

# ===== ping_mode I =====
"${C[@]}" "$B/jint/hello" >/dev/null
"${C[@]}" "$B/jint/hello" >/dev/null
check "ping_mode I: no CPing on a connection just used" test "$(log_count 'jl: jint: CPONG')" = 0
sleep 1.5
"${C[@]}" "$B/jint/hello" >/dev/null
check "ping_mode I: a CPing after connection_ping_interval" test "$(log_count 'jl: jint: CPONG')" = 1

# ===== ping_mode A, with the worker stopped =====
"${C[@]}" "$B/jping/hello" >/dev/null
"${C[@]}" "$B/jping/hello" >/dev/null
check "ping_mode P: a CPing before each request on a pooled connection" test "$(log_count 'jl: jping: CPONG')" = 1
kill -STOP "$w1"
out=$("${C[@]}" -o /dev/null -w '%{http_code} %{time_total}' "$B/jping/hello")
echo "# stopped worker, pooled connection: $out"
check "stopped worker, pooled connection: 503" test "${out% *}" = 503
check "stopped worker, pooled connection: within ping_timeout" between "${out#* }" 0.9 2.5
check "stopped worker: logged" grep -q "no CPONG within ping_timeout" "$work/error_log"
out=$("${C[@]}" -o /dev/null -w '%{http_code} %{time_total}' "$B/jping/hello")
echo "# stopped worker, new connection: $out"
check "stopped worker, new connection: 503" test "${out% *}" = 503
check "stopped worker, new connection: within ping_timeout" between "${out#* }" 0.9 2.5
check "stopped worker, new connection: HELLO timed out" grep -q "no HELLO_ACK within ping_timeout" "$work/error_log"
kill -CONT "$w1"
check "worker continued: served again" test "$(code "$B/jping/hello")" = 200

# ===== Waiting for a connection =====
"${C[@]}" "$B/jone/sleep/2000" >"$work/slow.out" &
slow=$!
sleep 0.5
out=$("${C[@]}" -o /dev/null -w '%{http_code} %{time_total}' "$B/jone/hello")
check "pool of one in use: 503" test "${out% *}" = 503
check "pool of one in use: after connection_acquire_timeout" between "${out#* }" 0.15 1.0
wait "$slow"
check "pool of one in use: the slow request is served" test "$(cat "$work/slow.out")" = slept
check "pool of one free again: served" test "$(code "$B/jone/hello")" = 200

# ===== Idle connections close =====
burst "$B/jttl/sleep/300"
sleep 1.5
"${C[@]}" "$B/jttl/hello" >/dev/null
check "connection_pool_timeout: idle connections closed" grep -qE "jl: jttl: closed [0-9]+ idle connection" \
    "$work/error_log"

# ===== Rolling restart: a new worker takes over the socket, the old drains =====
load() { # N: sequential requests until stop exists; status codes to load-N
    while [[ ! -e $work/stop ]]; do
        if ((RANDOM % 2)); then
            code "$B/j1/hello"
        else
            code --data-binary "posted" "$B/j1/count"
        fi
        echo
    done >"$work/load-$1"
}
rm -f "$work/stop"
loaders=()
for i in 1 2 3 4; do
    load "$i" &
    loaders+=($!)
done
sleep 1
start_worker 2
w2=$WPID
sleep 1
kill -INT "$w1"
wait "$w1"
w1_exit=$?
sleep 1
touch "$work/stop"
wait "${loaders[@]}"
total=$(cat "$work"/load-* | wc -l)
bad=$(cat "$work"/load-* | grep -vc '^200$')
echo "# rolling restart: $total requests, $bad not 200"
check "rolling restart: every request served" test "$bad" = 0
check "rolling restart: the old worker drained and exited 0" test "$w1_exit" = 0
check "rolling restart: the socket now reaches the new worker" test "$("${C[@]}" "$B/j1/pid")" = "$w2"
check "rolling restart: the socket file is still there" test -S "$work/j1.sock"

# ===== Crash: the pooled connections are dead, none of them answers 502 =====
burst "$B/j1/sleep/100"
kill -9 "$w2"
wait "$w2" 2>/dev/null
start_worker 3
w3=$WPID
codes=$(for _ in $(seq 1 50); do code "$B/j1/hello"; echo; done | sort | uniq -c | tr -s ' ')
echo "# after the crash: $codes"
check "crash and restart: 50 requests all served" test "$codes" = " 50 200"
check "crash and restart: POST on the pool served" test "$("${C[@]}" --data-binary "abc" "$B/j1/count")" = 3

# ===== Death in a handler: retried only when it is safe =====
"${C[@]}" "$B/jone/hello" >/dev/null # jone's one connection is pooled now
retries=$(log_count "trying again on a new one")
check "worker dies on a POST: 502" test "$(code --data-binary "x" "$B/jone/die")" = 502
check "worker dies on a POST: not retried" test "$(log_count "trying again on a new one")" = "$retries"
start_worker 4
"${C[@]}" "$B/jone/hello" >/dev/null
check "worker dies on a GET: retried on a new connection" \
    bash -c '[[ $(curl -sS --max-time 60 -o /dev/null -w "%{http_code}" "$0") == 503 ]] &&
             [[ $(grep -c "trying again on a new one" "$1") -gt $2 ]]' "$B/jone/die" "$work/error_log" "$retries"
start_worker 5
check "after the deaths: served" test "$(code "$B/jone/hello")" = 200

# ===== Shut down =====
check "httpd stops cleanly (no crash, no sanitizer report)" httpd_stop "$work"
HTTPD_PID=
kill -INT "$WPID"
wait "$WPID"
check "the worker drains and exits 0" test $? -eq 0
check "no worker logged a failed connection" bash -c '! grep -E "connection .* failed" "$0"/jlserv-*.log' "$work"

((fails == 0))
