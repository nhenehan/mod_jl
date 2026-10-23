#!/usr/bin/env bash
# Load balancing, sticky sessions, failover and shared state. httpd with
# conf/lb.conf.in in front of three JlServ workers (lb_app.jl), j1-j3, plus
# two workers that never run.
#
#   tests/httpd/test_lb.sh MODULE SRCDIR
#
# JULIA and APACHE2 override the binaries. JL_T_MPM=prefork runs httpd with
# four processes instead of one, so every check also shows that what one
# process learns, the others see. JL_ASAN=1 runs httpd as one process with
# libasan preloaded (a module built with -DJL_SANITIZE=ON). JL_T_KEEP=1 keeps
# the work directory.
set -uo pipefail
module=$1
src=$2
julia=${JULIA:-julia}
here=$(cd "$(dirname "$0")" && pwd)
source "$here/run-httpd.sh"
export JL_T_CONF=$here/conf/lb.conf.in

work=$(mktemp -d "${TMPDIR:-/tmp}/jl-lb.XXXXXX")
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
near() { (($1 >= $2 - $3 && $1 <= $2 + $3)); } # value, target, tolerance

for w in j1 j2 j3; do launch "$w"; done
for w in j1 j2 j3; do await "$w" 0; done
httpd_start "$work" "$module" "$secret" || { fails=1; exit 1; }
B="http://127.0.0.1:$HTTPD_PORT"
echo "# httpd (${JL_T_MPM:-event}) on port $HTTPD_PORT, workers ${wpid[*]}"

# many N URL [curl args...]: N requests, one answer per line.
many() {
    local n=$1 url=$2
    shift 2
    curl -sS --max-time 120 "$@" $(for _ in $(seq 1 "$n"); do printf '%s ' "$url"; done)
}
codes() { # N URL [curl args...]: the status of each, one per line
    local n=$1 url=$2
    shift 2
    for _ in $(seq 1 "$n"); do curl -sS --max-time 60 -o /dev/null -w '%{http_code}\n' "$@" "$url"; done
}
count() { grep -cx -- "$2" <<<"$1"; } # answers, name
last_election() { grep "jl: $1: elected" "$work/error_log" | tail -1; }

# ===== Distribution: lbfactors 1:1:2 =====
for lb in req traffic busy next; do
    out=$(many 400 "$B/$lb/whoami")
    c1=$(count "$out" j1) c2=$(count "$out" j2) c3=$(count "$out" j3)
    echo "# $lb: j1 $c1, j2 $c2, j3 $c3"
    check "$lb: 400 requests split 1:1:2" bash -c "(($c1 >= 98 && $c1 <= 102 && $c2 >= 98 && $c2 <= 102 &&
        $c3 >= 196 && $c3 <= 204))"
done

# ===== Busyness steers around a busy worker =====
curl -sS --max-time 60 "$B/busy/sleep/3000" >"$work/slow.out" &
slow=$!
sleep 0.5
out=$(many 20 "$B/busy/whoami")
wait "$slow"
held=$(cat "$work/slow.out")
check "Busyness: no request to the worker busy with a slow one ($held)" test "$(count "$out" "$held")" = 0

# ===== Sticky sessions =====
out=$(many 50 "$B/req/whoami" -H 'Cookie: other=1; JSESSIONID=abc.j2')
check "sticky by cookie: 50 of 50 to j2" test "$(count "$out" j2)" = 50
out=$(many 20 "$B/req/p/;jsessionid=abc.j3/whoami")
check "sticky by path parameter: 20 of 20 to j3" test "$(count "$out" j3)" = 20
out=$(many 40 "$B/req/whoami" -H 'Cookie: JSESSIONID=abc.nobody')
check "unknown route: balanced" bash -c '[[ $(sort -u <<<"$0" | wc -l) -ge 2 ]]' "$out"
check "the elected route reaches the worker: session_id ends in it" \
    test "$(curl -sS -H 'Cookie: JSESSIONID=x.j3' "$B/req/session" | sed 's/.*\.//')" = j3

# ===== Activation and distance =====
out=$(many 30 "$B/act/whoami")
check "activation: only the active member balanced" test "$(count "$out" j1)" = 30
check "activation D: still serves its sessions" \
    test "$(curl -sS -H 'Cookie: JSESSIONID=x.j2' "$B/act/whoami")" = j2
check "activation S: not even its sessions" \
    test "$(curl -sS -H 'Cookie: JSESSIONID=x.j3' "$B/act/whoami")" = j1
