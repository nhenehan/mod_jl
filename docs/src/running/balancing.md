# Load balancing

One Julia process uses one garbage collector and one event loop. Past what
that serves, run several workers and put a **balancer** in front of them.
mod_jl's balancer follows mod_jk's: the same methods, sticky sessions,
failover and recovery.

```apache
LoadModule slotmem_shm_module modules/mod_slotmem_shm.so
LoadModule jl_module modules/mod_jl.so

JlWorker j1 unix:/run/myapp/j1.sock secret=change-me
JlWorker j2 unix:/run/myapp/j2.sock secret=change-me
JlWorker j3 unix:/run/myapp/j3.sock secret=change-me

JlBalancer myapp method=Busyness
JlBalancerMember myapp j1
JlBalancerMember myapp j2
JlBalancerMember myapp j3 lbfactor=2

JlMount /myapp/* myapp
```

A `JlMount` or `SetHandler jl:NAME` names a worker or a balancer; workers
and balancers share one set of names. `mod_slotmem_shm` must be loaded: the
balancer keeps what it knows about the workers in shared memory, so every
httpd process sees the same.

Two complete examples are in the repository's `examples/httpd/`:
`sticky-round-robin.conf` (with a systemd template, `jlserv@.service`, that
runs the instances) and `by-load.conf`. The test suite starts httpd with
both, so they stay correct.

## `JlBalancer`

```apache
JlBalancer NAME [setting=value]...
```

| Setting | Meaning |
|---|---|
| `method=` | how a worker is chosen: `Requests` (default), `Traffic`, `Busyness` or `Next`; see below |
| `sticky_session=` | send a request that belongs to a session to the worker that started it (default `on`) |
| `sticky_session_force=` | when that worker is unavailable, answer 503 instead of choosing another (default `off`) |
| `session_cookie=` | the cookie holding the session id (default `JSESSIONID`) |
| `session_path=` | the path parameter holding it (default `;jsessionid`) |
| `recover_time=` | how long a failed worker is left alone before one request tries it again (default 60 s) |
| `error_escalation_time=` | how long a worker may keep failing in WARN before it is put in ERROR (default half of `recover_time`) |
| `max_reply_timeouts=` | how many `reply_timeout`s a worker may have between two maintenance runs before it is put in ERROR (default 0: no limit) |
| `retries=` | how many workers one request may try (default 2) |
| `maintain=` | how often the balancer's counters are halved, so old load fades (default 60 s) |

## `JlBalancerMember`

```apache
JlBalancerMember BALANCER WORKER [setting=value]...
```

| Setting | Meaning |
|---|---|
| `lbfactor=` | the worker's share, 1 to 100 (default 1): `lbfactor=2` gets twice the requests of `lbfactor=1` |
| `activation=` | `A` active (default); `D` disabled: only requests of sessions it already has; `S` stopped: nothing |
| `distance=` | a preference tier (default 0): workers at a higher distance are used only while none at a lower one can take the request |
| `redirect=` | the member that takes this one's sessions when it is unavailable |
| `domain=` | a group of members that share sessions: when the session's worker is unavailable, another of its domain is tried first |

The same worker may belong to several balancers. Its health is shared
between them: a worker in error is in error for all of them.

## Methods

| Method | Chooses the worker with the fewest... | Good for |
|---|---|---|
| `Requests` | requests so far, weighted by `lbfactor` | requests that cost about the same |
| `Traffic` | bytes so far (request and response bodies), weighted | workloads where size is the cost: downloads, uploads |
| `Busyness` | requests in progress right now, weighted; ties by `Requests` | requests whose cost varies a lot; steering around a worker paused in garbage collection |
| `Next` | like `Requests`, but ties go round in turn rather than to the first member | small pools where `Requests` would favour the first members |

The counts are halved every `maintain` seconds, so a worker that was busy
an hour ago isn't owed an hour of extra requests.

`Busyness` is usually the best choice for Julia workers. A worker that is
collecting garbage still holds its requests, so its count of requests in
progress rises and new requests go to the others until it catches up.

## Sticky sessions

A session id that ends in `.ROUTE` belongs to the worker whose route is
`ROUTE`. A worker's route is its `JlWorker` name, unless it sets `route=`.
mod_jl tells the worker its route with each request, and JlServ's
`session_id(req)` appends it to the ids it makes, so sessions created with
`session_id` stick without anything else to configure:

```julia
route!(app, "POST", "/login") do req, p
    id = session_id(req)          # e.g. "k3Jd...Qw.j2" on worker j2
    Response(200, ["set-cookie" => "JSESSIONID=$id; Path=/; HttpOnly"], "welcome\n")
end
```

mod_jl looks for the id in the `session_path` parameter of the URL first
(`/app;jsessionid=k3Jd...Qw.j2/page`), then in each `session_cookie`
cookie, and uses the first one whose route names a member.

When that worker can't take the request (it is stopped, or in error), the
request goes to its `redirect`, then to a member of its `domain`, then to
any member, unless `sticky_session_force` is on, when it gets a 503. Keep
sessions in a store the workers share if they must survive a worker
failing.

## Failures and failover

A request that fails on one worker can go to another, up to `retries`
workers, as long as nothing has reached the client yet:

