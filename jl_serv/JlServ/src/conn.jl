# One connection from mod_jl: frame reading, coalesced frame writing, and the
# request loop. One request is in flight at a time (as in AJP); mod_jl's pool
# provides concurrency.
#
# Output is staged in `wbuf` and written with one call per flush. A BodyChunk
# frame is built in place: `CHUNK_RESERVE` bytes are kept free in front of its
# data, and the header and EXI prefix are written right-aligned against the data
# when the chunk is closed (their length depends on the data length's uvarint).

const UVARINT_RESERVE = 3  # enough for any length below 2^21 > MAX_FRAME_MAX
const CHUNK_RESERVE = FRAME_HDR + length(BODY_PREFIX) + UVARINT_RESERVE

"""Most data bytes one BodyChunk frame may carry with frame limit `max_frame`."""
max_chunk_data(max_frame::Integer) = max_frame - length(BODY_PREFIX) - UVARINT_RESERVE

"""What `payload` returns: a view into the receive buffer."""
const RBufView = typeof(view(UInt8[], 1:0))

mutable struct Conn{S<:IO}
    const io::S
    const server::Any  # Server; untyped to break the include cycle
    max_frame::Int
    rbuf::Vector{UInt8}
    wbuf::Vector{UInt8}
    out_start::Int  # pending output is wbuf[out_start:wpos-1]
    wpos::Int
    chunk_at::Int   # start of the open BodyChunk's reserved space, or 0
    chunk_flags::UInt8
    req_id::UInt32
    const wlock::ReentrantLock
    busy::Bool
    closed::Bool
    peer::String
    # Reused for every message on this connection (J.reset!); the decoder is
    # made by the first ForwardRequest, since it starts from a stream.
    dec::Union{Nothing,J.Decoder{RBufView}}
    const enc::J.Encoder
    const qstack::Vector{J.QNameId}
end

function Conn(io::S, server, max_frame::Integer, peer::AbstractString = "") where {S<:IO}
    c = Conn{S}(io, server, Int(max_frame), UInt8[], UInt8[], 1, 1, 0, 0x00, 0x00000000, ReentrantLock(), false,
        false, String(peer), nothing, new_encoder(), J.QNameId[])
    size_buffers!(c, max_frame)
    return c
end

function size_buffers!(c::Conn, max_frame::Integer)
    c.max_frame = Int(max_frame)
    resize!(c.rbuf, FRAME_HDR + c.max_frame)
    resize!(c.wbuf, 2 * (FRAME_HDR + c.max_frame) + 64)
    return c
end

# ===== Reading =====

"""Reads one frame into `rbuf`; the payload is `rbuf[13:12+len]`."""
function read_frame!(c::Conn)
    b = c.rbuf
    GC.@preserve b unsafe_read(c.io, pointer(b), FRAME_HDR)
    h = unpack_header(b, c.max_frame)
    h.len > 0 && GC.@preserve b unsafe_read(c.io, pointer(b, FRAME_HDR + 1), h.len)
    return h
end

payload(c::Conn, h::FrameHeader) = view(c.rbuf, FRAME_HDR+1:FRAME_HDR+Int(h.len))

"""Decodes the ForwardRequest in `bytes` with the connection's decoder."""
function decode_forward(c::Conn, bytes::RBufView)
    d = c.dec
    d = d === nothing ? (c.dec = J.Decoder(bytes, TABLES; options = OPTS)) : J.reset!(d, bytes)
    return decode_forward!(d, c.qstack)
end

"""The connection's encoder, ready for the next message. What `J.finish!`
returns is overwritten by the next use, so copy it out (`put_frame!` does)."""
message_encoder(c::Conn) = J.reset!(c.enc)

# ===== Writing =====

function flush_socket!(c::Conn)
    close_chunk!(c)
    n = c.wpos - c.out_start
    if n > 0
        b = c.wbuf
        GC.@preserve b unsafe_write(c.io, pointer(b, c.out_start), n)
    end
    c.out_start = 1
    c.wpos = 1
    return nothing
end

"""Makes room for `n` more bytes in `wbuf`, writing out what is pending if needed."""
function reserve!(c::Conn, n::Int)
    c.wpos + n - 1 <= length(c.wbuf) && return nothing
    flush_socket!(c)
    n <= length(c.wbuf) || throw(ArgumentError("frame of $n bytes does not fit the write buffer"))
    return nothing
end

"""Appends a complete frame (header + `p`) to the output."""
function put_frame!(c::Conn, type::FrameType, flags::UInt8, p::AbstractVector{UInt8})
    close_chunk!(c)
    n = length(p)
    n <= c.max_frame || throw(ArgumentError("$(type) payload of $n bytes exceeds max frame $(c.max_frame)"))
    reserve!(c, FRAME_HDR + n)
    pack_header!(c.wbuf, c.wpos, type, flags, c.req_id, n)
    copyto!(c.wbuf, c.wpos + FRAME_HDR, p, 1, n)
    c.wpos += FRAME_HDR + n
    return nothing
end

function open_chunk!(c::Conn)
    reserve!(c, CHUNK_RESERVE + 1)
    c.chunk_at = c.wpos
    c.chunk_flags = 0x00
    c.wpos += CHUNK_RESERVE
    return nothing
end

