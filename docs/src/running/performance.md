# Performance notes

## How fast is it?

Fast enough to be fun, which is the goal: mod_jl aims to be readable and
to work well, more than to win benchmarks. One run of `bench/run.sh`, on a
laptop (16 cores, AMD Ryzen 7 7840U). Apache 2.4 with the event MPM, one
process with 128 threads, in front of one JlServ worker (`-t 8,1
--gcthreads=4,1`) on a Unix socket, everything at its defaults:

| Workload | Requests/s | p50 | p99 |
|---|---|---|---|
| `GET /hello/world`, 1 connection | 10,700 | 87 µs | 0.44 ms |
| `GET /hello/world`, 64 connections | 64,000 | 0.91 ms | 3.3 ms |
| `GET /bytes/65536`, 64 connections | 14,700 (about 965 MB/s of body) | 3.2 ms | 29 ms |
| `POST /count` with 1 KiB, 64 connections | 59,200 | 0.95 ms | 14 ms |
| `GET` of 1 GiB, streamed | 2,100 MB/s | | |

During the 1 GiB stream, httpd's process peaked at 33 MiB and the worker at
410 MiB, most of which is Julia itself and its compiled code: a streamed
response goes through in frames and is never held whole. The p99 column is
mostly Julia's garbage collector; see below.

The raw results are in `bench/results/`. To run it yourself, in the dev
container with a Release build of the module:

```bash
ct/dev.sh bench/run.sh build-mod-rel/mod_jl/mod_jl.so
```

`ping_mode=A` (a CPing before every request) costs about 30 µs per request,
which is why the default only checks connections that have been idle.

To measure your own application without Apache:

```bash
jlc --unix /tmp/j1.sock --secret change-me bench -c 64 -d 10 /your/route
```

## What costs time

**Allocation.** Julia's garbage collector is the main source of latency
spikes, and every allocation brings the next collection closer. A simple GET
costs about 29 allocations inside the worker (44 through a `Router`, which
splits the path and builds the `params` Dict). Most of the rest are the
request's own strings and objects. Each connection reuses one EXI decoder and
encoder for all its messages, which halved the count. Your handler's
allocations come on top.

- Reuse buffers for reading bodies (one `Vector{UInt8}` per request, as the
  examples do, not one per block).
- Return a `String` or a `Vector{UInt8}` rather than other kinds of string
  or array. These two go straight into the outgoing frames, without being
  converted first.
- Measure a handler with `@allocations` or `Profile.Allocs`.

**The EXI codecs.** Apache's side encodes a typical request head (12
headers) in about 9 µs and decodes a response head in about 2.5 µs
(`tests/bench_codec`). A profile puts most of that inside rexi: a new
encoder for each message, looking up element names by string, and a second
UTF-8 check of each value. Each could be made faster; none has been,
because the codecs' clarity matters more here than a few microseconds, and
a request spends far longer in Julia.

**Locks.** A lock held while doing slow work makes every other request that
needs it wait. Do the work first, and lock only to read or update the shared
structure.

**Flushing.** Every `flush` sends a frame and asks Apache to send it to the
client. Flush when a person or program is waiting for the output, not after
every write.

## Scaling

- **More connections**, up to the worker's thread count, use more cores in
  one worker.
- **More workers**, each a separate process, are the way to scale beyond
  that. Each has its own garbage collector, so a collection in one doesn't
  pause the others. See [Load balancing](balancing.md) to spread requests
  over them; `method=Busyness` steers around a worker that is collecting.
- **Julia's GC options**: `--gcthreads=N,1` gives the collector threads of
  its own. `--heap-size-hint` makes it collect earlier, keeping the heap and
  the pauses smaller.
