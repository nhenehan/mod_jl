# Request heads and headers as JlServ hands them to handlers.

"""
    Headers

An ordered list of `name => value` pairs. Names of known headers are the
canonical lowercase name; others are kept as sent. Lookup ignores case.
Header values that were not UTF-8 on the wire arrive as `String`s holding the
raw bytes (`isvalid` is false).
"""
struct Headers <: AbstractVector{Pair{String,String}}
    pairs::Vector{Pair{String,String}}
end

Headers() = Headers(Pair{String,String}[])
Headers(ps::AbstractVector{<:Pair}) = Headers([String(k) => String(v) for (k, v) in ps])

Base.size(h::Headers) = size(h.pairs)
Base.getindex(h::Headers, i::Int) = h.pairs[i]
Base.push!(h::Headers, p::Pair) = (push!(h.pairs, String(p.first) => String(p.second)); h)
Base.empty!(h::Headers) = (empty!(h.pairs); h)

"""The first value of header `name` (any case), or `default`."""
function header(h::Headers, name::AbstractString, default = nothing)
    for (k, v) in h.pairs
        (k === name || lowercase_eq(k, name)) && return v
    end
    return default
end

"""All values of header `name` (any case), in order."""
headers(h::Headers, name::AbstractString) = [v for (k, v) in h.pairs if lowercase_eq(k, name)]

hasheader(h::Headers, name::AbstractString) = header(h, name) !== nothing

function lowercase_eq(a::AbstractString, b::AbstractString)
    ncodeunits(a) == ncodeunits(b) || return false
    ca, cb = codeunits(a), codeunits(b)
    @inbounds for i in eachindex(ca)
        x, y = ca[i], cb[i]
        x == y && continue
        (UInt8('A') <= x <= UInt8('Z') ? x + 0x20 : x) == (UInt8('A') <= y <= UInt8('Z') ? y + 0x20 : y) || return false
    end
    return true
end

"""TLS details forwarded by mod_ssl, when the client connection used TLS."""
Base.@kwdef mutable struct SslInfo
    cipher::Union{Nothing,String} = nothing
    key_size::Union{Nothing,Int} = nothing
    protocol::Union{Nothing,String} = nothing
    session_id::Union{Nothing,String} = nothing
    client_cert::Union{Nothing,Vector{UInt8}} = nothing  # DER
    chain::Vector{Vector{UInt8}} = Vector{UInt8}[]
end

"""
    RequestHead

Everything a ForwardRequest carries except the streamed body. `target` is the
path as the client sent it (still %-encoded); `query` excludes the `?`.
`body_length` is -1 for a chunked body and `nothing` when there is no body.
"""
Base.@kwdef mutable struct RequestHead
    method::String = "GET"
    target::String = "/"
    query::Union{Nothing,String} = nothing
    headers::Headers = Headers()
    proto::UInt16 = 0x03e9
    is_ssl::Bool = false
    remote_addr::String = ""
    remote_port::UInt16 = 0x0000
    local_addr::Union{Nothing,String} = nothing
    local_port::Union{Nothing,UInt16} = nothing
    server_name::String = ""
    server_port::UInt16 = 0x0000
    auth_type::Union{Nothing,String} = nothing
    remote_user::Union{Nothing,String} = nothing
    route::Union{Nothing,String} = nothing
    body_length::Union{Nothing,Int64} = nothing
    ssl::Union{Nothing,SslInfo} = nothing
    env::Vector{Pair{String,String}} = Pair{String,String}[]
    inline_body::Union{Nothing,Vector{UInt8}} = nothing
end
