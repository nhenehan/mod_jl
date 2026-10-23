# JlServ API

Everything `using JlServ` brings into scope.

## Handlers and routing

| | |
|---|---|
| `Router(; fallback = nothing)` | an empty router; `fallback(req)` answers requests no route matches |
| `route!(f, app, method, pattern)` | add a route; `f(req::Request, params::Dict{Symbol,String})` returns a response |
| `stream!(f, app, method, pattern)` | add a streaming route; `f(s::Stream, params)` writes the response |

`method` is `"GET"`, `"POST"`, ... or `"*"` for any method. Patterns are
described in [Routing](../guide/routing.md).

## Responses

| | |
|---|---|
| `Response(status = 200, headers = [], body = UInt8[]; reason = nothing)` | a complete response |
| `Response(body::AbstractString)` | `200`, `text/plain; charset=utf-8` |

The fields are `status`, `headers`, `body` and `reason`. `body` may be a
string, bytes, `nothing` or an `IO`. Besides a `Response`, a handler may
return a string (`200`), bytes (`200`, `application/octet-stream`) or
`nothing` (`204`). See [Handlers and responses](../guide/handlers.md).

## Requests

| | |
|---|---|
| `Request` | what a handler gets: the fields in [Requests and bodies](../guide/requests.md#the-request-head), plus `body` and `id` |
| `RequestBody <: IO` | `req.body`: `read`, `readbytes!`, `readavailable`, `eof`, `bytesavailable` |
| `Headers <: AbstractVector{Pair{String,String}}` | `req.headers`, in arrival order |
| `header(h, name, default = nothing)` | the first value of `name`, ignoring case |
| `headers(h, name)` | every value of `name`, as a `Vector{String}` |
| `hasheader(h, name)` | whether `name` is present |
| `queryparams(req)`, `queryparams(s)` | the decoded `name => value` pairs of `req.query`, or of a query string or form body |
| `unescapeuri(s; plus = false)` | decode `%XX` escapes; with `plus`, `+` too |
| `session_id(req)` | a new random session id, ending in `.route` when the request has one |

## Streaming

| | |
|---|---|
| `Stream <: IO` | what a `stream!` route gets |
| `setstatus!(s, status, reason = nothing)` | set the status, before the first write |
| `setheader!(s, name => value)` | add a header, before the first write |
| `startwrite(s)` | send the status and headers now |
| `write`, `print`, `println` | write body bytes |
| `flush(s)` | send buffered output and ask Apache to pass it on now |
| `read`, `eof`, `readavailable` | read the request body |
| `s.request` | the `Request` |

## Running

| | |
|---|---|
| `start(handler; unix, tcp, name = "jlserv", secret = "", max_frame = 65536, insecure_tcp = false, warmup_paths = nothing)` | start listening and return a `Server`; doesn't block |
| `warmup(handler; paths = ["/"])` | run GETs for `paths` through a real local connection, so they're compiled before real traffic |
| `serve(handler; drain_timeout = 30.0, kw...)` | `start`, block until Ctrl-C, then drain |
| `shutdown!(srv; grace = 1.0, timeout = 30.0)` | drain and stop a running server |
| `wait(srv)` | block until `srv` has stopped |

- `unix` is a socket path.
- `tcp` is a port, or `(host, port)`. It needs a `secret` unless
  `insecure_tcp = true`.

See [Running workers](../running/jlserv.md) and
[Shutdown and draining](../running/shutdown.md).
