#!/usr/bin/env bash
# The httpd examples in examples/httpd/ still load: httpd starts with each one
# included, its balancer and mount resolve, and a request reaches mod_jl (a
# 503, since no worker runs).
#
#   tests/httpd/test_examples.sh MODULE SRCDIR
set -uo pipefail
module=$1
src=$2
here=$(cd "$(dirname "$0")" && pwd)
source "$here/run-httpd.sh"

fails=0
check() { # name, command...
    local name=$1
    shift
    if "$@"; then echo "ok   $name"; else echo "FAIL $name"; fails=$((fails + 1)); fi
}

for example in "$src"/examples/httpd/*.conf; do
    name=$(basename "$example")
    mount=$(awk '$1 == "JlMount" { sub(/\/\*$/, "", $2); print $2; exit }' "$example")
    lb=$(awk '$1 == "JlBalancer" { print $2; exit }' "$example")
    work=$(mktemp -d "${TMPDIR:-/tmp}/jl-ex.XXXXXX")
    cat >"$work/conf.in" <<CONF
ServerRoot "@WORK@"
DefaultRuntimeDir "@WORK@/run"
PidFile "@WORK@/run/httpd.pid"
Mutex file:@WORK@/run default
ServerName localhost
Listen 127.0.0.1:@PORT@
LoadModule mpm_event_module /usr/lib/apache2/modules/mod_mpm_event.so
LoadModule authz_core_module /usr/lib/apache2/modules/mod_authz_core.so
LoadModule slotmem_shm_module /usr/lib/apache2/modules/mod_slotmem_shm.so
LoadModule jl_module "@MODULE@"
ErrorLog "@WORK@/error_log"
LogLevel warn jl:debug
DocumentRoot "@WORK@/htdocs"
Include "$example"
CONF
    if JL_T_CONF=$work/conf.in httpd_start "$work" "$module"; then
        check "$name: httpd starts" true
        check "$name: balancer $lb has its members" grep -q "JlBalancer $lb (.*): member" "$work/error_log"
        check "$name: $mount/* mounted" grep -q "JlMount $mount/\* -> $lb (balancer)" "$work/error_log"
        check "$name: a request reaches mod_jl (503, no workers)" \
            test "$(curl -s -o /dev/null -w '%{http_code}' --max-time 30 "http://127.0.0.1:$HTTPD_PORT$mount/x")" = 503
        check "$name: httpd stops cleanly" httpd_stop "$work"
    else
        check "$name: httpd starts" false
    fi
    HTTPD_PID=
    ((fails)) && echo "kept $work" || rm -rf "$work"
done
((fails == 0))
