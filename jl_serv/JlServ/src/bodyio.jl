# Request bodies (pulled from mod_jl with GetBodyChunk), the streaming
# response API, and running one request through the handler.

"""
    RequestBody <: IO

The request body. Bytes come first from the chunk sent inline with the
ForwardRequest, then from BodyChunk frames requested one at a time. Data read
from a frame lives in the connection's receive buffer only until the next
read, so every read copies out of it.
"""
mutable struct RequestBody{C<:Conn} <: IO
    const conn::C
    src::Vector{UInt8}  # current bytes are src[pos:last]
    pos::Int
    last::Int
    remaining::Int64    # bytes still expected, -1 if unknown (chunked)
    done::Bool          # no more chunks to ask for
    total::Int64
end

function RequestBody(c::Conn, head::RequestHead, flags::UInt8)
    inline = head.inline_body
    src = inline === nothing ? UInt8[] : inline
    n = length(src)
    bl = head.body_length
    remaining = bl === nothing ? Int64(0) : bl < 0 ? Int64(-1) : max(Int64(0), bl - n)
    done = bl === nothing || remaining == 0 || flags & F_LAST != 0
    return RequestBody{typeof(c)}(c, src, 1, n, remaining, done, n)
end

"""Makes bytes available; false at the end of the body."""
function fillbuf!(b::RequestBody)
    while b.pos > b.last
        b.done && return false
        c = b.conn
        want = max_chunk_data(c.max_frame)
        b.remaining > 0 && (want = min(want, b.remaining))
        tmp = Vector{UInt8}(undef, 16)
        put_frame!(c, T_GET_BODY_CHUNK, 0x00, view(tmp, 1:get_body_chunk!(tmp, 1, want)-1))
        flush_socket!(c)
        h = read_frame!(c)
        h.type == UInt8(T_BODY_CHUNK) || throw(ProtocolError("expected BODY_CHUNK, got frame type $(h.type)"))
        h.req_id == c.req_id || throw(ProtocolError("BODY_CHUNK for request $(h.req_id), expected $(c.req_id)"))
        start, len = body_view(c.rbuf, FRAME_HDR + 1, FRAME_HDR + Int(h.len))
        b.src = c.rbuf
        b.pos = start
        b.last = start + len - 1
        b.total += len
        b.remaining >= 0 && (b.remaining = max(Int64(0), b.remaining - len))
        (len == 0 || h.flags & F_LAST != 0 || b.remaining == 0) && (b.done = true)
    end
    return true
end

Base.eof(b::RequestBody) = !fillbuf!(b)
Base.isopen(b::RequestBody) = !(b.done && b.pos > b.last)
Base.bytesavailable(b::RequestBody) = b.last - b.pos + 1
Base.close(b::RequestBody) = nothing

function Base.read(b::RequestBody, ::Type{UInt8})
    fillbuf!(b) || throw(EOFError())
    x = @inbounds b.src[b.pos]
    b.pos += 1
    return x
end

function Base.unsafe_read(b::RequestBody, p::Ptr{UInt8}, n::UInt)
    while n > 0
        fillbuf!(b) || throw(EOFError())
        k = min(n, UInt(b.last - b.pos + 1))
        GC.@preserve b unsafe_copyto!(p, pointer(b.src, b.pos), k)
        b.pos += Int(k)
        p += k
        n -= k
    end
    return nothing
end

function Base.readbytes!(b::RequestBody, a::AbstractVector{UInt8}, nb::Integer = length(a))
    got = 0
    while got < nb && fillbuf!(b)
        k = min(nb - got, b.last - b.pos + 1)
        got + k > length(a) && resize!(a, got + k)
        copyto!(a, got + 1, b.src, b.pos, k)
        b.pos += k
        got += k
    end
    return got
end

function Base.readavailable(b::RequestBody)
    fillbuf!(b) || return UInt8[]
    a = b.src[b.pos:b.last]
    b.pos = b.last + 1
    return a