| What happened | The worker | Another worker tries? |
|---|---|---|
| connecting, the handshake or a health check failed; sending the request failed | ERROR | yes |
| all of the worker's connections were in use (`connection_acquire_timeout`) | unchanged | yes |
| the worker had the request but closed the connection, or didn't answer within `reply_timeout`, before anything reached the client | WARN | only if `recovery_options` allows (below) and the body can be sent again |
| the worker answered with a status listed in `fail_on_status` | ERROR | yes, if the body can be sent again |
| the worker answered with a negative `fail_on_status` (`-500`) | unchanged | no: the client gets that status |
| the worker broke the protocol | ERROR | no |
| the worker failed after the response started | WARN | no: the response is cut short |

The body can be sent again when nothing beyond the first block (the part
sent with the request, half a frame) was read from the client. A request
the worker received may have done something, so by default it is sent to
another worker only if its method is safe to repeat: `GET`, `HEAD`, `PUT`,
`DELETE`, `OPTIONS`, `TRACE`. `recovery_options` on the `JlWorker` changes
that:

| `recovery_options=` | After the worker received the request... |
|---|---|
| `idempotent` (default) | another worker tries only the methods above |
| `always` | another worker tries any method |
| `never` | no other worker tries; only requests that never reached a worker move |

### States

| State | Meaning |
|---|---|
| OK | taking requests |
| WARN | failed recently, still taking requests. A success puts it back to OK. Failing again after `error_escalation_time` puts it in ERROR. |
| ERROR | skipped. After `recover_time`, one request tries it: success puts it back to OK, level with the others' counts so it isn't flooded; failure starts `recover_time` again. |

With the [watchdog](jl-status.md#the-watchdog), httpd also checks workers in
ERROR itself, so one that comes back is OK before traffic needs it.

If every active worker is in ERROR, a request doesn't wait out
`recover_time`: it tries the worker that failed longest ago. So a balancer
whose workers all restarted at once is back as soon as one of them is.

With `LogLevel jl:trace1`, every choice is logged with every member's
state, requests in progress and count:

```text
jl: myapp: elected j2 (sticky); j1:OK/busy=3/lb=1204 j2:OK/busy=1/lb=1188 j3:ERROR/busy=0/lb=2410
```

## Workers on other machines

Workers don't have to live next to Apache. Put them on other hosts, in VMs
or in containers, and reach them over TCP:

```apache
# A small VM, a big bare-metal box, and a standby in the other site.
JlWorker vm1   tcp://10.0.1.11:7801 secret=change-me socket_keepalive=on
JlWorker metal tcp://10.0.1.20:7801 secret=change-me socket_keepalive=on
JlWorker far   tcp://10.8.0.30:7801 secret=change-me socket_keepalive=on reply_timeout=30

JlBalancer app method=Busyness
JlBalancerMember app vm1
JlBalancerMember app metal lbfactor=4
JlBalancerMember app far distance=1

JlMount /app/* app
```

- **Weight by hardware.** `lbfactor` is the share: a machine with four
  times the cores gets `lbfactor=4`. With `Busyness`, a slower machine also
  gets less, because its requests pile up and count against it.
- **Standbys.** A member at a higher `distance` takes requests only while
  none nearer can: a disaster-recovery site, or a spare box.
- **Names outlive addresses.** A worker's route is its `JlWorker` name (or
  `route=`), not its address. When a container comes back somewhere else,
  change the address and keep the name, and the sessions it had still find
  it.
- **Groups that share sessions.** If workers in one site share a session
  store, put them in one `domain`: a session whose worker is down goes to
  another worker of its domain first.
- **Secrets.** Every TCP worker needs a `secret` (JlServ refuses TCP without
  one), and should listen on a private network: the worker trusts what
  mod_jl tells it about the client.

### On a thin link

jlp is a good fit for a slow or metered link between Apache and its
workers:

- **Heads are compact.** Each request and response head travels as an EXI
  document guided by the schema: a method or a known header name costs a
  byte or two, the names of fields aren't sent at all, and a string that repeats
  within a head is sent once. A typical browser request head is about a
  quarter smaller than its HTTP/1.1 text.
- **Bodies travel as they are**, in chunks of up to `max_packet_size`, with
  a few bytes of framing each. They are not encoded at all.
- **No handshake per request.** Connections are pooled and reused; the
  HELLO happens once per connection.
- **Uploads are pulled.** A worker that answers without reading a large
  upload (a 413, say) means it never crosses the link.
- **Idle connections stay alive and checked.** `socket_keepalive=on` keeps
  firewalls and NAT from forgetting them, and `ping_mode=I` (on by default)
  checks one that has been idle before trusting it with a request.
- **Bound the slow cases.** `reply_timeout` gives up on a worker that
  doesn't answer in time, and `max_reply_timeouts` puts a worker that keeps
  doing that out of rotation.

## Changing members while httpd runs

The [status page](jl-status.md) shows every member's state and counts, and
can change a member's `activation`, `lbfactor` and `distance` without a
restart: set a worker to `D` before taking it down, and back to `A` after.

## Restarting workers one at a time

To deploy new code without failing a request, restart the workers one after
another, each as in [Restarting without a gap](shutdown.md#restarting-without-a-gap):
start the new process on the same socket, then send the old one SIGINT. The
balancer needs nothing for it. While one worker restarts, the others take
its share.

## Graceful restarts of httpd

The balancer's state (counts, states, and changes made on the status page)
starts over when httpd is restarted or reloaded. Requests still running in the old processes finish against
the old state, so the counts of requests in progress stay right.
