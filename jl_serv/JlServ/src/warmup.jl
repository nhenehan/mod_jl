# warmup(), which runs requests through a real connection before the worker
# listens, and an in-memory transport that replays one ForwardRequest frame
# and discards the output, for the tests.

mutable struct ReplayIO <: IO
    frame::Vector{UInt8}
    pos::Int
    written::Int
end
ReplayIO(frame) = ReplayIO(frame, 1, 0)

function Base.unsafe_read(io::ReplayIO, p::Ptr{UInt8}, n::UInt)
    for i in 1:n
        unsafe_store!(p, io.frame[io.pos], i)
        io.pos = io.pos == length(io.frame) ? 1 : io.pos + 1
    end
    return nothing
end
Base.unsafe_write(io::ReplayIO, p::Ptr{UInt8}, n::UInt) = (io.written += Int(n); Int(n))
Base.isopen(::ReplayIO) = true
Base.close(::ReplayIO) = nothing

"""A Server that isn't listening anywhere, for running requests in memory."""
detached_server(handler; name = "replay") =
    Server(handler, String(name), UInt8[], MAX_FRAME_DEFAULT, Any[], Tuple{String,UInt64,UInt64}[], Set{Conn}(),
        ReentrantLock(), Task[], false, Threads.Event(), false)

"""A ForwardRequest frame carrying `payload`, the EXI document."""
function forward_frame(payload::AbstractVector{UInt8}; req_id::UInt32 = 0x00000001)
    frame = zeros(UInt8, FRAME_HDR + length(payload))
    pack_header!(frame, 1, T_FORWARD_REQUEST, 0x00, req_id, length(payload))
    frame[FRAME_HDR+1:end] = payload
    return frame
end

"""A connection that receives `frame` over and over, for `handler`."""
replay_conn(handler, frame::Vector{UInt8}) = Conn(ReplayIO(frame), detached_server(handler), MAX_FRAME_DEFAULT)

"""
    warmup(handler; paths = ["/"])

Runs a GET for each of `paths` through `handler` before the worker listens,
so the first real requests don't wait for Julia to compile the request path.
It goes through a real connection on a temporary Unix socket, handshake
included, because everything on that path is compiled for the socket type.
Only GETs are sent, since handlers may have side effects: list paths that are
safe to request. Handler errors are logged as for any request.
"""
function warmup(handler; paths = ["/"])
    dir = mktempdir()
    l = Sockets.listen(joinpath(dir, "warmup.sock"))
    try
        client = Sockets.connect(joinpath(dir, "warmup.sock"))
        c = Conn(Sockets.accept(l), detached_server(handler; name = "warmup"), MAX_FRAME_DEFAULT, "warmup")
        t = Threads.@spawn :default serve_conn(c)
        try
            hello = Hello(PROTO_MINOR, FEAT_BYTE_ALIGNED, MAX_FRAME_DEFAULT, SCHEMA_SHA256, UInt8[], "jlserv-warmup")
            send_frame(client, T_HELLO, 0x00000000, pack_hello(hello))
            await_frame(client, T_HELLO_ACK) || return nothing
            for (i, p) in enumerate(paths)
                send_frame(client, T_FORWARD_REQUEST, UInt32(i), encode_forward(warmup_head(p)))
                await_frame(client, T_END_RESPONSE) || break
            end
        finally
            close(client)
            wait(t)
        end
    finally
        close(l)
        rm(dir; recursive = true, force = true)
    end
    return nothing
end

function warmup_head(p::AbstractString)
    target, query = occursin('?', p) ? split(p, '?'; limit = 2) : (p, nothing)
    return RequestHead(method = "GET", target = String(target), query = query === nothing ? nothing : String(query),
        headers = Headers(["host" => "warmup", "user-agent" => "jlserv-warmup"]), remote_addr = "127.0.0.1",
        server_name = "warmup")
end

function send_frame(io::IO, type::FrameType, req_id::UInt32, payload::AbstractVector{UInt8})
    b = zeros(UInt8, FRAME_HDR + length(payload))
    pack_header!(b, 1, type, 0x00, req_id, length(payload))
    b[FRAME_HDR+1:end] = payload
    write(io, b)
end

"""Reads frames until one of `type`; false if the worker closed the connection first."""
function await_frame(io::IO, type::FrameType)
    hdr = Vector{UInt8}(undef, FRAME_HDR)
    try
        while true
            read!(io, hdr)
            h = unpack_header(hdr, MAX_FRAME_MAX)
            read(io, h.len)
            h.type == UInt8(type) && return true
        end
    catch err
        (err isa EOFError || err isa Base.IOError) && return false
        rethrow()
    end
end
