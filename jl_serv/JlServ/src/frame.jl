# Framing and the HELLO handshake; must match libjlproto/include/jlp.h.
#
# Frame header, 12 bytes, big-endian:
#   magic 0x4A | major 0x02 | type u8 | flags u8 | req_id u32 | len u32

const MAGIC = 0x4A
const PROTO_MAJOR = 0x02
const PROTO_MINOR = 0x0000
const FRAME_HDR = 12
const MAX_FRAME_DEFAULT = 65536
const MAX_FRAME_MIN = 8192
const MAX_FRAME_MAX = 1 << 20

@enum FrameType::UInt8 begin
    T_HELLO = 0x01
    T_HELLO_ACK = 0x02
    T_FORWARD_REQUEST = 0x10
    T_BODY_CHUNK = 0x11
    T_GET_BODY_CHUNK = 0x12
    T_SEND_HEADERS = 0x13
    T_END_RESPONSE = 0x14
    T_CPING = 0x20
    T_CPONG = 0x21
    T_SHUTDOWN = 0x22
    T_ERROR = 0x23
end

const F_LAST = 0x01
const F_FLUSH = 0x02

const FEAT_BYTE_ALIGNED = 0x0001
const FEAT_SECRET = 0x0002

const HELLO_OK = 0x0000
const HELLO_SCHEMA_MISMATCH = 0x0001
const HELLO_BAD_SECRET = 0x0002
const HELLO_UNSUPPORTED = 0x0003

"""A protocol violation by the peer; the connection is closed."""
struct ProtocolError <: Exception
    msg::String
end

struct FrameHeader
    type::UInt8
    flags::UInt8
    req_id::UInt32
    len::UInt32
end

@inline function put_u16!(b::AbstractVector{UInt8}, i::Int, v::Integer)
    @inbounds b[i] = (v >> 8) % UInt8
    @inbounds b[i+1] = v % UInt8
    return nothing
end

@inline function put_u32!(b::AbstractVector{UInt8}, i::Int, v::Integer)
    @inbounds b[i] = (v >> 24) % UInt8
    @inbounds b[i+1] = (v >> 16) % UInt8
    @inbounds b[i+2] = (v >> 8) % UInt8
    @inbounds b[i+3] = v % UInt8
    return nothing
end

@inline get_u16(b::AbstractVector{UInt8}, i::Int) = @inbounds (UInt16(b[i]) << 8) | b[i+1]
@inline get_u32(b::AbstractVector{UInt8}, i::Int) =
    @inbounds (UInt32(b[i]) << 24) | (UInt32(b[i+1]) << 16) | (UInt32(b[i+2]) << 8) | b[i+3]

"""Writes a frame header at `b[i:i+11]`."""
function pack_header!(b::AbstractVector{UInt8}, i::Int, type::FrameType, flags::UInt8, req_id::UInt32, len::Integer)
    @inbounds b[i] = MAGIC
    @inbounds b[i+1] = PROTO_MAJOR
    @inbounds b[i+2] = UInt8(type)
    @inbounds b[i+3] = flags
    put_u32!(b, i + 4, req_id)
    put_u32!(b, i + 8, len)
    return nothing
end

function unpack_header(b::AbstractVector{UInt8}, max_frame::Integer)
    (b[1] == MAGIC && b[2] == PROTO_MAJOR) ||
        throw(ProtocolError("bad frame magic/version $(string(b[1]; base = 16)) $(string(b[2]; base = 16))"))
    h = FrameHeader(b[3], b[4], get_u32(b, 5), get_u32(b, 9))
    h.len > max_frame && throw(ProtocolError("frame of $(h.len) bytes exceeds max $max_frame"))
    return h
end

struct Hello
    minor::UInt16
    features::UInt16
    max_frame::UInt32
    schema_sha256::Vector{UInt8}
    secret::Vector{UInt8}
    agent::String
end

function unpack_hello(p::AbstractVector{UInt8})
    n = length(p)
    n >= 42 || throw(ProtocolError("HELLO too short"))
    sl = Int(get_u16(p, 41))
    n >= 44 + sl || throw(ProtocolError("HELLO secret truncated"))
    al = Int(get_u16(p, 43 + sl))
    n == 44 + sl + al || throw(ProtocolError("HELLO agent length mismatch"))
    return Hello(get_u16(p, 1), get_u16(p, 3), get_u32(p, 5), p[9:40], p[43:42+sl], String(p[45+sl:44+sl+al]))
end

function pack_hello(h::Hello)
    sl, al = length(h.secret), ncodeunits(h.agent)
    b = Vector{UInt8}(undef, 44 + sl + al)
    put_u16!(b, 1, h.minor)
    put_u16!(b, 3, h.features)
    put_u32!(b, 5, h.max_frame)
    b[9:40] = h.schema_sha256
    put_u16!(b, 41, sl)
    b[43:42+sl] = h.secret
    put_u16!(b, 43 + sl, al)
    b[45+sl:end] = codeunits(h.agent)
    return b
end

function pack_hello_ack(status::UInt16, features::UInt16, max_frame::Integer, pid::Integer, name::AbstractString)
    nl = ncodeunits(name)
    b = Vector{UInt8}(undef, 14 + nl)
    put_u16!(b, 1, status)
    put_u16!(b, 3, features)
    put_u32!(b, 5, max_frame)
    put_u32!(b, 9, pid)
    put_u16!(b, 13, nl)
    b[15:end] = codeunits(name)
    return b
end

"""Unsigned LEB128, as EXI writes unsigned integers in byte-aligned mode."""
function put_uvarint!(b::AbstractVector{UInt8}, i::Int, v::UInt64)
    while v >= 0x80
        @inbounds b[i] = (v % UInt8) | 0x80
        v >>= 7
        i += 1
    end
    @inbounds b[i] = v % UInt8
    return i + 1
end

uvarint_len(v::UInt64) = v == 0 ? 1 : cld(64 - leading_zeros(v), 7)

"""`(value, next_index)`, or `nothing` if truncated or longer than 10 bytes."""
function get_uvarint(b::AbstractVector{UInt8}, i::Int, last::Int)
    v = UInt64(0)
    shift = 0
    while i <= last && shift < 70
        x = @inbounds b[i]
        v |= UInt64(x & 0x7f) << shift
        i += 1
        x & 0x80 == 0 && return (v, i)
        shift += 7
    end
    return nothing
end
