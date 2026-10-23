# Configuring Apache

`mod_jl` is the httpd module that forwards requests to your workers, over
connections that are kept open and checked before use. This chapter covers
workers and mounts; spreading requests over several workers is in
[Load balancing](balancing.md).

## A minimal configuration

```apache
LoadModule jl_module modules/mod_jl.so

# The worker: a name, where it listens, and the secret it expects.
JlWorker app unix:/run/myapp/j1.sock secret=change-me

# Which requests go to it.
JlMount /app/* app
```

Start the worker as in [Running workers](jlserv.md), with the same socket
path and `JL_SECRET`. httpd's user (often `www-data`) must be able to reach
the socket. The worker creates it with mode `0660`, so put both in a common
group.

## Building the module

The module is built with the rest of the project. It needs httpd's
development files (`apxs`, the APR headers):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJL_BUILD_MODULE=ON
cmake --build build          # gives build/mod_jl/mod_jl.so
```

Copy `mod_jl.so` to httpd's modules directory (`apxs -q LIBEXECDIR`), or
give `LoadModule` its full path. The module exports one symbol, `jl_module`.
The protocol library and the Rust EXI codec inside it stay private, so they
can't clash with other modules.

## `JlWorker`

```apache
JlWorker NAME ADDRESS [setting=value]...
```

| Part | Meaning |
|---|---|
| `NAME` | how mounts and balancers refer to it; not case-sensitive. Also its route for sticky sessions, unless `route=` says otherwise |
| `ADDRESS` | `unix:/absolute/path` or `tcp://host:port` |
| `secret=` | the worker's `JL_SECRET` |
| `connect_timeout=` | how long to wait to connect (default 5 s) |
| `socket_timeout=` | how long to wait on the worker once connected (default: httpd's `Timeout`) |
| `reply_timeout=` | how long to wait for each message from the worker once the request is sent (default: `socket_timeout`); see [Load balancing](balancing.md#failures-and-failover) |
| `max_packet_size=` | the largest frame, 8192 to 1048576 bytes (default 65536) |
| `connection_pool_size=` | connections kept per httpd child process (default: one per thread, `ThreadsPerChild`) |
| `connection_pool_min=` | idle connections never closed by `connection_pool_timeout` (default 0) |
| `connection_pool_timeout=` | close a connection idle this long (default 0: never) |
| `connection_acquire_timeout=` | how long a request waits for a free connection when all are in use (default 0: as long as it takes) |
| `ping_mode=` | when to check a connection is alive: letters `C`, `P`, `I`, or `A` for all of them, or `none` (default `CI`); see below |
| `ping_timeout=` | how long a check may take (default 10 s) |
| `connection_ping_interval=` | under `I`, check a connection idle longer than this (default 60 s) |
| `socket_keepalive=` | `on` sets TCP keepalive on connections to a TCP worker (default `off`) |
| `route=` | the suffix of the session ids this worker hands out (default: its name) |
| `retries=` | how many times to try connecting while nothing listens, as while a worker restarts (default 2) |
| `retry_interval=` | the wait between those tries (default 100 ms) |
| `recovery_options=` | whether a request the worker received but didn't answer may go to another worker: `idempotent` (default), `always` or `never`; see [Load balancing](balancing.md#failures-and-failover) |
| `fail_on_status=` | statuses that count as the worker failing, comma-separated: `503` puts the worker in error and lets another try; `-500` only fails the request |

Times are in seconds, or a number with a unit: `1500ms`, `30s`, `2mi`, `1h`.

`JlWorker` goes in the main server configuration, not inside a
`<VirtualHost>`. Every virtual host can use the workers defined there.

## Connections and health checks

Each httpd child process keeps its own connections to each worker. A request
takes a free connection, opening it the first time it is needed, and puts it
back when the response is done. The connection is closed instead when the
worker says not to reuse it (it is shutting down), or when anything went
wrong with the request, including a client that went away.

Before a pooled connection is used, mod_jl checks it has nothing waiting on
it: an idle connection should be silent, so anything there means the worker
closed it or is shutting down, and a new connection is opened instead. That
check is free and always on. `ping_mode` adds checks that cost a round trip
to the worker (a `CPING` answered by `CPONG`):

| Letter | Check |
|---|---|
| `C` | a new connection's handshake must be answered within `ping_timeout`. The handshake is a round trip anyway, so this costs nothing. |
| `P` | every request first checks its connection. It catches a worker that is hung but still connected, at the price of one round trip per request (tens of microseconds on a Unix socket). |
| `I` | a connection idle longer than `connection_ping_interval` is checked before use. |

A check that isn't answered within `ping_timeout` closes that connection and
answers the request with 503, without waiting for `socket_timeout`.

The default, `CI`, adds nothing to a busy worker's requests. Add `P` when a
worker that stops answering (stuck in a long garbage collection, or stopped
under a debugger) must be noticed within `ping_timeout` rather than
`socket_timeout`:

```apache
JlWorker app unix:/run/myapp/j1.sock secret=change-me ping_mode=A ping_timeout=2
```

If a pooled connection turns out to be dead only once the request is on it
(the worker died between two requests), the request is sent again once, on a
new connection. That happens only when the worker cannot have acted on it:
when sending failed, or for methods that are safe to repeat (`GET`, `HEAD`,
`PUT`, `DELETE`, `OPTIONS`, `TRACE`). A `POST` gets a 502 instead, since the
worker may have run it.

## `JlMount`

```apache
JlMount /reports app          # exactly /reports
JlMount /app/* app            # /app, and everything below it
JlMount /shop/* shop          # a JlBalancer
```

- The name is a `JlWorker` or a `JlBalancer`.
- A pattern ending in `/*` matches the path before it and everything below:
  `/app/*` matches `/app`, `/app/` and `/app/x/y`, but not `/application`.
- Any other pattern matches only that exact path.
- When several mounts match, the longest pattern wins.
- Mounts belong to the server or virtual host they're written in.
- They're matched against the decoded, normalised path: the same path
  `<Location>` sections see, after `mod_rewrite`.

Instead of a mount, you can name the worker as a handler:

```apache
<Location "/app">
    SetHandler jl:app
</Location>
```

A mounted URL is not a file. httpd doesn't look for it on disk, so
`<Directory>` sections and `.htaccess` files don't apply to it. Use
`<Location>` for access control on mounted paths, as you would with
`mod_proxy`.

## What the worker receives

- **The path.** It is the normalised path httpd matched, with `.` and `..`
  segments resolved, percent-encoded again. So the worker routes on the same
  path httpd matched mounts and `<Location>` sections on: a request for
  `/hello/../whoami` is a request for `/whoami`, at both ends.
- **The query string,** as the client sent it.
- **The request headers,** in the client's order. Hop-by-hop headers
  (`Connection` and the headers it names, `Keep-Alive`, `TE`, `Trailer`,
  `Transfer-Encoding`, `Upgrade`, `Proxy-*`) and `Expect` are not forwarded.
- **The client's address and port,** after `mod_remoteip` if you use it.
- **The server name and port** (`UseCanonicalName` applies), whether the
  connection used TLS, and the authenticated user and auth type when httpd
  authenticated the request.
- **The request body.** The first block (up to half a frame, 32 KiB by
  default) goes with the request, and the rest only as the handler reads it.
  If a handler doesn't read all of the body, nothing more travels to Julia,
  and httpd closes the client's connection after the response rather than
  read the rest of the upload only to discard it.

### `Expect: 100-continue`

When a client asks permission before sending a large body, mod_jl sends the
request without the body. httpd answers `100 Continue` only when the worker
starts to read. A handler that rejects the request without reading it (say,
a 413 for a body that's too large) answers the client straight away; the
client never uploads the body, and httpd closes that connection afterwards.

## What the client receives

- **Status, reason phrase and headers** as the handler set them, in its
  order, repeated headers included (several `Set-Cookie`).
  Hop-by-hop headers are dropped; httpd adds its own `Date`, `Server` and
  connection headers.
- **The body,** streamed as it arrives. httpd flushes what it has whenever
  the worker pauses, and whenever a handler calls `flush`.

### Errors

| What happened | The client gets |
|---|---|
| The worker isn't running, or refuses the connection (wrong secret) | `503 Service Unavailable` |
| A health check (`ping_mode`) isn't answered within `ping_timeout` | `503 Service Unavailable` |
| Every connection is in use for longer than `connection_acquire_timeout` | `503 Service Unavailable` |
| The worker answers with a status in `fail_on_status` | that status, with httpd's own error page |
| The worker breaks the protocol, or closes the connection before answering | `502 Bad Gateway` |
| The worker doesn't answer within `socket_timeout` | `504 Gateway Timeout` |
| The handler throws before sending anything | the worker's own `500` |
| The worker fails after the response started | the response is cut short: the connection closes before the declared length, or a chunked response ends without its final chunk, so the client can tell it is incomplete |
| The client goes away | the worker's connection is closed; the handler sees an `IOError` on its next write |

The [status page](jl-status.md) shows each worker's state and counts;
`JlWatchdogInterval` and `JlStatusReadOnly` are described there.

Behind a balancer, a request that fails before anything reached the client
may be tried on another worker first; see
[Load balancing](balancing.md#failures-and-failover).

Every failure is logged in httpd's error log with the worker's name and
address. `LogLevel jl:debug` also lists the workers and mounts at startup
and logs client aborts.

## Performance today

After its first request, a connection is reused, so a request costs one
round trip to the worker and no handshake. See
[Performance](performance.md) for numbers.
