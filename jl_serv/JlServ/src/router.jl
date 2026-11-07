# A small router: method + path pattern -> handler.
#
# Patterns match the request target's path segments (still %-encoded):
# `{name}` captures one segment, a trailing `*` matches the rest (captured as
# `:*`). Routes registered with `route!` get `(req, params)` and return a
# Response; routes registered with `stream!` get `(s::Stream, params)`.

struct Route
    method::String  # "" for any method
    segments::Vector{String}
    tail::Bool
    stream::Bool
    f::Any
end

"""
    Router(; fallback)

`fallback(req)` runs when nothing matches; by default a 404, or a 405 with an
`Allow` field listing the methods the path's routes take (RFC 9110 15.5.6).
"""
struct Router
    routes::Vector{Route}
    fallback::Any
end

Router(; fallback = nothing) = Router(Route[], fallback)

function add_route!(r::Router, method::AbstractString, pattern::AbstractString, f, stream::Bool)
    segs = split_path(pattern)
    tail = !isempty(segs) && segs[end] == "*"
    tail && pop!(segs)
    push!(r.routes, Route(method == "*" ? "" : String(method), segs, tail, stream, f))
    return r
end

"""`route!(app, "GET", "/hello/{name}") do req, params ... end`"""
route!(f, r::Router, method::AbstractString, pattern::AbstractString) = add_route!(r, method, pattern, f, false)
"""`stream!(app, "POST", "/upload") do s::Stream, params ... end`"""
stream!(f, r::Router, method::AbstractString, pattern::AbstractString) = add_route!(r, method, pattern, f, true)

split_path(p::AbstractString) = String[s for s in split(p, '/') if !isempty(s)]

function match_route(rt::Route, segs::Vector{SubString{String}})
    n = length(rt.segments)
    (length(segs) == n || (rt.tail && length(segs) >= n)) || return nothing
    params = Dict{Symbol,String}()
    for (i, pat) in enumerate(rt.segments)
        s = segs[i]
        if startswith(pat, '{') && endswith(pat, '}')
            params[Symbol(pat[2:end-1])] = String(s)
        elseif pat != s
            return nothing
        end
    end
    rt.tail && (params[:*] = join(segs[n+1:end], '/'))
    return params
end

target_segments(target::AbstractString) = [x for x in split(target, '/') if !isempty(x)]

"""
    allowed_methods(r::Router, target) -> Vector{String}

The methods the routes matching `target`'s path take, in a fixed order (`HEAD`
with `GET`); empty when no route matches the path. A `"*"` route gives `["*"]`.
"""
function allowed_methods(r::Router, target::AbstractString)
    segs = target_segments(target)
    ms = Set{String}()
    for rt in r.routes
        match_route(rt, segs) === nothing && continue
        isempty(rt.method) && return ["*"]
        push!(ms, rt.method)
        rt.method == "GET" && push!(ms, "HEAD")
    end
    order = ("GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS")
    return sort!(collect(ms); by = m -> (something(findfirst(==(m), order), length(order) + 1), m))
end

"""The default answer when no route takes the request: 404, or 405 with `Allow`."""
function unmatched_response(r::Router, target::AbstractString)
    allow = allowed_methods(r, target)
    isempty(allow) && return Response(404, ["content-type" => "text/plain"], "Not Found\n")
    return Response(405, ["content-type" => "text/plain", "allow" => join(allow, ", ")], "Method Not Allowed\n")
end

function handle(r::Router, s::Stream)
    req = s.request
    segs = target_segments(req.target)
    for rt in r.routes
        params = match_route(rt, segs)
        params === nothing && continue
        (isempty(rt.method) || rt.method == req.method || (rt.method == "GET" && req.method == "HEAD")) || continue
        if rt.stream
            rt.f(s, params)
        else
            write_response(s, to_response(rt.f(req, params)))
        end
        return nothing
    end
    write_response(s, r.fallback !== nothing ? to_response(r.fallback(req)) : unmatched_response(r, req.target))
    return nothing
end
