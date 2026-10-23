#!/usr/bin/env bash
# The jl-status page and the watchdog. httpd with conf/status.conf.in in
# front of three JlServ workers (lb_app.jl).
#
#   tests/httpd/test_status.sh MODULE SRCDIR
#
# JULIA and APACHE2 override the binaries. JL_T_MPM=prefork runs httpd with
# four processes, so a change made through one must hold in all. JL_ASAN=1
# runs httpd as one process with libasan preloaded. JL_T_KEEP=1 keeps the
# work directory.
set -uo pipefail
module=$1
src=$2
julia=${JULIA:-julia}
here=$(cd "$(dirname "$0")" && pwd)
source "$here/run-httpd.sh"
export JL_T_CONF=$here/conf/status.conf.in

work=$(mktemp -d "${TMPDIR:-/tmp}/jl-st.XXXXXX")
secret=t-$RANDOM$RANDOM
app=$here/lb_app.jl
source "$here/workers.sh"
cleanup() {
    [[ -n ${HTTPD_PID:-} ]] && kill -9 "$HTTPD_PID" 2>/dev/null
    kill_workers
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

for w in j1 j2 j3; do launch "$w"; done
for w in j1 j2 j3; do await "$w" 0; done
JL_T_WATCHDOG=0 httpd_start "$work" "$module" "$secret" || { fails=1; exit 1; }
B="http://127.0.0.1:$HTTPD_PORT"
S="$B/jl-status"
echo "# httpd (${JL_T_MPM:-event}) on port $HTTPD_PORT"

many() { # N URL [curl args...]: one answer per line
    local n=$1 url=$2
    shift 2
    curl -sS --max-time 120 "$@" $(for _ in $(seq 1 "$n"); do printf '%s ' "$url"; done)
}
count() { grep -cx -- "$2" <<<"$1"; }
status() { curl -sS "$S?mime=json"; }
# jqs FILTER: FILTER applied to the JSON status, raw.
jqs() { status | jq -r "$1"; }
member() { echo ".balancers[] | select(.name == \"lbreq\") | .members[] | select(.worker == \"$1\")"; }
post() { # [field=value]...: POST to the writable page; prints the status code
    local args=()
    for f in "$@"; do args+=(--data-urlencode "$f"); done
    curl -sS -o "$work/post.out" -w '%{http_code}' "${args[@]}" "$S?mime=json"
}

# ===== Reading =====
many 40 "$B/req/whoami" >/dev/null
check "JSON: jq reads it" bash -c 'curl -sS "$0?mime=json" | jq -e . >/dev/null' "$S"
check "JSON: elected 10/10/20 after 40 requests" \
    test "$(jqs '[.balancers[] | select(.name == "lbreq") | .members[].elected] | join(",")')" = 10,10,20
check "JSON: nothing busy" test "$(jqs '[.workers[].busy] | add')" = 0
check "JSON: every worker OK" test "$(jqs '[.workers[].state] | unique | join(",")')" = OK
check "JSON: show w=j2 gives only j2" \
    test "$(curl -sS "$S?mime=json&cmd=show&w=j2" | jq -r '[.workers[].name] + [.balancers[].name] | join(",")')" = j2
nonce=$(jqs .nonce)
check "JSON: a nonce" bash -c '[[ $0 =~ ^[0-9a-f]{32}$ ]]' "$nonce"
check "key=value" bash -c 'curl -sS "$0?mime=prop" | grep -qx "balancer.lbreq.member.j2.activation=A"' "$S"
html=$(curl -sS "$S")
check "HTML: the balancer's table" grep -q "<h2>Balancer lbreq</h2>" <<<"$html"
check "HTML: forms carry the nonce" grep -q "name=\"nonce\" value=\"$nonce\"" <<<"$html"
check "HTML read-only: no forms" bash -c '! curl -sS "$0" | grep -q "<form"' "$B/jl-status-ro"

# ===== Refusals =====
code() { curl -sS -o /dev/null -w '%{http_code}' "$@"; }
check "update by GET: 405" test "$(code "$S?cmd=update&lb=lbreq&w=j2&activation=D&nonce=$nonce")" = 405
check "update without the nonce: 403" test "$(post cmd=update lb=lbreq w=j2 activation=D)" = 403
check "update with a wrong nonce: 403" test "$(post cmd=update lb=lbreq w=j2 activation=D nonce=00)" = 403
check "update on the read-only page: 403" test "$(code --data "cmd=update&lb=lbreq&w=j2&activation=D&nonce=$nonce" \
    "$B/jl-status-ro?mime=json")" = 403
check "update with activation=X: 400" test "$(post cmd=update lb=lbreq w=j2 activation=X nonce="$nonce")" = 400
check "update of a worker that isn't a member: 400" \
    test "$(post cmd=update lb=lbreq w=nobody activation=D nonce="$nonce")" = 400
check "mime=xml: 400" test "$(code "$S?mime=xml")" = 400
check "nothing changed by the refusals" test "$(jqs "$(member j2) | .activation")" = A

# ===== update =====
check "disable j2: 200" test "$(post cmd=update lb=lbreq w=j2 activation=D nonce="$nonce")" = 200
check "disable j2: the answer shows it" test "$(jq -r "$(member j2) | .activation" "$work/post.out")" = D
out=$(many 30 "$B/req/whoami")
check "j2 disabled: no requests to it" test "$(count "$out" j2)" = 0
check "j2 disabled: its sessions still reach it" \
    test "$(curl -sS -H 'Cookie: JSESSIONID=x.j2' "$B/req/whoami")" = j2
check "enable j2, lbfactor 3 for j1" bash -c '[[ $0 == 200 && $1 == 200 ]]' \
    "$(post cmd=update lb=lbreq w=j2 activation=A nonce="$nonce")" \
    "$(post cmd=update lb=lbreq w=j1 lbfactor=3 nonce="$nonce")"
check "reset the counts: 200" test "$(post cmd=reset lb=lbreq nonce="$nonce")" = 200
check "reset: elected 0" test "$(jqs '[.balancers[] | select(.name == "lbreq") | .members[].elected] | add')" = 0
out=$(many 60 "$B/req/whoami")
c1=$(count "$out" j1) c2=$(count "$out" j2) c3=$(count "$out" j3)
echo "# lbfactors 3:1:2: j1 $c1, j2 $c2, j3 $c3"
check "lbfactors 3:1:2: 60 requests split 30/10/20" bash -c "(($c1 >= 28 && $c1 <= 32 && $c2 >= 8 && $c2 <= 12 &&
    $c3 >= 18 && $c3 <= 22))"
