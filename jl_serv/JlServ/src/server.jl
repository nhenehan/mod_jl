# Listening, accepting, and draining.

"""
    Server

A running JlServ instance; see [`start`](@ref) and [`serve`](@ref).
"""
mutable struct Server
    const handler::Any
    const name::String
    const secret::Vector{UInt8}
    const max_frame::Int
    const listeners::Vector{Any}
    const unix_paths::Vector{Tuple{String,UInt64,UInt64}}  # (path, device, inode) of our sockets
    const conns::Set{Conn}
    const lock::ReentrantLock
    const tasks::Vector{Task}
    draining::Bool
    const stopped::Threads.Event
    @atomic interrupted::Bool  # SIGINT seen by a task other than serve's
end

server_pool() = Threads.nthreads(:interactive) > 0 ? :interactive : :default

"""
    start(handler; unix = nothing, tcp = nothing, name = "jlserv", secret = "",
          max_frame = 65536) -> Server

Starts listening without blocking. `unix` is a socket path (created with mode
0660; an existing socket file is replaced atomically, so a new process can
take over from an old one while it drains). `tcp` is a port, or
`(host, port)` (host defaults to 127.0.0.1). A secret is required for TCP
unless `insecure_tcp = true`, since the worker trusts the forwarded client
attributes. `warmup_paths`, a list of paths, runs [`warmup`](@ref) on them
before listening.
"""
function start(handler; unix::Union{Nothing,AbstractString} = nothing, tcp = nothing, name::AbstractString = "jlserv",
    secret::AbstractString = "", max_frame::Integer = MAX_FRAME_DEFAULT, insecure_tcp::Bool = false,
    warmup_paths = nothing)
    (unix === nothing && tcp === nothing) && throw(ArgumentError("give unix = path and/or tcp = port"))
    warmup_paths === nothing || warmup(handler; paths = warmup_paths)
    tcp !== nothing && isempty(secret) && !insecure_tcp &&
        throw(ArgumentError("a TCP listener needs a secret (or insecure_tcp = true)"))
    mf = clamp(Int(max_frame), MAX_FRAME_MIN, MAX_FRAME_MAX)
    srv = Server(handler, String(name), Vector{UInt8}(secret), mf, Any[], Tuple{String,UInt64,UInt64}[], Set{Conn}(),
        ReentrantLock(), Task[], false, Threads.Event(), false)
    unix === nothing || push!(srv.listeners, listen_unix!(srv, String(unix)))
    if tcp !== nothing
        host, port = tcp isa Integer ? (Sockets.localhost, tcp) : (Sockets.getaddrinfo(string(tcp[1])), tcp[2])
        push!(srv.listeners, Sockets.listen(host, port))
    end
    for l in srv.listeners
        push!(srv.tasks, Threads.@spawn server_pool() accept_loop(srv, l))
    end
    return srv
end

"""
Listens on a unix socket at `path`, replacing whatever is there without a
moment when the path is missing: it binds a name of its own, then renames it
into place. A new process can so take over from one still draining, and
connections go to the new one at once. libuv removes the bound name when the
listener closes, which after the rename is a name nobody uses; `shutdown!`
removes `path` itself only if it is still ours.
"""
function listen_unix!(srv::Server, path::String)
    tmp = "$path.$(getpid())"
    if sizeof(tmp) >= 108  # sun_path: no room for the extra name
        ispath(path) && rm(path)
        l = Sockets.listen(path)
        chmod(path, 0o660)
    else
        ispath(tmp) && rm(tmp)
        l = Sockets.listen(tmp)
        chmod(tmp, 0o660)
        Base.systemerror(:rename, ccall(:rename, Cint, (Cstring, Cstring), tmp, path) != 0)
    end
    st = stat(path)
    push!(srv.unix_paths, (path, UInt64(st.device), UInt64(st.inode)))
    return l
end

function release_unix_paths!(srv::Server)
    for (path, dev, ino) in srv.unix_paths
        st = stat(path)
        if ispath(st) && UInt64(st.device) == dev && UInt64(st.inode) == ino
            try rm(path) catch end
        end
    end
    empty!(srv.unix_paths)
    return nothing
end

function accept_loop(srv::Server, l)
    while !srv.draining
        sock = try
            Sockets.accept(l)
        catch err
            srv.draining && break
            err isa Base.IOError && break
            if err isa InterruptException
                # SIGINT is thrown into whichever task runs on thread 1,
                # which may be this one; hand it to serve() to drain.
                @atomic srv.interrupted = true
                continue
            end
            @error "jlserv: accept failed" exception = err
            continue
        end
        if sock isa Sockets.TCPSocket
            Sockets.nagle(sock, false)
            Sockets.quickack(sock, true)
        end
        peer = sock isa Sockets.TCPSocket ? (try string(Sockets.getpeername(sock)) catch; "tcp" end) : "unix"
        c = Conn(sock, srv, srv.max_frame, peer)
        lock(srv.lock) do
            push!(srv.conns, c)
        end
        Threads.@spawn :default begin
            try
                serve_conn(c)
            finally
                lock(srv.lock) do
                    delete!(srv.conns, c)
                end
            end
        end
    end
    return nothing
end

"""
    shutdown!(srv; grace = 1.0, timeout = 30.0)

Graceful stop: close the listeners; send SHUTDOWN on idle connections and
close them after `grace` seconds; let in-flight requests finish (their
END_RESPONSE says reuse=false); force-close whatever is left after `timeout`.
"""
function shutdown!(srv::Server; grace::Real = 1.0, timeout::Real = 30.0)
    srv.draining && (wait(srv.stopped); return srv)
    srv.draining = true
    foreach(close, srv.listeners)
    release_unix_paths!(srv)
    for c in lock(() -> collect(srv.conns), srv.lock)
        notify_shutdown!(c, "draining")
    end
    t0 = time()
    sleep(min(grace, timeout))
    while true
        live = lock(() -> collect(srv.conns), srv.lock)
        isempty(live) && break
        for c in live
            (!c.busy || time() - t0 > timeout) && close_conn!(c)
        end
        sleep(0.05)
    end
    notify(srv.stopped)
    return srv
end

Base.wait(srv::Server) = wait(srv.stopped)

"""
    serve(handler; kw...)

Like [`start`](@ref), then blocks until the server stops. Ctrl-C (SIGINT)
drains gracefully; see [`shutdown!`](@ref).
"""
function serve(handler; drain_timeout::Real = 30.0, kw...)
    Base.exit_on_sigint(false)
    srv = start(handler; kw...)
    @info "jlserv: $(srv.name) listening" listeners = [string(l) for l in srv.listeners] threads = Threads.nthreads()
    interrupted = false
    try
        # Polled: SIGINT never reaches a task blocked in wait(), only one
        # that wakes up now and then, and it may land in an accept loop.
        stopped = Threads.@spawn wait(srv)
        while !istaskdone(stopped) && !(@atomic srv.interrupted)
            sleep(0.1)
        end
        interrupted = @atomic srv.interrupted
    catch err
        err isa InterruptException || rethrow()
        interrupted = true
    end
    if interrupted
        @info "jlserv: draining"
        shutdown!(srv; timeout = drain_timeout)
    end
    return srv
end
