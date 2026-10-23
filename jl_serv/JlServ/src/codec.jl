# The worker's side of the jlp v2 messages: decode ForwardRequest, encode
# everything the worker sends. Options must match libjlproto's jlp_ctx_create:
# strict, byte-aligned, valueMaxLength 128, options out of band.

const OPTS = J.strict_options(; alignment = J.ALIGN_BYTE_ALIGNED, value_max_length = UInt32(128))

# A connection reuses one decoder and one encoder for all its messages
# (J.reset!), so the functions below take one; by default they make their own.
"""A new encoder for one message."""
new_encoder() = J.Encoder(TABLES, OPTS)

const HEADER_INDEX = Dict{String,UInt32}(n => UInt32(i - 1) for (i, n) in enumerate(HEADER_NAMES))
const METHOD_INDEX = Dict{String,UInt32}(n => UInt32(i - 1) for (i, n) in enumerate(METHOD_NAMES))

"""Ordinal of a known header name (any case), or `nothing`."""
function header_ordinal(name::AbstractString)
    o = get(HEADER_INDEX, name, nothing)
    o !== nothing && return o
    any(c -> 'A' <= c <= 'Z', name) || return nothing
    return get(HEADER_INDEX, lowercase(name), nothing)
end

# ===== Fixed templates, learnt from the encoder and checked (as in C) =====

function simple_doc(q::J.QNameId; attr = nothing, value = nothing, binary = nothing)
    enc = J.Encoder(TABLES, OPTS)
    J.start_document!(enc)
    J.start_element!(enc, q)
    attr === nothing || J.attribute!(enc, attr, value)
    binary === nothing || J.characters!(enc, binary)
    J.end_element!(enc)
    J.end_document!(enc)
    return J.finish!(enc)
end

"""The bytes before `uvarint(v)` in documents of the form prefix ‖ uvarint(v) [‖ v bytes]."""
function learn_template(q::J.QNameId, binary::Bool)
    doc(v) = binary ? simple_doc(q; binary = UInt8[(k * 131 + 7) % UInt8 for k in 0:v-1]) :
             simple_doc(q; attr = LN_MAX, value = UInt64(v))
    d0 = doc(0)
    d0[end] == 0x00 || error("jlp: $(q) is not a prefix + uvarint document")
    prefix = d0[1:end-1]
    for v in (1, 127, 128, 255, 16383, 16384, 65535, 65536)
        d = doc(v)
        vi = zeros(UInt8, 10)
        vn = put_uvarint!(vi, 1, UInt64(v)) - 1
        payload = binary ? UInt8[(k * 131 + 7) % UInt8 for k in 0:v-1] : UInt8[]
        d == vcat(prefix, vi[1:vn], payload) || error("jlp: template check failed for $(q) at $v")
    end
    return prefix
end

const BODY_PREFIX = learn_template(LN_BODY_CHUNK, true)
const GET_BODY_PREFIX = learn_template(LN_GET_BODY_CHUNK, false)
const CPING_DOC = simple_doc(LN_CPING)
const CPONG_DOC = simple_doc(LN_CPONG)
const END_REUSE_DOC = simple_doc(LN_END_RESPONSE; attr = LN_REUSE, value = true)
const END_NOREUSE_DOC = simple_doc(LN_END_RESPONSE; attr = LN_REUSE, value = false)

"""Payload bytes before `len` bytes of body data in a BodyChunk frame."""
body_overhead(len::Integer) = length(BODY_PREFIX) + uvarint_len(UInt64(len))

"""Writes the BodyChunk prefix for `len` data bytes at `b[i]`; returns the index after it."""
function body_prefix!(b::AbstractVector{UInt8}, i::Int, len::Integer)
    n = length(BODY_PREFIX)
    copyto!(b, i, BODY_PREFIX, 1, n)
    return put_uvarint!(b, i + n, UInt64(len))
end

"""Data range of a BodyChunk payload `b[first:last]`, as `(start, len)`."""
function body_view(b::AbstractVector{UInt8}, first::Int, last::Int)
    n = length(BODY_PREFIX)
    ok = last - first + 1 >= n
    if ok
        @inbounds for k in 1:n
            b[first+k-1] == BODY_PREFIX[k] || (ok = false; break)
        end
    end
    ok || throw(ProtocolError("malformed BodyChunk"))
    r = get_uvarint(b, first + n, last)
    (r === nothing || r[1] != UInt64(last - r[2] + 1)) && throw(ProtocolError("malformed BodyChunk"))
    return (r[2], Int(r[1]))
