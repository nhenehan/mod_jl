# Streaming

A normal route builds the whole response and returns it. A **streaming route**
writes the response as it goes. Use one when:

- the response is large or of unknown size (exports, generated files);
- the client should see output before you're done (progress, logs,
  server-sent events);
- you want to read the request body and write the response at the same time
  (transforming an upload as it arrives).

```julia
stream!(app, "GET", "/count/{n}") do s, params
    setheader!(s, "content-type" => "text/plain")
    for i in 1:parse(Int, params[:n])
        println(s, i)
        flush(s)
    end
end
```

## The `Stream`

The function gets a `Stream`, an `IO` you write the response to and read the
request body from:

| Call | Does |
|---|---|
| `setstatus!(s, code[, reason])` | sets the status (default `200`) |
| `setheader!(s, name => value)` | adds a response header |
| `startwrite(s)` | sends the status and headers now |
| `write(s, x)`, `print(s, x)`, `println(s, x)` | writes body bytes |
| `flush(s)` | sends what's buffered **and** asks Apache to pass it to the client now |
| `read(s, ...)`, `eof(s)`, `readavailable(s)` | reads the request body |
| `s.request` | the `Request` |

The rules:

1. **Set the status and headers before the first write.** The first `write`
   (or `startwrite`) sends them. After that, `setstatus!` and `setheader!`
   have no effect on what the client sees.
2. **Writes are buffered.** Output goes out in frames (up to 64 KiB by
   default), so many small writes cost about the same as one large one. Call `flush` when the
   client should see something *now*. Each flush sends a frame, so don't
   flush after every byte.
3. **The response ends when your function returns.** There is nothing to
   close.

## Reading while writing

Once you've started the response you can keep reading the request body. For
large blocks, read `s.request.body` directly, since it's faster than reading
through the `Stream`:

```julia
stream!(app, "POST", "/upper") do s, params
    setheader!(s, "content-type" => "text/plain")
    startwrite(s)
    body = s.request.body
    buf = Vector{UInt8}(undef, 64 * 1024)
    while !eof(body)
        n = readbytes!(body, buf)
        # ASCII-uppercase byte by byte: a block may end mid-way through
        # a UTF-8 character, so don't turn blocks into Strings.
        write(s, map(b -> UInt8('a') <= b <= UInt8('z') ? b - 0x20 : b, view(buf, 1:n)))
    end
end
```

## Server-sent events

Server-sent events are a streaming response with a particular format. Set
`content-type: text/event-stream`, then write events and flush each one:

```julia
setheader!(s, "content-type" => "text/event-stream")
setheader!(s, "cache-control" => "no-cache")
startwrite(s)
write(s, "event: progress\ndata: 50%\n\n")
flush(s)
```

The [difficult example](../examples/difficult.md) is a complete event stream.
It waits for events from another connection, sends a heartbeat, and stops
cleanly.

## When the client goes away

If the connection to the worker closes while you're writing (with `jlc`,
that happens when `jlc` exits), the next `write`/`flush` that has to send
data throws an `IOError`. JlServ expects this: let the exception leave your
function and the connection is cleaned up. Use `try`/`finally` for your own
cleanup (closing files, stopping timers), not `catch`, so that the exception
still reaches JlServ.

A handler that only waits and never writes won't notice the client is gone.
A long-lived stream should write something every so often (a comment line
`": keep-alive\n\n"` in server-sent events) so that it finds out.

## Errors after the response started

Once the status line has been sent, an exception can't become a 500 any more.
JlServ logs it and drops the connection without finishing the response, so
Apache can tell the client the response was cut short.
