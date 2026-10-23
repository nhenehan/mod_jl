#!/usr/bin/env bash
# Benchmarks mod_jl: httpd + mod_jl in front of one JlServ worker on a Unix
# socket, everything at its defaults.
#
#   ct/dev.sh bench/run.sh build-rel/mod_jl/mod_jl.so
#
# Use a Release build of the module and a quiet machine. Prints a Markdown
# table and saves it as bench/results/DATE-COMMIT.md. DURATION is the length
# of each run in seconds (default 10).
set -uo pipefail
module=$(realpath "$1")
src=$(cd "$(dirname "$0")/.." && pwd)
duration=${DURATION:-10}
source "$src/tests/httpd/run-httpd.sh"
export JL_T_CONF=$src/bench/httpd.conf.in

work=$(mktemp -d "${TMPDIR:-/tmp}/jl-bench.XXXXXX")
secret=bench-$RANDOM$RANDOM
cleanup() {
    [[ -n ${HTTPD_PID:-} ]] && httpd_stop "$work" >/dev/null 2>&1
    [[ -n ${jpid:-} ]] && kill -INT "$jpid" 2>/dev/null && wait "$jpid" 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

# ===== Start the worker and httpd =====
JL_SECRET=$secret julia --startup-file=no --project="$src/jl_serv/JlServ" -t 8,1 --gcthreads=4,1 \
    "$src/jl_serv/JlServ/bin/jlserv" "$src/tests/httpd/app.jl" --unix "$work/j1.sock" --name j1 \
    --warmup /hello/world >"$work/jlserv.log" 2>&1 &
jpid=$!
until grep -q listening "$work/jlserv.log"; do
    kill -0 "$jpid" 2>/dev/null || { cat "$work/jlserv.log"; exit 1; }
    sleep 0.2
done
httpd_start "$work" "$module" "$secret" || exit 1
B="http://127.0.0.1:$HTTPD_PORT"

# ===== Measuring =====
rows=()
# wrk_row LABEL CONNECTIONS URL [wrk args...]: throughput and latency.
wrk_row() {
    local label=$1 conns=$2 url=$3
    shift 3
    wrk -t"$((conns < 4 ? conns : 4))" -c"$conns" -d"${duration}s" --latency "$@" "$url" >"$work/wrk.out" 2>&1
    local rps p50 p99
    rps=$(awk '/^Requests\/sec/ { printf "%.0f", $2 }' "$work/wrk.out")
    p50=$(awk '$1 == "50%" { print $2 }' "$work/wrk.out")
    p99=$(awk '$1 == "99%" { print $2 }' "$work/wrk.out")
    rows+=("| $label | $rps | $p50 | $p99 | |")
}

# Warm up the module's pool and the worker's code paths.
wrk -t2 -c16 -d3s "$B/hello/world" >/dev/null
wrk -t2 -c16 -d3s "$B/bytes/65536" >/dev/null

wrk_row "GET /hello/world, 1 connection" 1 "$B/hello/world"
wrk_row "GET /hello/world, 64 connections" 64 "$B/hello/world"
wrk_row "GET /bytes/65536, 64 connections" 64 "$B/bytes/65536"
wrk_row "POST /count with 1 KiB, 64 connections" 64 "$B/count" -s "$src/bench/post.lua"

# A 1 GiB download, watching the memory of httpd's child and of the worker.
rss() { ps -o rss= -p "$1" 2>/dev/null | awk '{ print $1 }'; }
child=$(pgrep -P "$HTTPD_PID" | head -1)
hmax=0 jmax=0
curl -s -o /dev/null -w '%{speed_download}' "$B/zeros/1073741824" >"$work/speed" &
cpid=$!
while kill -0 "$cpid" 2>/dev/null; do
    h=$(rss "$child") j=$(rss "$jpid")
    ((${h:-0} > hmax)) && hmax=$h
    ((${j:-0} > jmax)) && jmax=$j
    sleep 0.1
done
mbps=$(awk '{ printf "%.0f", $1 / 1e6 }' "$work/speed")
rows+=("| GET of 1 GiB, streamed | $mbps MB/s | | | peak memory: httpd $((hmax / 1024)) MiB, worker $((jmax / 1024)) MiB |")

# ===== Report =====
commit=$(git -C "$src" rev-parse --short HEAD 2>/dev/null || echo unknown)
report=$(
    echo "# mod_jl benchmark, $(date +%F), $commit"
    echo
    echo "$(nproc) cores ($(lscpu | sed -n 's/^Model name: *//p')), Linux $(uname -r)," \
        "$(apache2 -v | sed -n 's/^Server version: //p'), $(julia --version)."
    echo "httpd: event MPM, one process, 128 threads. Worker: JlServ, -t 8,1 --gcthreads=4,1, Unix socket."
    echo "Each run ${duration} s, after a warm-up."
    echo
    echo "| Workload | Requests/s | p50 | p99 | |"
    echo "|---|---|---|---|---|"
    printf '%s\n' "${rows[@]}"
)
echo "$report"
mkdir -p "$src/bench/results"
echo "$report" >"$src/bench/results/$(date +%F)-$commit.md"