end

"""Writes a GetBodyChunk payload at `b[i]`; returns the index after it."""
function get_body_chunk!(b::AbstractVector{UInt8}, i::Int, max::Integer)
    n = length(GET_BODY_PREFIX)
    copyto!(b, i, GET_BODY_PREFIX, 1, n)
    return put_uvarint!(b, i + n, UInt64(max))
end

is_cping(p::AbstractVector{UInt8}) = p == CPING_DOC

# ===== Encoding what the worker sends =====

function write_header!(enc::J.Encoder, name::AbstractString, value::AbstractString)
    if !isvalid(String, name) || !isvalid(String, value)
        J.start_element!(enc, LN_R)
        J.attribute!(enc, LN_N, Vector{UInt8}(codeunits(name)))
        J.characters!(enc, Vector{UInt8}(codeunits(value)))
    else
        o = header_ordinal(name)
        if o === nothing
            J.start_element!(enc, LN_X)
            J.attribute!(enc, LN_N, name)
        else
            J.start_element!(enc, LN_H)
            J.attribute!(enc, LN_N, J.EnumOrdinal(o))
        end
        J.characters!(enc, value)
    end
    J.end_element!(enc)
    return nothing
end

function write_headers!(enc::J.Encoder, list, q::J.QNameId)
    J.start_element!(enc, q)
    for (k, v) in list
        write_header!(enc, k, v)
    end
    J.end_element!(enc)
    return nothing
end

"""SendHeaders: status line and response headers."""
function encode_send_headers(status::Integer, reason::Union{Nothing,AbstractString}, hdrs; enc = new_encoder())
    100 <= status <= 999 || throw(ArgumentError("status $status outside 100..999"))
    J.start_document!(enc)
    J.start_element!(enc, LN_SEND_HEADERS)
    reason === nothing || J.attribute!(enc, LN_REASON, isvalid(String, reason) ? reason : "")
    J.attribute!(enc, LN_STATUS, Int(status))
    write_headers!(enc, hdrs, LN_HEADERS)
    J.end_element!(enc)
    J.end_document!(enc)
    return J.finish!(enc)
end

"""EndResponse; without trailers this is one of two constant documents."""
function encode_end_response(reuse::Bool, trailers = nothing; enc = nothing)
    (trailers === nothing || isempty(trailers)) && return reuse ? END_REUSE_DOC : END_NOREUSE_DOC
    enc === nothing && (enc = new_encoder())
    J.start_document!(enc)
    J.start_element!(enc, LN_END_RESPONSE)
    J.attribute!(enc, LN_REUSE, reuse)
    write_headers!(enc, trailers, LN_TRAILERS)
    J.end_element!(enc)
    J.end_document!(enc)
    return J.finish!(enc)
end

function encode_shutdown(graceful::Bool, reason::Union{Nothing,AbstractString} = nothing)
    enc = J.Encoder(TABLES, OPTS)
    J.start_document!(enc)
    J.start_element!(enc, LN_SHUTDOWN)
    J.attribute!(enc, LN_GRACEFUL, graceful)
    reason === nothing || J.attribute!(enc, LN_REASON, reason)
    J.end_element!(enc)
    J.end_document!(enc)
    return J.finish!(enc)
end

function encode_error(code::Integer, message::Union{Nothing,AbstractString} = nothing; enc = new_encoder())
    J.start_document!(enc)
    J.start_element!(enc, LN_ERROR)
    J.attribute!(enc, LN_CODE, UInt64(code))
    message === nothing || J.attribute!(enc, LN_MESSAGE, isvalid(String, message) ? message : "")
    J.end_element!(enc)
    J.end_document!(enc)
    return J.finish!(enc)
end

# ===== Decoding ForwardRequest =====

const NO_QNAME = J.QNameId(typemax(UInt32), typemax(UInt32))

# Integer values arrive boxed, usually as UInt64; unbox them without dispatch.
as_u16(@nospecialize v) = v isa UInt64 ? UInt16(v) : v isa Int64 ? UInt16(v) : UInt16(v::Integer)::UInt16
as_i64(@nospecialize v) = v isa UInt64 ? Int64(v) : v isa Int64 ? v : Int64(v::Integer)::Int64
bytes_string(v) = String(copy(v::Vector{UInt8}))