post cmd=update lb=lbreq w=j1 lbfactor=1 nonce="$nonce" >/dev/null

# ===== recover =====
kill -9 "${wpid[j3]}"
wait "${wpid[j3]}" 2>/dev/null
curl -sS -o /dev/null -H 'Cookie: JSESSIONID=x.j3' "$B/req/whoami"
check "j3 killed: ERROR" test "$(jqs "$(member j3) | .state")" = ERROR
restart_worker j3
check "j3 back but recover_time not over: its sessions go elsewhere" \
    test "$(curl -sS -H 'Cookie: JSESSIONID=x.j3' "$B/req/whoami")" != j3
check "recover j3: 200" test "$(post cmd=recover w=j3 nonce="$nonce")" = 200
check "recover j3: the next of its sessions reaches it" \
    test "$(curl -sS -H 'Cookie: JSESSIONID=x.j3' "$B/req/whoami")" = j3
check "recover j3: OK" test "$(jqs "$(member j3) | .state")" = OK

# ===== The watchdog =====
check "httpd stops cleanly (no crash, no sanitizer report)" httpd_stop "$work"
HTTPD_PID=
JL_T_WATCHDOG=1 httpd_start "$work" "$module" "$secret" || { fails=1; exit 1; }
B="http://127.0.0.1:$HTTPD_PORT"
S="$B/jl-status"
kill -9 "${wpid[j2]}"
wait "${wpid[j2]}" 2>/dev/null
curl -sS -o /dev/null -H 'Cookie: JSESSIONID=x.j2' "$B/req/whoami"
check "watchdog: j2 killed, ERROR" test "$(jqs "$(member j2) | .state")" = ERROR
sleep 2.5
check "watchdog: j2 still down, still ERROR" test "$(jqs "$(member j2) | .state")" = ERROR
restart_worker j2
ok=0
for _ in $(seq 1 50); do
    [[ $(jqs "$(member j2) | .state") == OK ]] && { ok=1; break; }
    sleep 0.1
done
check "watchdog: j2 back, OK with no requests to it" test $ok = 1
check "watchdog: logged" grep -q "jl: watchdog: worker j2 is OK again" "$work/error_log"
check "watchdog: j2's sessions reach it" test "$(curl -sS -H 'Cookie: JSESSIONID=x.j2' "$B/req/whoami")" = j2

# ===== Shut down =====
check "httpd stops cleanly, with the watchdog (no crash, no sanitizer report)" httpd_stop "$work"
HTTPD_PID=
stop_workers

((fails == 0))
