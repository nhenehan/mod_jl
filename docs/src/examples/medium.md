# Medium: a notes API

**Shows:** a small REST API over an in-memory store; shared state behind a
lock; reading a request body with a size limit, whether or not its length is
declared; validating path parameters; and the status codes and conditional
requests a well-behaved API uses.

**File:** `jl_serv/JlServ/examples/medium/notes.jl`

| Request | Answer |
|---|---|
| `GET /notes` | the note ids, one per line |
| `GET /notes/{id}` | the note, with an `ETag`; `304` if `If-None-Match` matches; `404` if there is no such note |
| `HEAD /notes/{id}` | the same headers, no body |
| `PUT /notes/{id}` | `201` when created, `204` when replaced; `400` for a bad id; `413` over 1 MiB; `412` if `If-Match` doesn't match the current version |
| `DELETE /notes/{id}` | `204`, or `404` |

## Run it

```bash
export JL_SECRET=change-me
julia --project=jl_serv/JlServ -t 4,1 jl_serv/JlServ/bin/jlserv \
    jl_serv/JlServ/examples/medium/notes.jl --unix /tmp/notes.sock
```

## Try it

```bash
$ J="jlc --unix /tmp/notes.sock --secret change-me"

$ $J -i -X PUT --data 'buy milk' /notes/shopping
< 201
< location: /notes/shopping
< etag: "9799a972c6b6f616"

$ $J -i /notes/shopping
< 200
< content-type: text/plain
< etag: "9799a972c6b6f616"
< content-length: 8
buy milk

$ $J -i -H 'if-none-match: "9799a972c6b6f616"' /notes/shopping
< 304
< etag: "9799a972c6b6f616"
```

Two clients editing the same note. Each sends the ETag it last saw, and only
the first one wins:

```bash
$ $J -i -X PUT -H 'if-match: "9799a972c6b6f616"' --data 'buy oat milk' /notes/shopping
< 204
< etag: "4a43af1fb0f25631"
$ $J -i -X PUT -H 'if-match: "9799a972c6b6f616"' --data 'buy cola' /notes/shopping
< 412
< content-type: text/plain
< content-length: 21
the note has changed
```

Uploads from a pipe are chunked (their length isn't known in advance), and
the limit still applies:

```bash
$ head -c 2M /dev/zero | $J -i -X PUT --chunked --data-file - /notes/big
< 413
< content-type: text/plain
< content-length: 35
notes are limited to 1048576 bytes
```

(The ETags are shortened SHA-1 hashes of the note's text, so yours will be
the same.)

## The code

```julia
{{#include ../../../jl_serv/JlServ/examples/medium/notes.jl}}
```

## How it works

**Shared state.** `NOTES` is one `Dict` for the whole worker, and requests on
different connections run at the same time, so every access goes through
`NOTES_LOCK`. The PUT handler reads the whole body *before* it takes the lock,
so a slow upload never blocks readers. See
[Concurrency](../guide/concurrency.md).

**The size limit is enforced twice.** If the client declared a length
(`req.body_length`), an oversized request is refused before a byte is read.
A chunked upload has no declared length, so `read_limited` also counts while
it reads and gives up as soon as it passes the limit. Neither path keeps more
than the limit in memory.

**Path parameters are validated.** `params[:id]` is exactly what the client
sent, still percent-encoded. Accepting only `[A-Za-z0-9_-]` means an id can
never be `..` or contain a `/`, and never needs decoding.

**Conditional requests.** The ETag is a hash of the content, so it changes
exactly when the note does.
- `If-None-Match` lets a client that already has the current version get
  `304` and no body.
- `If-Match` on a PUT is *optimistic concurrency*: the update applies only if
  the note is still the version the client last read. The check and the write
  happen under the same lock, so two clients can't both pass the check.

**HEAD for free.** There's no HEAD route. The router answers HEAD with the
GET route, and JlServ drops the body.

**Returning early from a `do` block.** In the PUT handler, `return` inside
`lock(NOTES_LOCK) do ... end` returns from the block. The block's value
becomes the value of `lock(...)`, which is the last expression of the route,
so it is the response.

## Things to try

- Add `GET /notes?prefix=...`, reading the prefix with
  `Dict(queryparams(req))["prefix"]` (see
  [Query strings and forms](../guide/requests.md#query-strings-and-forms)).
- Persist the notes: write `NOTES` to a file on every change, and load it at
  startup.
- Make `DELETE` honour `If-Match` too.