out=$(many 20 "$B/near/whoami")
check "distance: the far member idle while a near one serves" test "$(count "$out" j1)" = 20
out=$(many 20 "$B/far/whoami")
check "distance: the far member serves when the near ones are down" test "$(count "$out" j3)" = 20
check "distance: the near ones are in ERROR" bash -c 'grep -q "jd1:ERROR.* jd2:ERROR" <<<"$0"' \
    "$(last_election lbfar)"

# ===== A member down =====
out=$(codes 30 "$B/fail/whoami")
check "a member down: every request served" test "$(sort -u <<<"$out")" = 200
check "a session pinned to it: served by another" \
    bash -c '[[ $(curl -sS -H "Cookie: JSESSIONID=x.jd1" "$0") =~ ^j[12]$ ]]' "$B/fail/whoami"
check "... with sticky_session_force: 503" \
    test "$(codes 1 "$B/force/whoami" -H 'Cookie: JSESSIONID=x.jd1')" = 503
check "... with sticky_session_force, no session: served" test "$(curl -sS "$B/force/whoami")" = j2

# ===== fail_on_status =====
check "fail_on_status -500: the client gets the 500" \
    test "$(codes 1 "$B/req/flaky/500" -H 'Cookie: JSESSIONID=x.j1')" = 500
curl -sS -o /dev/null "$B/req/whoami"
check "fail_on_status -500: j1 stays OK" bash -c 'grep -q " j1:OK/" <<<"$0"' "$(last_election lbreq)"
out=$(curl -sS -H 'Cookie: JSESSIONID=x.j1' -w ' %{http_code}' "$B/req/flaky/503" | tr -d '\n')
check "fail_on_status 503: another worker answers ($out)" bash -c '[[ $0 =~ ^j[23]\ 200$ ]]' "$out"
curl -sS -o /dev/null "$B/req/whoami"
check "fail_on_status 503: j1 in ERROR" bash -c 'grep -q " j1:ERROR/" <<<"$0"' "$(last_election lbreq)"
sleep 2.5
check "after recover_time: j1 probed and back" \
    test "$(curl -sS -H 'Cookie: JSESSIONID=x.j1' "$B/req/whoami")" = j1

# ===== A worker killed, then back =====
many 20 "$B/req/whoami" >/dev/null # connections to j2 in the pool
kill -9 "${wpid[j2]}"
wait "${wpid[j2]}" 2>/dev/null
out=$(many 40 "$B/req/whoami" -H 'Cookie: JSESSIONID=x.j2' -w '%{http_code}\n')
check "j2 killed: its sessions served elsewhere, no errors" \
    bash -c '! grep -qvE "^(j1|j3|200)$" <<<"$0" && [[ $(grep -c "^200$" <<<"$0") == 40 ]]' "$out"
check "j2 killed: in ERROR" bash -c 'grep -q " j2:ERROR/" <<<"$0"' "$(last_election lbreq)"
restart_worker j2
sleep 2.5
check "j2 back: after recover_time its sessions return" \
    test "$(curl -sS -H 'Cookie: JSESSIONID=x.j2' "$B/req/whoami")" = j2

# ===== A worker dies with the request =====
before=$(grep -c "trying another worker after j3" "$work/error_log")
out=$(curl -sS -H 'Cookie: JSESSIONID=x.j3' -w ' %{http_code}' "$B/req/die?who=j3" | tr -d '\n')
check "dies on a GET: another worker answers ($out)" bash -c '[[ $0 =~ ^j[12]\ 200$ ]]' "$out"
check "dies on a GET: logged as failed over" \
    test "$(grep -c "trying another worker after j3" "$work/error_log")" -gt "$before"
restart_worker j3
sleep 2.5
curl -sS -o /dev/null -H 'Cookie: JSESSIONID=x.j3' "$B/req/whoami"
before=$(grep -c "trying another worker after j3" "$work/error_log")
check "dies on a POST: 502" \
    test "$(codes 1 "$B/req/die?who=j3" -H 'Cookie: JSESSIONID=x.j3' --data-binary x)" = 502
check "dies on a POST: not sent to another worker" \
    test "$(grep -c "trying another worker after j3" "$work/error_log")" = "$before"
restart_worker j3

# ===== Nothing left busy =====
sleep 0.5
curl -sS -o /dev/null "$B/busy/whoami"
line=$(last_election lbbusy)
echo "# $line"
check "busy back to 0 for every worker" bash -c '[[ $(grep -o "busy=[0-9]*" <<<"$0" | sort -u) == busy=0 ]]' "$line"

# ===== Shut down =====
check "httpd stops cleanly (no crash, no sanitizer report)" httpd_stop "$work"
HTTPD_PID=
stop_workers

((fails == 0))
