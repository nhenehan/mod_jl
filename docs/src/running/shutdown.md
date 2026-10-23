# Shutdown and draining

Restarting a worker to deploy new code shouldn't cost any client a failed
request. JlServ's shutdown is built for that.

## What happens on Ctrl-C (SIGINT) or `shutdown!`

1. **Stop accepting.** The listening sockets are closed, so new connections
   are refused at once. A balancer can send them to another worker.
2. **Tell idle connections.** Each connection that isn't in the middle of a
   request gets a `SHUTDOWN` message, so the client knows to stop using it.
3. **Finish what's in flight.** Requests already running carry on to the end.
   Their response says *don't reuse this connection*, and the worker closes
   the connection after it.
4. **Serve what's already on its way.** A request the client sent just
   before it learned of the shutdown is still served, rather than reset.
5. **Close the rest.** After a one-second grace period, idle connections are
   closed. Connections still busy after the **drain timeout** (30 seconds by
   default) are closed anyway.
6. **Exit with status 0.**

The log shows it:

```text
[ Info: jlserv: draining
```

## What a client sees

For a client in the middle of a request, nothing changes. It gets its full
response. Its next attempt to connect is refused. mod_jl is designed to take that as
*this worker is gone*, and `jlc bench` counts it separately from errors:

```text
GET /hello/world, 64 connections, 3.1 s
  requests 180284  errors 0  58472 req/s  0.7 MB/s body
  reconnects refused after the worker closed: 64
```

The project's test suite checks this. A 16-connection benchmark keeps
running while the worker is sent SIGINT, and it must finish with zero failed
requests.

## Restarting without a gap

To deploy new code to a worker behind mod_jl with no failed requests:

1. Start the new worker on **the same socket path**. It takes the path over
   at once (it binds a name of its own and renames it into place), so new
   connections reach it while the old worker is still running.
2. Once the new worker logs `listening`, send the old one SIGINT.

The old worker finishes its requests and sends `SHUTDOWN` on its idle
connections. mod_jl sees that before it next uses one of them, and opens a
new connection instead, which reaches the new worker. When the old worker
exits it leaves the socket path alone, since it is no longer its own.

The project's test suite checks this: requests keep running through the
whole swap, and every one must succeed.

## Long requests

The drain timeout is the most time a request can take to finish during a
shutdown. Anything still running after it is cut off. Choose it to fit your
slowest legitimate request, with `serve(app; drain_timeout = 120, ...)` when
you start the worker from your own code, and make sure whatever supervises
the worker waits longer than that (`TimeoutStopSec` in systemd).

Open-ended streams, such as a server-sent event feed, never finish on their
own, so they always run until the timeout. If your application has them,
consider a shorter timeout, or ending the streams yourself when the worker
starts draining.

## Signals

| Signal | Effect |
|---|---|
| `SIGINT` (Ctrl-C) | graceful drain, as above |
| `SIGTERM` | **immediate exit**, dropping requests in flight; Julia doesn't handle it |
| `SIGKILL` | immediate exit |

Configure whatever stops your workers to send SIGINT. See
[Running workers](jlserv.md#running-under-systemd).