end

function Base.read(b::RequestBody)
    out = UInt8[]
    b.remaining > 0 && sizehint!(out, Int(b.remaining + bytesavailable(b)))
    while fillbuf!(b)
        append!(out, view(b.src, b.pos:b.last))
        b.pos = b.last + 1
    end
    return out
end

"""Reads and drops whatever is left of the body."""
function discard!(b::RequestBody)
    while fillbuf!(b)
        b.pos = b.last + 1
    end
    return nothing
end

# ===== Request and Response =====

"""
    Request

What a handler receives: the head fields (`method`, `target`, `query`,
`headers`, `remote_addr`, `route`, ... see [`RequestHead`](@ref)), the
streamed `body::RequestBody`, and `id`, the frame request id (for logs).
"""
struct Request
    head::RequestHead
    body::RequestBody
    id::UInt32
end

function Base.getproperty(r::Request, s::Symbol)
    (s === :head || s === :body || s === :id) && return getfield(r, s)
    return getproperty(getfield(r, :head), s)
end

Base.propertynames(r::Request) = (:head, :body, :id, fieldnames(RequestHead)...)

"""
    Response(status = 200, headers = [], body = UInt8[]; reason = nothing)
    Response(body::AbstractString)

`body` is bytes, a string, or an `IO`, which is read to its end (not at all
for HEAD) and then closed. `content-length` is added for bytes and strings
when the handler did not set it.
"""
struct Response
    status::Int
    headers::Vector{Pair{String,String}}
    body::Any
    reason::Union{Nothing,String}
end

Response(status::Integer = 200, headers = Pair{String,String}[], body = UInt8[]; reason = nothing) =
    Response(Int(status), [String(k) => String(v) for (k, v) in headers], body, reason)
Response(body::AbstractString) = Response(200, Pair{String,String}["content-type" => "text/plain; charset=utf-8"], body, nothing)

# ===== Streaming =====

"""
    Stream <: IO

The streaming handler API, modelled on `HTTP.Stream`: `setstatus!`,
`setheader!`, `startwrite`, then `write` / `flush` (sends what is buffered,
flagged so httpd flushes it to the client too); reading reads the request body.
The response ends when the handler returns.
"""
mutable struct Stream{C<:Conn} <: IO
    const conn::C
    const request::Request
    status::Int
    reason::Union{Nothing,String}
    headers::Vector{Pair{String,String}}
    started::Bool
    is_head::Bool
end

Stream(c::C, req::Request) where {C<:Conn} =
    Stream{C}(c, req, 200, nothing, Pair{String,String}[], false, req.method == "HEAD")

setstatus!(s::Stream, status::Integer, reason = nothing) = (s.status = Int(status); s.reason = reason; s)
setheader!(s::Stream, p::Pair) = (push!(s.headers, String(p.first) => String(p.second)); s)

function startwrite(s::Stream)
    s.started && return s
    s.started = true
    c = s.conn
    put_frame!(c, T_SEND_HEADERS, 0x00, encode_send_headers(s.status, s.reason, s.headers; enc = message_encoder(c)))
    return s
end

function Base.unsafe_write(s::Stream, p::Ptr{UInt8}, n::UInt)
    startwrite(s)
    s.is_head || put_body!(s.conn, p, Int(n))
    return Int(n)
end

Base.write(s::Stream, x::UInt8) = unsafe_write(s, Ref(x), UInt(1))

function Base.flush(s::Stream)
    startwrite(s)
    c = s.conn
    c.chunk_at != 0 && (c.chunk_flags |= F_FLUSH)
    flush_socket!(c)
    return nothing
end

