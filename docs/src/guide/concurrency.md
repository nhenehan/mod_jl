# Concurrency and shared state

## How requests are run

- The client (mod_jl, or `jlc` today) keeps **connections** open to the
  worker and reuses them.
- Each connection has **one task** in the worker, and that task runs **one
  request at a time**.
- Requests on different connections run **at the same time**, on different
  threads.

So the number of requests a worker runs at once is the number of connections
Apache has open to it, and they can run in parallel on as many threads as
you gave Julia with `-t`.

Two things follow:

1. **Shared state needs a lock.** A global `Dict` that handlers write to is a
   data race without one.
2. **A slow handler only holds up its own connection.** Other connections
   keep being served. With mod_jl, each busy Apache thread or process uses a
   connection of its own, so a slow request doesn't hold up the others.

## Guarding shared state

Use a `ReentrantLock` and keep the locked section short:

```julia
const NOTES = Dict{String,Vector{UInt8}}()
const NOTES_LOCK = ReentrantLock()

route!(app, "GET", "/notes/{id}") do req, params
    note = lock(() -> get(NOTES, params[:id], nothing), NOTES_LOCK)
    ...
end
```

Do the slow work (reading the body, formatting, I/O) **outside** the lock,
and hold the lock only to read or update the shared structure. The
[medium example](../examples/medium.md) reads a whole request body before it
takes the lock.

For a value updated very often, such as a counter, `Threads.Atomic{Int}` is
cheaper than a lock.

## Waiting for something another request does

A request can wait for another one with a `Threads.Condition`. Hold the
condition's lock, check the state, and `wait` if it isn't ready. The other
side changes the state and calls `notify` while holding the same lock:

```julia
cond = Threads.Condition()
ready = Ref(false)

# waiter
lock(cond) do
    while !ready[]
        wait(cond)
    end
end

# notifier
lock(cond) do
    ready[] = true
    notify(cond)
end
```

The [difficult example](../examples/difficult.md) builds a progress feed this
way: the upload's request notifies, and any number of event streams on other
connections wait.

## Background work

You can start tasks of your own with `Threads.@spawn`. A task you start isn't
part of any request, so:

- catch and log its errors yourself, since nobody else will see them;
- don't write to a `Stream` from it after the handler that owns the stream
  has returned.

## State is per process

Each worker process has its own memory. When mod_jl balances requests over
several workers, a later request can land on a different process, which won't
have the first one's in-memory state. Sticky sessions keep one client on one
worker: make session ids with `session_id(req)`, which ends them in the
worker's route, and mod_jl sends the session back to that worker (see
[Load balancing](../running/balancing.md#sticky-sessions)). A worker can still
fail or restart, so for data that has to survive that, or be shared, use a
database or another external store.
