# Requests and bodies

A handler gets a `Request`. Its fields come from the client's request as
Apache saw it.

## The request head

| Field | Type | Notes |
|---|---|---|
| `method` | `String` | `"GET"`, `"POST"`, ... |
| `target` | `String` | the path, **percent-encoded**, without the query; behind mod_jl, with `.` and `..` segments already resolved |
| `query` | `String` or `nothing` | everything after `?`, still encoded |
| `headers` | `Headers` | see below |
| `remote_addr`, `remote_port` | `String`, `UInt16` | the client |
| `server_name`, `server_port` | `String`, `UInt16` | the virtual host that took the request |
| `local_addr`, `local_port` | `String`/`UInt16` or `nothing` | the address the client connected to |
| `is_ssl` | `Bool` | whether the client used TLS |
| `ssl` | `SslInfo` or `nothing` | cipher, key size, protocol, session id, client certificate and chain (DER bytes) |
| `auth_type`, `remote_user` | `String` or `nothing` | set when Apache authenticated the user |
| `route` | `String` or `nothing` | the worker route for sticky sessions |
| `proto` | `UInt16` | HTTP version × 1000 + minor: `1001` is HTTP/1.1 |
| `body_length` | `Int64` or `nothing` | see [The body](#the-body) |
| `body` | `RequestBody` | the request body, an `IO` |
| `id` | `UInt32` | the request's id on its connection, handy in logs |

Because Apache does the authentication, `remote_user` can be trusted, as long
as only Apache can reach the worker. [Running workers](../running/jlserv.md)
covers that.

## Headers

`req.headers` is an ordered list of `name => value` pairs, in the order the
client sent them. Lookups ignore case:

```julia
header(req.headers, "content-type")              # first value, or nothing
header(req.headers, "accept", "*/*")             # ... or a default
headers(req.headers, "cookie")                   # every value, as a Vector
hasheader(req.headers, "authorization")
for (name, value) in req.headers ... end         # all of them, in order
```

Well-known header names arrive in lowercase. Others keep the case the client
used.

A header value that wasn't valid UTF-8 on the wire still arrives as a
`String`, holding the raw bytes. Check `isvalid(value)` if that matters to
you.

## The body

`req.body` is an `IO`. It reads the body **on demand**. The first block arrives
with the request (`jlc` sends up to half a frame, 32 KiB by default), and JlServ asks
for each later block only when you read past what it already has. A handler
that doesn't read a large upload costs only that first block.

`req.body_length` tells you what to expect before you read:

| `body_length` | Meaning |
|---|---|
| `nothing` | the request has no body |
| `n ≥ 0` | the client declared `content-length: n` |
| `-1` | the body is chunked; its length isn't known until the end |

Read it in whichever way suits the size:

```julia
data = read(req.body)               # all of it, as bytes
text = read(req.body, String)       # all of it, as a string

buf = Vector{UInt8}(undef, 64 * 1024)
while !eof(req.body)                # in blocks, in constant memory
    n = readbytes!(req.body, buf)
    process(view(buf, 1:n))
end
```

## Query strings and forms

`queryparams` decodes a query string, or a form body, into ordered
`name => value` pairs:

```julia
# GET /search?q=caf%C3%A9+au+lait&tag=a&tag=b
queryparams(req)          # ["q" => "café au lait", "tag" => "a", "tag" => "b"]
Dict(queryparams(req))    # one value per name: the last one wins

# POST with content-type: application/x-www-form-urlencoded
form = queryparams(String(read(req.body)))
```

Repeated names are kept, in order, and a part without `=` gets an empty
value. `+` is decoded as a space, as browsers encode it.

`unescapeuri(s)` decodes `%XX` escapes in anything else, such as a path
parameter. It leaves `+` alone unless you pass `plus = true`:

```julia
route!(app, "GET", "/files/{name}") do req, params
    name = unescapeuri(params[:name])     # "/files/a%20b.txt" gives "a b.txt"
    ...
end
```

A `%` that isn't followed by two hex digits is kept as it is. If the escapes
don't make valid UTF-8, the result is a `String` for which `isvalid` is
false. After decoding, a name can contain `/` or `..`, so validate it
before using it as a file name.

## Sessions

`session_id(req)` makes a new random session id (128 bits from the system's
secure random source) for a cookie. When the request names a worker route,
the id ends with `.route`, the convention mod_jk uses, so that sticky load
balancing can send the session back to this worker:

```julia
id = session_id(req)    # "3f9c...e21a.j1"
Response(200, ["set-cookie" => "JSESSIONID=$id; Path=/; HttpOnly"], "welcome\n")
```

## Limits

**Limit what you accept.** `read(req.body)` will happily read a 10 GB upload
into memory. If you have a limit, check `body_length` first and then count
while you read, because a chunked body doesn't declare its length. The
[medium example](../examples/medium.md) has a `read_limited` function that
does both.
