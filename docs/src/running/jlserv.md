# Running workers

## The `jlserv` command

```text
julia --project=ENV -t N,1 path/to/JlServ/bin/jlserv APP.jl
      [--unix PATH] [--tcp [HOST:]PORT] [--name NAME] [--max-frame N]
      [--warmup PATH[,PATH...]]
```

`jlserv` loads `APP.jl` and serves the file's last expression as the handler.

| Option | Meaning |
|---|---|
| `--unix PATH` | listen on a Unix socket (recommended) |
| `--tcp [HOST:]PORT` | listen on TCP; `HOST` defaults to `127.0.0.1` |
| `--name NAME` | the worker's name in logs and in the handshake (default `jlserv`, or `$JLSERV_NAME`) |
| `--warmup PATHS` | before listening, send a GET for each comma-separated path through the application (see [Warm-up](#warm-up)) |
| `--max-frame N` | the largest frame the worker accepts, in bytes: 8192 to 1048576, default 65536. The client and worker settle on the smaller of their two limits. |

`--unix` and `--tcp` can be given together, and you need at least one.

| Environment | Meaning |
|---|---|
| `JL_SECRET` | the shared secret clients must present; **required for TCP** |
| `JLSERV_NAME` | default for `--name`; your application can read it too (the test app uses it in `/whoami`) |

## Threads

Run with **`-t N,1`**: `N` threads for requests, plus one *interactive*
thread.

- Each connection's task runs on the `N` request threads, so up to `N`
  requests run in parallel.
- The accept loop runs on the interactive thread, so new connections are
  picked up promptly while every request thread is busy.
- Julia raises Ctrl-C's `InterruptException` in whatever task is running on
  its first thread. With `-t N,1`, only JlServ's own tasks run there, and
  they all turn it into a graceful drain. (Without an interactive thread,
  request tasks can run there too. In our tests the drain still worked, but
  `-t N,1` keeps your handlers out of the way.)

For `N`, a good start is the number of cores you want the worker to use.
Several smaller workers usually behave better than one large one. Each has
its own garbage collector, so a pause in one doesn't stall the others, and
mod_jl's balancer with `method=Busyness` steers around a worker that is busy
collecting (see [Load balancing](balancing.md)).

## Warm-up

Julia compiles code the first time it runs, so a new worker's first requests
are slower than the rest. JlServ compiles its own request path ahead of time
(when the package is precompiled), and `--warmup` runs your routes once
before the worker accepts traffic. Measured on the test application, once
the worker had logged `listening`:

| | first GET | each later GET |
|---|---|---|
| without `--warmup` | ~63 ms | ~4 ms |
| with `--warmup` of that route | ~37 ms | ~4 ms |

Any route that hasn't run yet still costs some tens of milliseconds on its
first request, while its code is compiled.

Only GETs are sent, and their responses are thrown away. List paths that are
safe to request: no side effects, nothing that needs a logged-in user.

The worker isn't ready until it logs `listening`. The socket file exists a
moment earlier, so anything that waits for the worker to start should watch
the log rather than the socket.

## Securing a worker

A worker believes what the client tells it about the request:
`remote_user`, `remote_addr`, `is_ssl` and the rest. So **only mod_jl may be
able to connect**.

- **Prefer a Unix socket.** It's created with mode `0660`, so only the
  worker's user and group can connect. Run the worker as a user that shares a
  group with Apache, and keep the socket in a directory only they can enter.
- **On TCP, set `JL_SECRET`** (the worker refuses to listen on TCP without
  one) and bind to a private address. Clients that present the wrong secret
  are refused during the handshake, and the worker logs it:

  ```text
  ┌ Warning: jlserv: refused unix (jlc/0.1): status 0x0002
  ```

- Give each worker **its own socket path**. Starting a worker on a path that
  already exists replaces the socket file, even if another worker is still
  listening there. That is what a restart without a gap relies on (see
  [Shutdown and draining](shutdown.md#restarting-without-a-gap)), but two
  workers meant to run side by side would end up sharing one path.

## Logs

The worker logs to stderr with Julia's standard logging: one line when it
starts listening, a stack trace for every handler error, a warning for each
refused client, and one line when it starts draining. Use `@info`,
`@warn` and so on in your handlers and they go to the same place.

`jlserv` flushes stderr after every message, so a log file is complete
even if the worker is killed.

## Running under systemd

A starting point for a unit file. Adjust the paths and the user:

```ini
[Unit]
Description=JlServ worker for myapp
After=network.target

[Service]
User=myapp
Group=www-data
Environment=JL_SECRET=change-me
WorkingDirectory=/srv/myapp
ExecStart=/usr/bin/julia --project=/srv/myapp -t 4,1 /srv/mod_jl/jl_serv/JlServ/bin/jlserv app.jl --unix /run/myapp/j1.sock --name j1
RuntimeDirectory=myapp
# SIGINT drains gracefully; SIGTERM (the default) kills at once.
KillSignal=SIGINT
# Longer than the 30 s drain timeout.
TimeoutStopSec=40
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

**`KillSignal=SIGINT` matters.** Julia doesn't handle SIGTERM: the process
exits within a fraction of a second and drops any requests in flight. The
same applies to containers. Use `STOPSIGNAL SIGINT` in the image, or
`docker stop --signal SIGINT`.

## Starting a worker from your own code

`jlserv` is a thin wrapper over three functions you can call yourself:

```julia
using JlServ

srv = start(app; unix = "/tmp/j1.sock", secret = "change-me", name = "j1")  # returns at once
# ... the worker is serving in the background ...
shutdown!(srv)                     # drain and stop

serve(app; unix = "/tmp/j1.sock", secret = "change-me")   # start, then block until Ctrl-C, then drain
```

Keywords for both: `unix`, `tcp` (a port, or `(host, port)`), `name`,
`secret`, `max_frame`, and `insecure_tcp = true` to allow TCP without a
secret, for tests. `serve` also takes `drain_timeout` (default 30 s).

Two things `jlserv` takes care of that you need to handle if you write your
own launcher. Both are in [Troubleshooting](../reference/troubleshooting.md):

- If you `include` the application from inside a function, call `serve`
  through `Base.invokelatest`. Otherwise every route fails with a world-age
  error.
- If stderr goes to a file, flush it after each log message, or the log is
  lost when the process is killed.
