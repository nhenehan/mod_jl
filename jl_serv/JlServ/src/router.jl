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

`fallback(req)` runs when nothing matches; by default a 404 (or a 405 when the
path matches under another method).
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

function handle(r::Router, s::Stream)
    req = s.request
    segs = [x for x in split(req.target, '/') if !isempty(x)]
    path_ok = false
    for rt in r.routes
        params = match_route(rt, segs)
        params === nothing && continue
        path_ok = true
        (isempty(rt.method) || rt.method == req.method || (rt.method == "GET" && req.method == "HEAD")) || continue
        if rt.stream
            rt.f(s, params)
        else
            write_response(s, to_response(rt.f(req, params)))
        end
        return nothing
    end
    if r.fallback !== nothing
        write_response(s, to_response(r.fallback(req)))
    else
        st = path_ok ? 405 : 404
        write_response(s, Response(st, ["content-type" => "text/plain"], path_ok ? "Method Not Allowed\n" : "Not Found\n"))
    end
    return nothing
end
