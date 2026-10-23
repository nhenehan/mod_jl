#!/usr/bin/env bash
# End to end: a real bin/jlserv worker on a Unix socket, driven by jlc.
#
#   tests/e2e.sh JLC SRCDIR
#
# JULIA overrides the julia binary; JL_E2E_ECHO_MIB sets the size of the
# POST /echo body (default 16; 1024 checks a 1 GiB echo). JLC_WRAP prefixes
# every jlc run (tests/vg.sh for valgrind; then set VG_LOG_DIR too), and
# JL_E2E_BENCH_SECS / JL_E2E_SIGINT_DELAY stretch the SIGINT bench for it.
set -uo pipefail
jlc=$1
src=$2
julia=${JULIA:-julia}
mib=${JL_E2E_ECHO_MIB:-16}
bench_secs=${JL_E2E_BENCH_SECS:-10}
sigint_delay=${JL_E2E_SIGINT_DELAY:-1}
read -ra wrap <<<"${JLC_WRAP:-}"

work=$(mktemp -d "${TMPDIR:-/tmp}/jl-e2e.XXXXXX")
sock=$work/j1.sock
log=$work/jlserv.log
pid=
cleanup() {
    [[ -n $pid ]] && kill -9 "$pid" 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
check() { # name, command...
    local name=$1
    shift
    if "$@"; then ok "$name"; else bad "$name"; fi
}

export JL_SECRET=e2e
"$julia" --startup-file=no --project="$src/jl_serv/JlServ" -t 4,1 \
    "$src/jl_serv/JlServ/bin/jlserv" "$src/jl_serv/JlServ/examples/app.jl" \
    --unix "$sock" --name e2e >"$log" 2>&1 &
pid=$!
for _ in $(seq 1 600); do
    [[ -S $sock ]] && break
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
done
if [[ ! -S $sock ]]; then
    echo "FAIL jlserv did not start"
    cat "$log"
    exit 1
fi

J=("${wrap[@]}" "$jlc" --unix "$sock" --secret "$JL_SECRET")

check "ping" "${J[@]}" --ping
check "GET /hello/world" test "$("${J[@]}" /hello/world)" = "hello world"
check "GET /whoami" test "$("${J[@]}" /whoami)" = "e2e"
check "GET /nope is 404" grep -q '^< 404' <("${J[@]}" -i /nope 2>&1)
check "GET /boom is 500" grep -q '^< 500' <("${J[@]}" -i /boom 2>&1)
check "wrong secret refused" bash -c '! "$@" 2>/dev/null' _ "${wrap[@]}" "$jlc" --unix "$sock" --secret wrong /hello/x
check "log written while running" grep -q 'handler failed for GET /boom' "$log"

head -c $((mib << 20)) /dev/urandom >"$work/in"
check "POST /echo ${mib} MiB" bash -c '"$@" -X POST --data-file "$0/in" /echo >"$0/out" && cmp "$0/in" "$0/out"' \
    "$work" "${J[@]}"
check "POST /echo ${mib} MiB chunked" bash -c '"$@" -X POST --chunked --data-file - /echo <"$0/in" >"$0/out" && cmp "$0/in" "$0/out"' \
    "$work" "${J[@]}"
rm -f "$work/in" "$work/out"

# SIGINT drains: requests in flight finish, a bench sees no failed request
# (only reconnects refused once the worker has closed), then the worker exits 0.
"${J[@]}" /sleep/2000 >"$work/sleep" 2>&1 &
req=$!
"${J[@]}" bench -c 16 -d "$bench_secs" /hello/world >"$work/bench" 2>&1 &
bench=$!
sleep "$sigint_delay"
kill -INT "$pid"
wait "$req"
check "request in flight at SIGINT completes" test $? -eq 0 -a "$(cat "$work/sleep")" = "slept 2000 ms"
wait "$bench"
check "no failed requests in a bench across SIGINT" test $? -eq 0
for _ in $(seq 1 150); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$pid" 2>/dev/null; then
    bad "SIGINT drains and exits"
else
    wait "$pid"
    check "SIGINT drains and exits 0" test $? -eq 0
fi
pid=

[[ -n ${VG_LOG_DIR:-} ]] && { "$(dirname "$0")/vg-check.sh" "$VG_LOG_DIR" || fails=$((fails + 1)); }

if ((fails)); then
    [[ -f $work/bench ]] && { echo "--- bench"; cat "$work/bench"; }
    echo "--- jlserv log"
    cat "$log"
    exit 1
fi
