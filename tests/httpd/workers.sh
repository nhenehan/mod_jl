# Starting and restarting JlServ workers for the httpd tests. Source it after
# setting work, secret, julia, src and app (the application file); worker
# NAME listens on $work/NAME.sock, logs to $work/NAME.log, and gets
# JL_T_NAME=NAME. Pids are in the associative array wpid.
declare -A wpid

# launch NAME: start it, without waiting.
launch() {
    JL_T_NAME=$1 JL_SECRET=$secret "$julia" --startup-file=no --project="$src/jl_serv/JlServ" -t 4,1 \
        "$src/jl_serv/JlServ/bin/jlserv" "$app" --unix "$work/$1.sock" --name "$1" >>"$work/$1.log" 2>&1 &
    wpid[$1]=$!
}

# await NAME N: until NAME.log says "listening" more than N times.
await() {
    for _ in $(seq 1 1200); do
        [[ $(grep -c listening "$work/$1.log" 2>/dev/null) -gt $2 ]] && return 0
        kill -0 "${wpid[$1]}" 2>/dev/null || break
        sleep 0.1
    done
    echo "FAIL jlserv $1 did not start"
    cat "$work/$1.log"
    exit 1
}

restart_worker() {
    local n
    n=$(grep -c listening "$work/$1.log")
    launch "$1"
    await "$1" "$n"
}

kill_workers() {
    for p in "${wpid[@]}"; do kill -9 "$p" 2>/dev/null; done
}

# stop_workers: SIGINT, so each drains; waits for them.
stop_workers() {
    for w in "${!wpid[@]}"; do
        kill -INT "${wpid[$w]}" 2>/dev/null
        wait "${wpid[$w]}" 2>/dev/null
    done
}