function set_forward_attr!(r::RequestHead, q::J.QNameId, @nospecialize(v))
    if q == LN_IS_SSL
        r.is_ssl = v::Bool
    elseif q == LN_PROTO
        r.proto = as_u16(v)
    elseif q == LN_REMOTE_ADDR
        r.remote_addr = v::String
    elseif q == LN_REMOTE_PORT
        r.remote_port = as_u16(v)
    elseif q == LN_SERVER_NAME
        r.server_name = v::String
    elseif q == LN_SERVER_PORT
        r.server_port = as_u16(v)
    elseif q == LN_ROUTE
        r.route = v::String
    elseif q == LN_BODY_LENGTH
        r.body_length = as_i64(v)
    elseif q == LN_LOCAL_ADDR
        r.local_addr = v::String
    elseif q == LN_LOCAL_PORT
        r.local_port = as_u16(v)
    elseif q == LN_AUTH_TYPE
        r.auth_type = v::String
    elseif q == LN_REMOTE_USER
        r.remote_user = v::String
    end
    return nothing
end

function set_ssl_attr!(s::SslInfo, q::J.QNameId, @nospecialize(v))
    if q == LN_CIPHER
        s.cipher = v::String
    elseif q == LN_KEY_SIZE
        s.key_size = Int(v)
    elseif q == LN_PROTOCOL
        s.protocol = v::String
    elseif q == LN_SESSION_ID
        s.session_id = v::String
    end
    return nothing
end

"""
    decode_forward(bytes) -> RequestHead

Decodes a ForwardRequest payload. `bytes` may be a view into a receive buffer;
nothing returned refers to it.
"""
decode_forward(bytes::AbstractVector{UInt8}) = decode_forward!(J.Decoder(bytes, TABLES; options = OPTS), J.QNameId[])

"""Decodes the ForwardRequest `dec` was reset to, with `stack` as scratch."""
function decode_forward!(dec::J.Decoder, stack::Vector{J.QNameId})
    r = RequestHead()
    empty!(stack)
    hname = ""
    hord = -1
    hdone = true
    while true
        k = J.next!(dec)
        if k == J.EV_SE
            q = J.event_qname_id(dec)
            isempty(stack) && q != LN_FORWARD_REQUEST && throw(ProtocolError("expected ForwardRequest"))
            push!(stack, q)
            if q == LN_H || q == LN_X || q == LN_R || q == LN_ENV
                hdone = false
                hord = -1
            elseif q == LN_SSL
                r.ssl = SslInfo()
            end
        elseif k == J.EV_AT
            q = J.event_qname_id(dec)
            v = J.event_value(dec)
            top = @inbounds stack[end]
            if top == LN_FORWARD_REQUEST
                set_forward_attr!(r, q, v)
            elseif top == LN_H
                hord = Int((v::J.EnumOrdinal).ordinal)
            elseif top == LN_X || top == LN_ENV
                hname = v::String
            elseif top == LN_R
                hname = bytes_string(v)
            elseif top == LN_SSL
                set_ssl_attr!(r.ssl::SslInfo, q, v)
            end
        elseif k == J.EV_CH
            top = @inbounds stack[end]
            v = J.event_value(dec)
            if top == LN_H
                push!(r.headers.pairs, HEADER_NAMES[hord+1] => v::String)
                hdone = true
            elseif top == LN_X
                push!(r.headers.pairs, hname => v::String)
                hdone = true
            elseif top == LN_R
                push!(r.headers.pairs, hname => bytes_string(v))
                hdone = true
            elseif top == LN_METHOD
                r.method = METHOD_NAMES[Int((v::J.EnumOrdinal).ordinal)+1]
            elseif top == LN_EXT_METHOD
                r.method = v::String
            elseif top == LN_URI
                r.target = v::String
            elseif top == LN_URI_RAW
                r.target = bytes_string(v)
            elseif top == LN_QUERY
                r.query = v::String
            elseif top == LN_ENV
                push!(r.env, hname => v::String)
                hdone = true
            elseif top == LN_BODY
                r.inline_body = v::Vector{UInt8}
            elseif top == LN_CLIENT_CERT
                (r.ssl::SslInfo).client_cert = v::Vector{UInt8}
            elseif top == LN_CHAIN_CERT
                push!((r.ssl::SslInfo).chain, v::Vector{UInt8})
            end
        elseif k == J.EV_EE
            top = pop!(stack)
            if !hdone
                if top == LN_ENV
                    push!(r.env, hname => "")
                else
                    push!(r.headers.pairs, (top == LN_H ? HEADER_NAMES[hord+1] : hname) => "")
                end
                hdone = true
            end
        elseif k == J.EV_ED || k == J.EV_DONE
            break
        end
    end
    return r
