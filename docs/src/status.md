# What works today

Everything in this book works. mod_jl was built in phases, a little at a
time, and they are done:

- The **JlServ** package and the `jlserv` command: handlers, routing,
  request bodies, streaming, graceful shutdown, restarts without a gap.
- The **Apache module**: `JlWorker`, `JlMount` and `SetHandler jl:NAME`, a
  pool of connections to each worker and health checks (`ping_mode`). See
  [Configuring Apache](running/apache.md).
- **Load balancing** in mod_jk's style: `JlBalancer` with the methods
  Requests, Traffic, Busyness and Next, lbfactors, distances, sticky
  sessions, failover and recovery, shared between httpd's processes. See
  [Load balancing](running/balancing.md).
- The **status page** (`jl-status`) and the **watchdog**. See
  [Status page and watchdog](running/jl-status.md).
- **`jlc`**, a client for trying out workers without Apache.
- The **jlp** protocol and its C library.

**Not carried over from mod_jk:** unmounts, extension mounts (`/*.jsp`),
`JkEnvVar` and `JkOptions`; see
[Coming from mod_jk](running/coming-from-mod_jk.md#not-carried-over).

**Known limits in JlServ:**

- `req.target`, `req.query` and path parameters arrive exactly as the
  client sent them, percent-encoded. Decode them with `unescapeuri` and
  `queryparams` (see [Requests and bodies](guide/requests.md#query-strings-and-forms)).
