# Coming from mod_jk

mod_jl is modelled on mod_jk, so most of what you know carries over: the
same kinds of workers, the same load balancer with the same methods and
states, sticky sessions by route, CPing, a status worker. This page maps
one to the other.

The biggest difference is where the configuration lives. mod_jk keeps its
workers in `workers.properties`. mod_jl's are ordinary httpd directives, in
httpd's own configuration.

## A side-by-side

mod_jk:

```properties
# workers.properties
worker.list=lb,status
worker.t1.type=ajp13
worker.t1.host=10.0.0.11
worker.t1.port=8009
worker.t1.secret=change-me
worker.t2.type=ajp13
worker.t2.host=10.0.0.12
worker.t2.port=8009
worker.t2.secret=change-me
worker.t2.lbfactor=2
worker.lb.type=lb
worker.lb.balance_workers=t1,t2
worker.lb.method=B
worker.status.type=status
```

```apache
JkWorkersFile conf/workers.properties
JkMount /app/* lb
JkMount /jkstatus status
```

mod_jl:

```apache
JlWorker t1 tcp://10.0.0.11:8009 secret=change-me
JlWorker t2 tcp://10.0.0.12:8009 secret=change-me

JlBalancer lb method=Busyness
JlBalancerMember lb t1
JlBalancerMember lb t2 lbfactor=2

JlMount /app/* lb

<Location "/jl-status">
    SetHandler jl-status
    Require local
</Location>
```

On the worker side, Tomcat's `jvmRoute` is the worker's `route`. It defaults
to the `JlWorker` name, so `t2`'s sessions end in `.t2`. JlServ's
`session_id(req)` adds the suffix for you.

## Directives

| mod_jk | mod_jl |
|---|---|
| `JkWorkersFile`, `worker.list` | `JlWorker`, `JlBalancer` in httpd's configuration |
| `JkMount /app/* w` | `JlMount /app/* w` |
| `SetHandler jakarta-servlet` + `JK_WORKER_NAME` | `SetHandler jl:w` |
| `JkWatchdogInterval` | `JlWatchdogInterval` |
| `JkShmFile` | none: load `mod_slotmem_shm` |
| `JkLogFile`, `JkLogLevel` | httpd's error log: `LogLevel jl:debug` (or `jl:trace1` to see every balancer decision) |

## Worker settings

`worker.NAME.X=V` becomes `JlWorker NAME ADDRESS X=V`.

| mod_jk | mod_jl | Notes |
|---|---|---|
| `type=ajp13`, `host`, `port` | `tcp://host:port`, or `unix:/path` | a Unix socket is the usual choice on one machine |
| `secret` | `secret` | JlServ refuses TCP without one |
| `socket_connect_timeout` | `connect_timeout` | |
| `socket_timeout`, `reply_timeout` | the same | |
| `socket_keepalive` | the same | |
| `ping_mode` (C, P, I, A), `ping_timeout`, `connection_ping_interval` | the same | default `CI`. `C` checks the handshake, which is a round trip anyway. |
| `connection_pool_size`, `connection_pool_timeout`, `connection_acquire_timeout` | the same | |
| `connection_pool_minsize` | `connection_pool_min` | |
| `retries`, `retry_interval` | the same | |
| `recovery_options` (a bit mask) | `recovery_options=idempotent\|always\|never` | words instead of bits |
| `fail_on_status` | the same | negative codes too |
| `max_packet_size` | the same | 8 KiB to 1 MiB, default 64 KiB |
| `route` (`jvmRoute`) | `route` | |

## Load balancer settings

`worker.lb.X=V` becomes `JlBalancer lb X=V`, and `balance_workers` becomes
one `JlBalancerMember` per worker.

| mod_jk | mod_jl |
|---|---|
| `method=R\|T\|B\|N` | `method=Requests\|Traffic\|Busyness\|Next` |
| `sticky_session`, `sticky_session_force` | the same |
| `session_cookie`, `session_path` | the same |
| `recover_time`, `error_escalation_time`, `max_reply_timeouts` | the same |
| `retries`, `maintain` | the same |
| member `lbfactor`, `activation`, `distance`, `redirect`, `domain` | the same, on `JlBalancerMember` |

## The status worker

| mod_jk | mod_jl |
|---|---|
| `worker.status.type=status` + `JkMount /jkstatus status` | `SetHandler jl-status` in a `<Location>` |
| `read_only` | `JlStatusReadOnly` (On by default) |
| `cmd=list\|show\|update\|reset\|recover` | the same |
| `mime=html\|xml\|prop\|txt` | `mime=html\|json\|prop`: no XML output, which is a little ironic for a module that speaks nothing but XML on the wire |
| the `vwa`, `vwf`... update parameters | `activation=`, `lbfactor=`, `distance=` |

Changes must be POSTed with the page's nonce; see
[Status page and watchdog](jl-status.md).

## The protocol

AJP and jlp have the same shape: one request per connection at a time, the
body pulled by the worker in chunks, then headers, body chunks and an
end-of-response saying whether to reuse the connection. CPING and CPONG
are the same idea. Where AJP has its own binary packet format, jlp sends each
message as an EXI document, so both ends check every message against
`jlp2.xsd`. A HELLO opens each connection, comparing the schema's hash and
the secret, so two builds that disagree about the format refuse each other
instead of misreading each other. See [How it works](../reference/protocol.md).

## Not carried over

- `JkUnMount`, `JkMountCopy`, `JkAutoAlias` and extension mounts
  (`JkMount /*.jsp`): mounts are exact paths or `/prefix/*`.
- `JkEnvVar`, `JkOptions` (`+ForwardURICompat` and the rest) and
  `JkRequestLogFormat`: mod_jl forwards the normalised path, as mod_jk's
  default `ForwardURIProxy` does, and httpd's own access log covers
  logging.
- `worker.NAME.type=ajp14`, `jni` and the other worker types, and the
  `lock` setting.
