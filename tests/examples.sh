#!/usr/bin/env bash
# The three example applications, each in a real bin/jlserv worker, driven
# by jlc.
#
#   tests/examples.sh JLC SRCDIR
#
# JLC_WRAP prefixes every jlc run (tests/vg.sh for valgrind, with VG_LOG_DIR);
# JL_EX_SUBSCRIBE_DELAY gives the event stream time to connect before the
# upload starts (default 0.5 s).
set -uo pipefail
jlc=$1
src=$2
julia=${JULIA:-julia}
ex=$src/jl_serv/JlServ/examples
read -ra wrap <<<"${JLC_WRAP:-}"

work=$(mktemp -d "${TMPDIR:-/tmp}/jl-examples.XXXXXX")
pid=
cleanup() {
    [[ -n $pid ]] && kill -9 "$pid" 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

fails=0
check() { # name, command...
    local name=$1
    shift
    if "$@"; then echo "ok   $name"; else echo "FAIL $name"; fails=$((fails + 1)); fi
}
has() { grep -q -- "$2" <<<"$1"; } # output, pattern

# start APP: runs a worker for APP on $work/sock and sets J (the jlc command).
start() {
    local app=$1
    rm -f "$work/sock"
    JL_SECRET=ex "$julia" --startup-file=no --project="$src/jl_serv/JlServ" -t 4,1 \
        "$src/jl_serv/JlServ/bin/jlserv" "$app" --unix "$work/sock" >"$work/log" 2>&1 &
    pid=$!
    for _ in $(seq 1 600); do
        [[ -S $work/sock ]] && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    [[ -S $work/sock ]] || { echo "FAIL $app did not start"; cat "$work/log"; exit 1; }
    J=("${wrap[@]}" "$jlc" --unix "$work/sock" --secret ex)
}
stop() {
    kill -INT "$pid"
    wait "$pid"
    check "worker drains and exits 0" test $? -eq 0
    pid=
    ((fails)) && { echo "--- log"; cat "$work/log"; }
}

echo "# easy/greeter.jl"
start "$ex/easy/greeter.jl"
check "GET /" has "$("${J[@]}" /)" "Try /hello"
check "GET /hello/ada" has "$("${J[@]}" -H 'user-agent: test' /hello/ada)" "Hello, ada! You are using test."
out=$("${J[@]}" -i /teapot 2>&1)
check "GET /teapot is 418 with x-brewing" bash -c 'grep -q "^< 418" <<<"$0" && grep -q "^< x-brewing: earl grey" <<<"$0"' "$out"
stop

echo "# medium/notes.jl"
start "$ex/medium/notes.jl"
check "PUT new note is 201" has "$("${J[@]}" -i -X PUT --data 'buy milk' /notes/shopping 2>&1)" "^< 201"
out=$("${J[@]}" -i /notes/shopping 2>&1)
check "GET note" has "$out" "^buy milk$"
tag=$(sed -n 's/^< etag: //p' <<<"$out")
check "GET with matching If-None-Match is 304" has "$("${J[@]}" -i -H "if-none-match: $tag" /notes/shopping 2>&1)" "^< 304"
check "HEAD has no body" test -z "$("${J[@]}" -X HEAD /notes/shopping)"
check "PUT with current If-Match is 204" has "$("${J[@]}" -i -X PUT -H "if-match: $tag" --data 'buy oat milk' /notes/shopping 2>&1)" "^< 204"
check "PUT with stale If-Match is 412" has "$("${J[@]}" -i -X PUT -H "if-match: $tag" --data 'x' /notes/shopping 2>&1)" "^< 412"
check "chunked PUT" has "$(printf 'line 1\nline 2\n' | "${J[@]}" -i -X PUT --chunked --data-file - /notes/todo 2>&1)" "^< 201"
check "GET /notes lists both" test "$("${J[@]}" /notes)" = $'shopping\ntodo'
check "bad id is 400" has "$("${J[@]}" -i -X PUT --data x '/notes/a%20b' 2>&1)" "^< 400"
head -c $((2 << 20)) /dev/zero >"$work/big"
check "2 MiB note is 413" has "$("${J[@]}" -i -X PUT --data-file "$work/big" /notes/big 2>&1)" "^< 413"
check "2 MiB chunked note is 413" has "$("${J[@]}" -i -X PUT --chunked --data-file - /notes/big <"$work/big" 2>&1)" "^< 413"
check "DELETE is 204" has "$("${J[@]}" -i -X DELETE /notes/todo 2>&1)" "^< 204"
check "GET deleted is 404" has "$("${J[@]}" -i /notes/todo 2>&1)" "^< 404"
stop

echo "# difficult/uploads.jl"
start "$ex/difficult/uploads.jl"
"${J[@]}" /uploads/u1/events >"$work/events" 2>&1 &
ev=$!
sleep "${JL_EX_SUBSCRIBE_DELAY:-0.5}"
head -c $((24 << 20)) /dev/urandom >"$work/data"
want=$(sha256sum "$work/data" | cut -d' ' -f1)
lines=$(tr -cd '\n' <"$work/data" | wc -c)
out=$("${J[@]}" -X POST --chunked --data-file - /uploads/u1 <"$work/data")
check "POST streams progress" has "$out" "^received 4194304 bytes"
check "POST digest matches sha256sum" has "$out" "sha256 $want"
wait "$ev"
check "events stream ends with done" has "$(cat "$work/events")" "^data: bytes=$((24 << 20)) lines=$lines sha256=$want"
check "events stream has progress" has "$(cat "$work/events")" "^event: progress"
check "GET result is 200" has "$("${J[@]}" /uploads/u1)" "sha256=$want"
check "second POST to the same id is 409" has "$("${J[@]}" -i -X POST --data x /uploads/u1 2>&1)" "^< 409"
check "unknown upload is 404" has "$("${J[@]}" -i /uploads/nope 2>&1)" "^< 404"
stop

[[ -n ${VG_LOG_DIR:-} ]] && { "$(dirname "$0")/vg-check.sh" "$VG_LOG_DIR" || fails=$((fails + 1)); }
((fails == 0))
