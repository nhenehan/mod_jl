# Handlers and responses

A **handler** is whatever the last expression of your application file
evaluates to. It can be:

- a **function of one argument**, called with the `Request` for every request;
- a **`Router`**, which picks a route function by method and path (see
  [Routing](routing.md)).

```julia
using JlServ

# The simplest possible application: every request gets the same answer.
req -> "Hello\n"
```

## What a handler can return

JlServ turns the return value into a response:

| Return value | Response |
|---|---|
| a `String` (any `AbstractString`) | `200`, `content-type: text/plain; charset=utf-8` |
| a `Vector{UInt8}` | `200`, `content-type: application/octet-stream` |
| `nothing` | `204 No Content` |
| a `Response` | exactly what it says |

`content-length` is added for you when the body is a string or bytes and you
didn't set it yourself.

## `Response`

```julia
Response(status = 200, headers = [], body = UInt8[]; reason = nothing)
Response(body::AbstractString)
```

- `headers` is any collection of `name => value` pairs. Order is kept, and
  repeated names (several `set-cookie`s, say) are allowed.
- `body` can be a `String`, bytes, `nothing` (no body), or **any `IO`**. An
  `IO` is read to its end, streamed out in frames and then **closed**, so you
  can return an open file without loading it into memory or leaking it. (For
  HEAD it is closed without being read.) No `content-length` is added for an
  `IO` body; set one if you know it, for example with `filesize`.
- `reason` replaces the standard reason phrase.

```julia
Response(201, ["location" => "/notes/42"], nothing)
Response(200, ["content-type" => "application/json"], """{"ok":true}""")
Response(200, ["content-type" => "application/pdf",
               "content-length" => string(filesize("report.pdf"))], open("report.pdf"))
```

## HEAD requests

A route registered for `GET` also answers `HEAD`. JlServ sends the status and
headers, including the `content-length` the GET would have had, and drops the
body. You don't have to do anything.

## Errors

If a handler throws, JlServ logs the error with its stack trace and, if
nothing has been sent yet, answers `500 Internal Server Error`:

```text
┌ Error: jlserv: handler failed for GET /boom
│   exception =
│    boom
│    Stacktrace: ...
```

The worker keeps running and the connection stays usable.

If the handler had **already started sending** the response (a streaming
handler that called `startwrite` or wrote some body), it's too late to change
the status. JlServ then drops the connection without finishing the response,
so Apache can tell the client the response was cut short rather than pass it
on as if it were complete.

To send a particular error status, return it. Don't throw for expected
conditions:

```julia
note === nothing && return Response(404, ["content-type" => "text/plain"], "no such note\n")
```