"""Appends body bytes, as BodyChunk frames of at most `max_chunk_data` bytes."""
function put_body!(c::Conn, p::Ptr{UInt8}, n::Int)
    limit = max_chunk_data(c.max_frame)
    while n > 0
        c.chunk_at == 0 && open_chunk!(c)
        used = c.wpos - (c.chunk_at + CHUNK_RESERVE)
        k = min(n, limit - used, length(c.wbuf) - c.wpos + 1)
        b = c.wbuf
        GC.@preserve b unsafe_copyto!(pointer(b, c.wpos), p, k)
        c.wpos += k
        p += k
        n -= k
        if used + k == limit
            close_chunk!(c)
        elseif c.wpos > length(c.wbuf)
            flush_socket!(c)
        end
    end
    return nothing
end

function close_chunk!(c::Conn)
    c.chunk_at == 0 && return nothing
    at = c.chunk_at
    c.chunk_at = 0
    data0 = at + CHUNK_RESERVE
    len = c.wpos - data0
    if len == 0  # nothing was written; drop the reservation
        c.wpos = at
        return nothing
    end
    plen = length(BODY_PREFIX) + uvarint_len(UInt64(len))
    fstart = data0 - plen - FRAME_HDR
    pack_header!(c.wbuf, fstart, T_BODY_CHUNK, c.chunk_flags, c.req_id, plen + len)
    body_prefix!(c.wbuf, fstart + FRAME_HDR, len)
    gap = fstart - at
    if gap > 0
        if at > c.out_start  # close the gap by moving the earlier frames up
            copyto!(c.wbuf, c.out_start + gap, c.wbuf, c.out_start, at - c.out_start)
        end
        c.out_start += gap
    end
    return nothing
end

# ===== Handshake =====

"""Constant-time comparison, for the shared secret."""
function secure_eq(a::AbstractVector{UInt8}, b::AbstractVector{UInt8})
    length(a) == length(b) || return false
    acc = 0x00
    @inbounds for i in eachindex(a, b)
        acc |= a[i] ⊻ b[i]
    end
    return acc == 0x00
end

"""Reads HELLO, answers HELLO_ACK. Returns false if the peer was refused."""
function handshake!(c::Conn)
    h = read_frame!(c)
    h.type == UInt8(T_HELLO) || throw(ProtocolError("expected HELLO, got frame type $(h.type)"))
    hello = unpack_hello(payload(c, h))
    srv = c.server
    status = if hello.schema_sha256 != SCHEMA_SHA256
        HELLO_SCHEMA_MISMATCH
    elseif !secure_eq(hello.secret, srv.secret)
        HELLO_BAD_SECRET
    elseif hello.features & FEAT_BYTE_ALIGNED == 0
        HELLO_UNSUPPORTED
    else
        HELLO_OK
    end
    mf = clamp(Int(min(hello.max_frame, srv.max_frame)), MAX_FRAME_MIN, MAX_FRAME_MAX)
    feats = FEAT_BYTE_ALIGNED | (isempty(srv.secret) ? 0x0000 : FEAT_SECRET)
    ack = pack_hello_ack(status, feats, mf, getpid(), srv.name)
    lock(c.wlock) do
        put_frame!(c, T_HELLO_ACK, 0x00, ack)
        flush_socket!(c)
    end
    if status != HELLO_OK
        @warn "jlserv: refused $(c.peer) ($(hello.agent)): status $status"
        return false
    end
    status == HELLO_OK && size_buffers!(c, mf)
    return true
end

# ===== The request loop =====

"""Serves one connection until the peer closes it or the server drains."""
function serve_conn(c::Conn)
    try
        handshake!(c) || return
        while true
            h = read_frame!(c)
            if h.type == UInt8(T_FORWARD_REQUEST)
                lock(c.wlock) do
                    c.busy = true
                end
                reuse = handle_request(c, h)
                lock(c.wlock) do
                    c.busy = false
                end
                # Close only when the peer was told reuse=false. If draining
                # began after END_RESPONSE said reuse=true, the peer may
                # already have sent its next request; it is served (with
                # reuse=false) rather than reset.
                reuse || break
            elseif h.type == UInt8(T_CPING)
                lock(c.wlock) do
                    c.req_id = h.req_id
                    put_frame!(c, T_CPONG, 0x00, CPONG_DOC)
                    flush_socket!(c)
                end
            elseif h.type == UInt8(T_SHUTDOWN)
                break
            else
                throw(ProtocolError("unexpected frame type $(h.type) between requests"))
            end
        end
    catch err
        if !(err isa EOFError || err isa Base.IOError || (err isa ProtocolError && c.closed))
            @error "jlserv: connection $(c.peer) failed" exception = (err, catch_backtrace())
        end
    finally
        close_conn!(c)
    end
    return nothing
end

function close_conn!(c::Conn)
    lock(c.wlock) do
        c.closed || (c.closed = true; close(c.io))
    end
    return nothing
end

"""Tells an idle connection the server is going away (SHUTDOWN frame)."""
function notify_shutdown!(c::Conn, reason::AbstractString)
    lock(c.wlock) do
        (c.closed || c.busy) && return false
        try
            c.req_id = 0x00000000
            put_frame!(c, T_SHUTDOWN, 0x00, encode_shutdown(true, reason))
            flush_socket!(c)
        catch
        end
        return true
    end
end