Base.eof(s::Stream) = eof(s.request.body)
Base.bytesavailable(s::Stream) = bytesavailable(s.request.body)
Base.unsafe_read(s::Stream, p::Ptr{UInt8}, n::UInt) = unsafe_read(s.request.body, p, n)
Base.read(s::Stream, ::Type{UInt8}) = read(s.request.body, UInt8)
Base.readavailable(s::Stream) = readavailable(s.request.body)
Base.isopen(s::Stream) = !s.conn.closed

# A body whose bytes are contiguous in memory goes out without a copy.
function write_bytes(s::Stream, b::Union{String,Vector{UInt8}})
    any(p -> lowercase_eq(p.first, "content-length"), s.headers) ||
        push!(s.headers, "content-length" => string(sizeof(b)))
    startwrite(s)
    !s.is_head && sizeof(b) > 0 && GC.@preserve b put_body!(s.conn, pointer(b), sizeof(b))
    return nothing
end

"""Writes a whole Response through a stream that has not started."""
function write_response(s::Stream, r::Response)
    s.status = r.status
    s.reason = r.reason
    append!(s.headers, r.headers)
    body = r.body
    if body isa String || body isa Vector{UInt8}
        write_bytes(s, body)
    elseif body isa AbstractString
        write_bytes(s, String(body))
    elseif body isa AbstractVector{UInt8}
        write_bytes(s, Vector{UInt8}(body))
    elseif body isa IO
        try
            startwrite(s)
            if !s.is_head
                buf = Vector{UInt8}(undef, max_chunk_data(s.conn.max_frame))
                while !eof(body)
                    n = readbytes!(body, buf)
                    n > 0 && GC.@preserve buf put_body!(s.conn, pointer(buf), n)
                end
            end
        finally
            close(body)
        end
    elseif body === nothing
        startwrite(s)
    else
        throw(ArgumentError("unsupported response body $(typeof(body))"))
    end
    return nothing
end

# ===== Running a request =====

"""
    handle(handler, stream)

How a handler is run. A plain function gets the `Request` and returns a
`Response` (or a string, or bytes); a [`Router`](@ref) dispatches per route,
and routes registered with `stream!` get the `Stream`.
"""
function handle(f, s::Stream)
    r = f(s.request)
    write_response(s, to_response(r))
    return nothing
end

to_response(r::Response) = r
to_response(r::AbstractString) = Response(r)
to_response(r::AbstractVector{UInt8}) = Response(200, ["content-type" => "application/octet-stream"], r)
to_response(::Nothing) = Response(204, Pair{String,String}[], nothing)

const ERROR_BODY = "Internal Server Error\n"

"""Handles one ForwardRequest frame. Returns whether the connection may be reused."""
function handle_request(c::Conn, h::FrameHeader)
    head = try
        decode_forward(c, payload(c, h))
    catch err
        @error "jlserv: bad ForwardRequest from $(c.peer)" exception = err
        lock(c.wlock) do
            c.req_id = h.req_id
            put_frame!(c, T_ERROR, 0x00, encode_error(400, "malformed ForwardRequest"; enc = message_encoder(c)))
            flush_socket!(c)
        end
        return false
    end
    c.req_id = h.req_id
    req = Request(head, RequestBody(c, head, h.flags), h.req_id)
    s = Stream(c, req)
    srv = c.server
    try
        handle(srv.handler, s)
    catch err
        (err isa ProtocolError || err isa EOFError || err isa Base.IOError) && rethrow()
        @error "jlserv: handler failed for $(head.method) $(head.target)" exception = (err, catch_backtrace())
        # Once headers are out, the only honest signal is to drop the
        # connection without END_RESPONSE, so httpd marks the response as
        # truncated.
        s.started && return false
        empty!(s.headers)
        write_response(s, Response(500, ["content-type" => "text/plain"], ERROR_BODY))
    end
    startwrite(s)
    reuse = !srv.draining
    put_frame!(c, T_END_RESPONSE, 0x00, encode_end_response(reuse))
    flush_socket!(c)
    ok = reuse
    return ok
end
