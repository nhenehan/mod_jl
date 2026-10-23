# Introduction

**mod_jl** lets Apache httpd serve web applications written in Julia, the way
mod_jk serves Java applications from Tomcat. It is a tribute to mod_jk, and
borrows freely from it: the directives, the load balancer's methods and
states, sticky sessions by route, CPing, even the shape of the protocol.

Apache keeps doing what it is good at (TLS, virtual hosts, authentication,
static files, access logs) and forwards the requests you choose to Julia
processes, the **workers**. They can be on the same machine or anywhere else:
other hosts, VMs, containers.

The Julia side is **JlServ**, a small package. You write a handler, a plain
function or a `Router` of routes, and JlServ runs it for every request Apache
forwards:

```julia
using JlServ

app = Router()

route!(app, "GET", "/hello/{name}") do req, params
    "Hello, $(params[:name])!\n"
end

app
```

```text
                                        ┌─▶ JlServ worker j1 (this machine, Unix socket)
 browser ──HTTP──▶ Apache httpd ──jlp──┼─▶ JlServ worker j2 (a VM, TCP)
                   mod_jl               └─▶ JlServ worker j3 (a container on a bigger host, TCP)
                   TLS, auth, static,
                   mounts, balancing
```

## What it's good for

The same things mod_jk is good for:

- **Mounting.** `JlMount /reports/* reports` sends one part of a site to
  Julia and leaves the rest to Apache, PHP, a proxy, or whatever else
  serves it.
- **Several workers behind one URL.** A garbage collection pause, a crash or
  a deploy in one Julia process doesn't stall every client. Restart workers
  one at a time and nobody notices.
- **Workers on different hardware.** Give a bigger machine a bigger
  `lbfactor`. Keep a standby in another rack at a higher `distance`. Use
  round robin when requests cost about the same, or `Busyness` to send
  each request to the worker with the least in progress.
- **Sticky sessions.** A session id ending in `.j2` goes back to worker j2,
  so in-memory session state keeps working, and moves elsewhere only when j2
  is gone.
- **Fault tolerance.** A worker that stops answering is skipped, requests
  that never reached it are sent to another, and it is taken back once it
  recovers.
- **A compact protocol.** Between Apache and the workers, requests and
  responses travel as EXI, a binary encoding of XML, guided by a schema
  both sides share. A request head is smaller than its HTTP/1.1 text, and
  bodies travel as they are, which is welcome on a thin link to a remote
  worker.

For development on your own machine, `jlserv` and the `jlc` client are
enough. Apache comes in when you want the above.

## The parts

| Part | What it is |
|---|---|
| **JlServ** | The Julia package your application uses: handlers, routing, request bodies, streaming responses, graceful shutdown. |
| **`jlserv`** | The command that runs a JlServ application as a worker process. |
| **jlp** | The protocol between Apache and the workers, AJP's shape in EXI. You don't need to know it to write applications; [How it works](reference/protocol.md) has the outline. |
| **mod_jl** | The Apache module: workers, mounts, a connection pool with health checks, load balancing, sticky sessions, failover, a status page. |
| **`jlc`** | A command-line client that does mod_jl's job over one socket, for trying out workers without Apache. |

## Who this book is for

People writing Julia applications to run behind mod_jl, and people running
them. It assumes you know Julia. You don't need to know anything about
Apache internals or the protocol; if you know mod_jk, start with
[Coming from mod_jk](running/coming-from-mod_jk.md).

mod_jl is a for-fun project, and a workout for two EXI codecs, rexi (Rust,
inside the Apache module) and jexi (Julia, inside the workers). It aims to
be readable and to work well, more than to be the fastest way to put Julia
on the web.
