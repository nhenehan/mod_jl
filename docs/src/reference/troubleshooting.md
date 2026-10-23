# Troubleshooting

## The client is refused at connect: `HELLO refused, status N`

| Status | Cause | Fix |
|---|---|---|
| 1 | The client and worker were built from different versions of the message schema. | Rebuild both from the same checkout. |
| 2 | Wrong secret, or a secret where the worker has none (or the other way round). | Make `--secret` / the client's secret match the worker's `JL_SECRET`. |
| 3 | The client doesn't offer a protocol feature the worker requires (byte-aligned encoding). | Update the older side. |

The worker also logs each refusal as a warning.

## `jlc: connect: No such file or directory` / `Connection refused`

The worker isn't listening on that path. Check that it started: look for the
`listening` line in its log. Also check that `--unix` names the same path on
both sides. On first start, Julia may spend several seconds precompiling
before the socket appears.

## `ArgumentError: a TCP listener needs a secret`

`--tcp` without `JL_SECRET` set. A worker believes what its client tells it
about each request, so a TCP worker requires a secret. Set one.

## Every route answers 500, but the 404s work

The log shows a `MethodError` mentioning a *world age*, or "method too new".
This happens with your own launcher script that `include`s the application
**inside a function** and then calls `serve` directly. The route functions
are defined after the launcher's function started, and Julia won't call
them from it. Call it this way instead:

```julia
handler = Base.include(Main, "app.jl")
Base.invokelatest(JlServ.serve, handler; unix = "/tmp/j1.sock")
```

`bin/jlserv` already does this.

## Ctrl-C doesn't stop the worker

- You started it with `start(...)` and then `wait(srv)`. Julia doesn't
  deliver Ctrl-C to a task blocked in `wait`. Use `serve(...)`, which waits
  in a way Ctrl-C can interrupt and then drains.
- It is stopping, but slowly: a request (or an open event stream) is still
  running. A worker waits up to the drain timeout, 30 seconds by default.
  See [Shutdown and draining](../running/shutdown.md).

## The worker died and its log file is empty

Julia holds output to a redirected stderr until the process exits, so a
killed process loses it. `bin/jlserv` flushes after every message. If you
start workers from your own code with output going to a file, install a
logger that flushes, as `bin/jlserv` does.

## Requests fail during a restart

Stop workers with **SIGINT**, not SIGTERM. Julia exits immediately on
SIGTERM, dropping whatever is in flight. With SIGINT the worker drains, and
requests that were running complete. See
[Running workers](../running/jlserv.md#running-under-systemd).

## A handler error shows as a truncated response, not a 500

The handler threw after it had started sending the response (a `stream!`
route that wrote something). The status can't be changed at that point, so
the connection is dropped. Check the worker's log for the stack trace, and
for errors you expect, return an error status before writing.

## Two workers, one socket

Starting a worker on a socket path that's already in use replaces the
socket. The old worker keeps running and serves the connections it already
has, but new connections go to the new one. Give each worker its own path.

## httpd won't start: `JlBalancer needs shared memory`

Load `mod_slotmem_shm` before mod_jl:

```apache
LoadModule slotmem_shm_module modules/mod_slotmem_shm.so
```

A balancer keeps its state in shared memory, so that all of httpd's
processes agree on which workers are up and how busy they are.

## Sessions don't stick to one worker

- The session id must end in `.ROUTE`, where `ROUTE` is the worker's route:
  its `JlWorker` name, or its `route=`. `session_id(req)` makes such ids.
  Ids made another way need the suffix added.
- The cookie must be the balancer's `session_cookie` (`JSESSIONID` by
  default), or the id must be in its `session_path` parameter.
- A session whose worker is in error, stopped (`activation=S`) or failing
  goes elsewhere. With `LogLevel jl:trace1`, the error log shows each choice
  and why: `elected j2 (sticky)`, or `(balanced)` when the session's worker
  couldn't take it.

## 503s with `no CPONG within ping_timeout`

The worker took a connection's health check (`ping_mode`) and didn't answer
in time. A worker that is busy compiling or collecting garbage can take
longer than you expect, so check the worker's log and CPU first. If it is
only slow, raise `ping_timeout`. `no HELLO_ACK within ping_timeout` is the
same for a new connection.

## 503s with `no free connection within connection_acquire_timeout`

Every connection the httpd child keeps to that worker was in use. Raise
`connection_pool_size`, or leave it at its default (one per httpd thread),
where a request never has to wait.
