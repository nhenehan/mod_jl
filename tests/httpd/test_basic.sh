#!/usr/bin/env bash
# httpd with mod_jl in front of one JlServ worker, driven by curl.
#
#   tests/httpd/test_basic.sh MODULE SRCDIR
#
# JULIA and APACHE2 override the binaries. JL_T_BIG_MIB sets the size of the
# large upload and download (default 1024). JL_ASAN=1 runs httpd as one
# process with libasan preloaded (a module built with -DJL_SANITIZE=ON).
# JL_T_KEEP=1 keeps the work directory. JL_T_J1_OPTS adds settings to the j1
# worker: connection_pool_size=1 sends every request over one connection.
set -uo pipefail
module=$1
src=$2
julia=${JULIA:-julia}
big=$((${JL_T_BIG_MIB:-1024} << 20))
here=$(cd "$(dirname "$0")" && pwd)
source "$here/run-httpd.sh"

work=$(mktemp -d "${TMPDIR:-/tmp}/jl-t.XXXXXX")
secret=t-$RANDOM$RANDOM
jpid=
cleanup() {
    [[ -n ${HTTPD_PID:-} ]] && kill -9 "$HTTPD_PID" 2>/dev/null
    [[ -n $jpid ]] && kill -9 "$jpid" 2>/dev/null
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
has() { grep -q -- "$2" <<<"$1"; }
hasnt() { ! grep -qi -- "$2" <<<"$1"; }

# ===== The worker (on a unix socket and on TCP), then httpd =====
export JL_T_TCP_PORT=$(_free_port)
JL_SECRET=$secret "$julia" --startup-file=no --project="$src/jl_serv/JlServ" -t 4,1 \
    "$src/jl_serv/JlServ/bin/jlserv" "$here/app.jl" --unix "$work/j1.sock" --tcp "127.0.0.1:$JL_T_TCP_PORT" \
    --name j1 >"$work/jlserv.log" 2>&1 &
jpid=$!
for _ in $(seq 1 1200); do
    grep -q listening "$work/jlserv.log" 2>/dev/null && break
    kill -0 "$jpid" 2>/dev/null || break
    sleep 0.1
done
grep -q listening "$work/jlserv.log" || { echo "FAIL jlserv did not start"; cat "$work/jlserv.log"; exit 1; }
httpd_start "$work" "$module" "$secret" || { fails=1; exit 1; }
B="http://127.0.0.1:$HTTPD_PORT"
C=(curl -sS --max-time 120 -H 'Expect:')
echo "# httpd on port $HTTPD_PORT, worker pid $jpid"

# ===== GET, HEAD, 404s, status =====
check "GET /hello/world" test "$("${C[@]}" "$B/hello/world")" = "hello world"
out=$("${C[@]}" -I "$B/hello/world")
check "HEAD has the GET's Content-Length" has "$out" "^Content-Length: 12"
check "HEAD has no body" test "$("${C[@]}" -I -o /dev/null -w '%{size_download}' "$B/hello/world")" = 0
out=$("${C[@]}" -i "$B/nothing/here")
check "unmounted path: httpd's own 404" bash -c 'grep -q "^HTTP/1.1 404" <<<"$0" && grep -q "<title>404 Not Found" <<<"$0"' "$out"
out=$("${C[@]}" -i "$B/missing/x")
check "mounted path the worker doesn't know: its 404" bash -c 'grep -q "^HTTP/1.1 404" <<<"$0" && grep -q "^Not Found" <<<"$0"' "$out"
out=$("${C[@]}" -i "$B/viaset/x")
check "SetHandler jl:j1 reaches the worker" bash -c 'grep -q "^HTTP/1.1 404" <<<"$0" && grep -q "^Not Found" <<<"$0"' "$out"
check "a worker over TCP" test "$("${C[@]}" -w ' %{http_code}' "$B/tcp/x")" = $'Not Found\n 404'
check "dot segments are resolved before forwarding" test "$("${C[@]}" --path-as-is "$B/hello/../whoami")" = j1
check "a status httpd has no name for: 299" has "$("${C[@]}" -i "$B/status/299")" "^HTTP/1.1 299"
check "worker error before headers: its own 500 body" \
    test "$("${C[@]}" -w ' %{http_code}' "$B/boom")" = $'Internal Server Error\n 500'

# ===== Headers =====
got=$("${C[@]}" -D - -o /dev/null "$B/order" | tr -d '\r' | grep -iE '^(x-order|set-cookie)' | tr 'A-Z' 'a-z')
want=$'x-order-1: a\nset-cookie: one=1; path=/\nx-order-2: b\nx-order-3: c\nset-cookie: two=2; path=/\nx-order-4: d\nx-order-5: e'
check "response headers keep their order, both Set-Cookie" test "$got" = "$want"
out=$("${C[@]}" -H 'X-A: 1' -H 'X-B: 2' -H 'Connection: X-Hop' -H 'X-Hop: 1' -H 'Upgrade: foo' "$B/headers")
check "request headers keep their order" bash -c 'a=$(grep -n "^x-a:" <<<"$0" | cut -d: -f1); b=$(grep -n "^x-b:" <<<"$0" | cut -d: -f1); [[ -n $a && -n $b && $a -lt $b ]]' "$(tr 'A-Z' 'a-z' <<<"$out")"
check "hop-by-hop request headers are not forwarded" \
    bash -c 'grep -qi "^x-a:" <<<"$0" && ! grep -qiE "^(x-hop|upgrade|connection|expect):" <<<"$0"' "$out"

# ===== Request bodies =====
head -c 300000 /dev/urandom >"$work/small"
"${C[@]}" -T - -X POST "$B/echo" <"$work/small" >"$work/small.out"
check "chunked POST, echoed" cmp -s "$work/small" "$work/small.out"
check "Content-Length POST, counted" test "$("${C[@]}" --data-binary @"$work/small" "$B/count")" = 300000

head -c 65536 /dev/urandom >"$work/64k"
start=$(date +%s)
err=$(curl -sS -v --max-time 60 --expect100-timeout 30 -H 'Expect: 100-continue' --data-binary @"$work/64k" \
    "$B/count" 2>&1 >"$work/count.out")
check "100-continue when the worker reads: 100 sent" has "$err" "< HTTP/1.1 100 Continue"
check "100-continue when the worker reads: body counted" test "$(cat "$work/count.out")" = 65536
check "100-continue when the worker reads: no wait for curl's timeout" test $(($(date +%s) - start)) -lt 10
start=$(date +%s)
err=$(curl -sS -v --max-time 60 --expect100-timeout 30 -H 'Expect: 100-continue' --data-binary @"$work/64k" \
    "$B/reject" 2>&1 >/dev/null)
check "100-continue when the worker rejects: no 100" hasnt "$err" "100 Continue"
check "100-continue when the worker rejects: 413" has "$err" "< HTTP/1.1 413"
check "100-continue when the worker rejects: connection closed" bash -c 'grep -qi "^< Connection: close" <<<"$0"' "$err"
check "100-continue when the worker rejects: answered at once" test $(($(date +%s) - start)) -lt 10

# ===== Large transfers =====
head -c "$big" /dev/urandom >"$work/big"
curl -sS --max-time 900 -T "$work/big" -X POST "$B/echo" -o "$work/big.out"
check "$((big >> 20)) MiB upload with Expect, echoed" cmp -s "$work/big" "$work/big.out"
rm -f "$work/big.out"
curl -sS --max-time 900 -H 'Expect:' -T "$work/big" -X POST "$B/echo" -o "$work/big.out"
check "$((big >> 20)) MiB upload without Expect, echoed" cmp -s "$work/big" "$work/big.out"
rm -f "$work/big" "$work/big.out"
want=$(head -c "$big" /dev/zero | sha256sum)
check "$((big >> 20)) MiB download" test "$(curl -sS --max-time 900 "$B/zeros/$big" | sha256sum)" = "$want"

# ===== Failures =====
check "worker not running: 503" test "$("${C[@]}" -o /dev/null -w '%{http_code}' "$B/down")" = 503
check "wrong secret: 503" test "$("${C[@]}" -o /dev/null -w '%{http_code}' "$B/badsecret")" = 503
check "wrong secret: logged" grep -q "HELLO refused: wrong secret" "$work/error_log"
curl -s --max-time 60 "$B/fail-after/65536" -o /dev/null
check "worker fails after a chunked response started: client sees it cut short" test $? -eq 18
curl -s --max-time 60 "$B/fail-after-cl/65536" -o /dev/null
check "worker fails after a Content-Length response started: client sees it cut short" test $? -eq 18

# ===== Client abort =====
# The aborted request's connection is closed, not put back: the open
# connections to the worker drop by one (or stay at none).
j1_conns() { ss -xH state connected 2>/dev/null | grep -c "$work/j1.sock"; }
before=$(j1_conns)
curl -sS --max-time 120 "$B/zeros/$((1 << 30))" 2>/dev/null | head -c 1048576 >/dev/null
for _ in $(seq 1 100); do
    grep -q "jl: client aborted" "$work/error_log" && break
    sleep 0.1
done
check "client abort: logged" grep -q "jl: client aborted" "$work/error_log"
gone=0
for _ in $(seq 1 100); do
    now=$(j1_conns)
    ((now == 0 || now < before)) && { gone=1; break; }
    sleep 0.1
done
check "client abort: the worker connection is closed" test $gone = 1
check "client abort: the worker is still running" kill -0 "$jpid"
check "client abort: the next request is served" test "$("${C[@]}" "$B/hello/again")" = "hello again"

# ===== Shut down =====
check "httpd stops cleanly (no crash, no sanitizer report)" httpd_stop "$work"
HTTPD_PID=
kill -INT "$jpid"
wait "$jpid"
check "the worker drains and exits 0" test $? -eq 0
jpid=
check "the worker logged no failed connection" bash -c '! grep -E "connection .* failed" "$0"' "$work/jlserv.log"

((fails == 0))
