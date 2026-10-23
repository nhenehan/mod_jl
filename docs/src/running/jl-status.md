# Status page and watchdog

## The status page

`jl-status` shows every balancer and worker: their state, how busy they are,
and how many requests each has had. It can also change a balancer member
while httpd runs: take a worker out of rotation, give it a different share,
or try a failed worker again now.

```apache
<Location "/jl-status">
    SetHandler jl-status
    JlStatusReadOnly Off      # allow changes; the default is On
    Require ip 10.0.0.0/8     # or Require local, or an authenticated user
</Location>
```

**Restrict who can reach it.** Anyone who can load the page can change the
balancers when `JlStatusReadOnly` is `Off`. A second, read-only location is
handy for monitoring:

```apache
<Location "/jl-status-ro">
    SetHandler jl-status
    Require ip 10.0.0.0/8
</Location>
```

### Formats

| URL | Gives |
|---|---|
| `/jl-status` | an HTML page: one table per balancer, one for the workers; with forms when changes are allowed |
| `/jl-status?refresh=5` | the same, reloading every 5 seconds |
| `/jl-status?mime=json` | everything as JSON |
| `/jl-status?mime=prop` | `key=value` lines, for shell scripts |

Add `cmd=show&lb=NAME` or `cmd=show&w=NAME` for one balancer or worker.

The JSON looks like this:

```json
{
  "server": "mod_jl", "pid": 4711, "time": 1761100000, "readonly": false,
  "nonce": "9f2c4e...",
  "balancers": [
    {"name": "myapp", "method": "Busyness", "sticky_session": true, "sticky_session_force": false,
     "session_cookie": "JSESSIONID", "recover_time": 60, "retries": 2,
     "members": [
       {"worker": "j1", "route": "j1", "activation": "A", "lbfactor": 1, "distance": 0,
        "lb_mult": 2, "lb_value": 1204, "elected": 1204, "state": "OK", "busy": 3}
     ]}
  ],
  "workers": [
    {"name": "j1", "target": "unix:/run/myapp/j1.sock", "route": "j1", "state": "OK", "busy": 3,
     "requests": 1204, "errors": 0, "failovers": 0, "bytes_out": 5310021, "reply_timeouts": 0,
     "error_time": 0, "connections": {"size": 25, "idle": 22, "open": 4}}
  ]
}
```

| Field | Meaning |
|---|---|
| `state` | `OK`, `WARN`, `ERROR` or `PROBING` (one request is trying it after `recover_time`); see [Load balancing](balancing.md#states) |
| `busy` | requests in progress, over all of httpd's processes |
| `lb_value` | the count the balancer chooses by (requests or bytes, weighted by `lb_mult`, halved every `maintain` seconds) |
| `elected` | how many times the balancer chose this member |
| `error_time` | when the worker last went into ERROR, in seconds since 1970; 0 if never |
| `connections` | the pool of **the process that served the page**: its size, idle slots and open connections. Each httpd process has its own pool. |

Everything else is shared by all of httpd's processes.

### Changing things

Changes are POSTed, and must carry the page's **nonce**. That stops another
web site from making a browser that can reach the page post changes to it.
The HTML forms include the nonce; from a script, read it from the JSON:

```bash
S=https://admin.example.com/jl-status
nonce=$(curl -s "$S?mime=json" | jq -r .nonce)

# Stop sending new requests to j2 (its sessions still reach it), e.g. before
# restarting it:
curl -s --data "cmd=update&lb=myapp&w=j2&activation=D&nonce=$nonce" "$S?mime=json"

# Give j1 three times the share of an lbfactor-1 member:
curl -s --data "cmd=update&lb=myapp&w=j1&lbfactor=3&nonce=$nonce" "$S?mime=json"

# j3 is fixed: try it with the next request instead of after recover_time:
curl -s --data "cmd=recover&w=j3&nonce=$nonce" "$S?mime=json"

# Zero a balancer's counts, or a worker's:
curl -s --data "cmd=reset&lb=myapp&nonce=$nonce" "$S?mime=json"
```

| Command | Parameters | Does |
|---|---|---|
| `update` | `lb=`, `w=`, then any of `activation=A\|D\|S`, `lbfactor=1..100`, `distance=0..100` | changes one member of one balancer |
| `reset` | `lb=` and/or `w=` | zeroes the counts: a balancer's `lb_value` and `elected`; a worker's requests, errors, failovers and bytes |
| `recover` | `w=` | a worker in ERROR is tried by the next request that could use it |

The answer is the updated state, in the format asked for; the HTML page
answers a change by reloading itself. Mistakes get a one-line answer:

| Status | Why |
|---|---|
| 400 | a bad parameter, or a worker that isn't a member of that balancer |
| 403 | the location is read-only, or the nonce is missing or wrong |
| 405 | a change asked for with GET |

Changes last until httpd restarts or reloads its configuration, which starts
from the configuration again.

## The watchdog

A worker in ERROR is tried again when a request comes along after
`recover_time`. On a quiet balancer that can be long after the worker came
back. With the watchdog, httpd checks such workers itself:

```apache
JlWatchdogInterval 5
```

Every 5 seconds, one httpd process opens a connection of its own to each
worker in ERROR and does the handshake and a health check (`CPING`). A worker
that answers is OK again at once, before any request needs it. One that
doesn't has its `recover_time` started again, so requests don't try it
either.

It needs `mod_watchdog`, which most httpd builds include (`apache2 -l`
lists it). Where it is a separate module, load it:

```apache
LoadModule watchdog_module modules/mod_watchdog.so
```
