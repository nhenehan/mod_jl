# Small request helpers: percent-decoding, query strings and form bodies,
# and session ids that carry the worker route.

hexval(c::UInt8) = UInt8('0') <= c <= UInt8('9') ? Int(c - UInt8('0')) :
                   UInt8('a') <= c <= UInt8('f') ? Int(c - UInt8('a')) + 10 :
                   UInt8('A') <= c <= UInt8('F') ? Int(c - UInt8('A')) + 10 : -1

"""
    unescapeuri(s; plus = false) -> String

Decodes `%XX` escapes. With `plus = true`, `+` becomes a space too, as in
query strings and `application/x-www-form-urlencoded` bodies. A `%` that
isn't followed by two hex digits is kept as it is. Escapes that don't form
valid UTF-8 give a `String` for which `isvalid` is false.
"""
function unescapeuri(s::AbstractString; plus::Bool = false)
    (occursin('%', s) || (plus && occursin('+', s))) || return String(s)
    b = codeunits(s)
    n = length(b)
    out = Vector{UInt8}(undef, 0)
    sizehint!(out, n)
    i = 1
    while i <= n
        c = b[i]
        if c == UInt8('%') && i + 2 <= n && (h = hexval(b[i+1])) >= 0 && (l = hexval(b[i+2])) >= 0
            push!(out, UInt8(h << 4 | l))
            i += 3
        else
            push!(out, plus && c == UInt8('+') ? UInt8(' ') : c)
            i += 1
        end
    end
    return String(out)
end

"""
    queryparams(req::Request)
    queryparams(s::AbstractString)

The `name => value` pairs of a query string or a form body, decoded, in
order. Repeated names are kept; `Dict(queryparams(req))` keeps the last of
each. A part without `=` gives an empty value.

```julia
queryparams("q=caf%C3%A9+au+lait&tag=a&tag=b")
# ["q" => "café au lait", "tag" => "a", "tag" => "b"]
queryparams(String(read(req.body)))   # an application/x-www-form-urlencoded body
```
"""
queryparams(req::Request) = queryparams(req.query)
queryparams(::Nothing) = Pair{String,String}[]
function queryparams(s::AbstractString)
    out = Pair{String,String}[]
    for part in eachsplit(s, '&')
        isempty(part) && continue
        k, v = occursin('=', part) ? split(part, '='; limit = 2) : (part, "")
        push!(out, unescapeuri(k; plus = true) => unescapeuri(v; plus = true))
    end
    return out
end

"""
    session_id(req) -> String

A new random session id (128 bits from the system's secure source, as hex),
with `.route` appended when the request names a worker route, as mod_jk does
with `jvmRoute`. Sticky load balancing uses that suffix to send the session
back to this worker.
"""
function session_id(req::Request)
    id = bytes2hex(open(io -> read(io, 16), "/dev/urandom"))
    route = req.route
    return route === nothing ? id : id * "." * route
end
