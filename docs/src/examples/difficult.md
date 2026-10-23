# Difficult: uploads with live progress

**Shows:** streaming a request body through a hash in constant memory;
writing a response while the request body is still arriving; tasks on
different connections waiting for each other with a `Threads.Condition`;
server-sent events; a heartbeat that notices a client that went away; and
failing cleanly mid-stream.

**File:** `jl_serv/JlServ/examples/difficult/uploads.jl`

| Request | Answer |
|---|---|
| `POST /uploads/{id}` | streams the body through SHA-256 and writes `received N bytes` every 4 MiB, then the digest; `409` if `id` was already uploaded |
| `GET /uploads/{id}/events` | server-sent events for that upload, from any connection, until it finishes |
| `GET /uploads/{id}` | `202` while running, `200` with the result, `500` if it failed, `404` if unknown |

## Run it

```bash
export JL_SECRET=change-me
julia --project=jl_serv/JlServ -t 4,1 jl_serv/JlServ/bin/jlserv \
    jl_serv/JlServ/examples/difficult/uploads.jl --unix /tmp/up.sock
```

## Try it

Open the event stream first, in one terminal. It waits for an upload that
hasn't started yet:

```bash
$ J="jlc --unix /tmp/up.sock --secret change-me"
$ $J /uploads/u1/events
```

Upload in another terminal:

```bash
$ head -c 12M /dev/urandom > /tmp/data
$ $J -X POST --data-file /tmp/data /uploads/u1
received 4194304 bytes
received 8388608 bytes
received 12582912 bytes
done: 12582912 bytes, 48823 lines, sha256 b7b160cddc0c29091a2a210c645d33195a86cce1580b44dc93aabf558616acd8
$ sha256sum /tmp/data
b7b160cddc0c29091a2a210c645d33195a86cce1580b44dc93aabf558616acd8  /tmp/data
```

Meanwhile the first terminal shows the upload's progress, and the stream
ends once the upload does:

```text
event: progress
data: bytes=0 lines=0

event: progress
data: bytes=65536 lines=251

event: progress
data: bytes=131072 lines=497
...
event: done
data: bytes=12582912 lines=48823 sha256=b7b160cddc0c29091a2a210c645d33195a86cce1580b44dc93aabf558616acd8
```

The first event reports the state at the moment the stream subscribed, which
is nothing yet. The line counts and the digest depend on your random data.

If the uploader goes away halfway, the stream ends with the error instead,
and `GET /uploads/u2` gives `500`:

```text
event: failed
data: IOError: write: broken pipe (EPIPE)
```

## The code

```julia
{{#include ../../../jl_serv/JlServ/examples/difficult/uploads.jl}}
```

## How it works

### One state object, two kinds of request

An `Upload` holds the progress (`bytes`, `lines`) and the outcome
(`sha256`, or `error`), guarded by its own `Threads.Condition`.
`upload!(id)` creates it on first use **from either side**, so an event
stream can subscribe before the upload exists. That's why the event stream
in *Try it* could be started first.

The global `UPLOADS` dictionary has a lock of its own. Each upload's fields
are guarded by that upload's condition, so concurrent uploads don't contend
on a single lock.

### The upload: streaming in, streaming out

The POST is a `stream!` route because it writes while it reads:

1. `claimed` makes sure only one POST per id runs. A second gets `409`,
   decided under the condition's lock so that two simultaneous POSTs can't
   both win.
2. `startwrite(s)` sends the `200` and headers right away. From then on the
   status can't change, which is why the error handling below can't send a
   `500`.
3. The loop reads `s.request.body` in 64 KiB blocks into **one reused
   buffer**, so memory use is the same for 1 MB and 100 GB. Each block goes
   into the hash and the line count, and then through `publish!` to the
   subscribers.
4. Every 4 MiB, a progress line is written and `flush`ed so the uploader
   sees it straight away rather than when the output buffer fills.

### Failing cleanly

If the upload fails (the uploader disconnects, so reading the body or
writing a progress line throws), the `catch` records the error on the `Upload` so the event streams can report
`event: failed`, and then **rethrows**. Rethrowing matters: the response has
already started, so JlServ has to drop the connection rather than pretend the
response finished. See
[Streaming](../guide/streaming.md#errors-after-the-response-started).

### The event stream: waiting across connections

Each pass of the loop takes the condition's lock and, if nothing has changed,
`wait`s. `publish!` on the upload's connection changes the fields and calls
`notify`, which wakes every waiting stream. The stream then writes one event
for whatever it finds:

- a new byte count gives `event: progress`;
- a digest gives `event: done`, and the stream ends;
- an error gives `event: failed`, and the stream ends;
- **no change** means a heartbeat woke it, so it writes `: keep-alive`.

### Why the heartbeat

A stream blocked in `wait` doesn't notice its client going away, because
only a write can fail. Without a heartbeat, an abandoned subscriber to an
upload that never happens would wait forever. The `Timer` notifies the
condition every 15 seconds. The stream wakes, writes a comment line, and if
the client has gone the write throws and the handler ends. The `finally`
closes the timer either way.

Progress events are throttled for free. A stream that's busy writing misses
the intermediate notifications and, when it next looks, reports only the
latest numbers. A slow subscriber falls behind on detail but never on
memory.

## Things to try

- Add `DELETE /uploads/{id}`, so the `UPLOADS` dictionary doesn't grow
  forever. What should happen to an event stream that's waiting on a deleted
  upload?
- Report throughput (bytes per second) in each progress event.
- Abort an upload halfway through (Ctrl-C the uploading `jlc`), watch the
  event stream report `failed`, then ask `GET /uploads/{id}` for the result.