end

# ===== Encoding ForwardRequest (tests and the Julia test client only) =====

"""Encodes a RequestHead as ForwardRequest; mirrors libjlproto's jlp_encode_forward."""
function encode_forward(r::RequestHead)
    enc = J.Encoder(TABLES, OPTS)
    J.start_document!(enc)
    J.start_element!(enc, LN_FORWARD_REQUEST)
    r.auth_type === nothing || J.attribute!(enc, LN_AUTH_TYPE, r.auth_type)
    r.body_length === nothing || J.attribute!(enc, LN_BODY_LENGTH, r.body_length)
    J.attribute!(enc, LN_IS_SSL, r.is_ssl)
    r.local_addr === nothing || J.attribute!(enc, LN_LOCAL_ADDR, r.local_addr)
    r.local_port === nothing || J.attribute!(enc, LN_LOCAL_PORT, UInt64(r.local_port))
    J.attribute!(enc, LN_PROTO, UInt64(r.proto))
    J.attribute!(enc, LN_REMOTE_ADDR, r.remote_addr)
    J.attribute!(enc, LN_REMOTE_PORT, UInt64(r.remote_port))
    r.remote_user === nothing || J.attribute!(enc, LN_REMOTE_USER, r.remote_user)
    r.route === nothing || J.attribute!(enc, LN_ROUTE, r.route)
    J.attribute!(enc, LN_SERVER_NAME, r.server_name)
    J.attribute!(enc, LN_SERVER_PORT, UInt64(r.server_port))
    m = get(METHOD_INDEX, r.method, nothing)
    if m === nothing
        J.start_element!(enc, LN_EXT_METHOD)
        J.characters!(enc, r.method)
    else
        J.start_element!(enc, LN_METHOD)
        J.characters!(enc, J.EnumOrdinal(m))
    end
    J.end_element!(enc)
    if isvalid(String, r.target)
        J.start_element!(enc, LN_URI)
        J.characters!(enc, r.target)
    else
        J.start_element!(enc, LN_URI_RAW)
        J.characters!(enc, Vector{UInt8}(codeunits(r.target)))
    end
    J.end_element!(enc)
    if r.query !== nothing
        J.start_element!(enc, LN_QUERY)
        J.characters!(enc, r.query)
        J.end_element!(enc)
    end
    write_headers!(enc, r.headers, LN_HEADERS)
    s = r.ssl
    if s !== nothing
        J.start_element!(enc, LN_SSL)
        s.cipher === nothing || J.attribute!(enc, LN_CIPHER, s.cipher)
        s.key_size === nothing || J.attribute!(enc, LN_KEY_SIZE, UInt64(s.key_size))
        s.protocol === nothing || J.attribute!(enc, LN_PROTOCOL, s.protocol)
        s.session_id === nothing || J.attribute!(enc, LN_SESSION_ID, s.session_id)
        if s.client_cert !== nothing
            J.start_element!(enc, LN_CLIENT_CERT)
            J.characters!(enc, s.client_cert)
            J.end_element!(enc)
        end
        for c in s.chain
            J.start_element!(enc, LN_CHAIN_CERT)
            J.characters!(enc, c)
            J.end_element!(enc)
        end
        J.end_element!(enc)
    end
    for (k, v) in r.env
        J.start_element!(enc, LN_ENV)
        J.attribute!(enc, LN_N, k)
        J.characters!(enc, v)
        J.end_element!(enc)
    end
    if r.inline_body !== nothing
        J.start_element!(enc, LN_BODY)
        J.characters!(enc, r.inline_body)
        J.end_element!(enc)
    end
    J.end_element!(enc)
    J.end_document!(enc)
    return J.finish!(enc)
end
