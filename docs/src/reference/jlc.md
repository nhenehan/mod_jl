# jlc, the test client

`jlc` speaks the worker protocol over one connection, doing what mod_jl will
do. Use it to try out and benchmark a worker without Apache.

```text
jlc CONN [-X METHOD] [-H 'Name: value']... [--data S | --data-file F|-] [--chunked]
         [-i] [-n COUNT] PATH[?QUERY]
jlc CONN --ping
jlc CONN bench [-c CONNS] [-d SECONDS] [-X METHOD] [-H ...] [--data S] PATH

CONN:  --unix PATH | --tcp HOST:PORT   [--secret S] [--max-frame N]
```

## One request

| Option | |
|---|---|
| `-X METHOD` | the method (default `GET`) |
| `-H 'Name: value'` | add a request header; repeat for more |
| `--data S` | send `S` as the body |
| `--data-file F` | send the file `F` as the body; `-` reads stdin |
| `--chunked` | send the body without a declared length, as a chunked upload would arrive |
| `-i` | print the status and headers, to stderr |
| `-n COUNT` | send the request `COUNT` times on the same connection |

The response body goes to stdout, unchanged, so you can pipe it or compare
it:

```bash
jlc --unix /tmp/j1.sock --secret s -X POST --data-file big.bin /echo | cmp - big.bin
```

A body from a regular file is sent with its length. A body from a pipe is
sent chunked, and so is any body when you add `--chunked`.

The exit status is `0` whenever a response arrived, **whatever its HTTP
status**. Use `-i` and look at the `<` status line to check for a 404 or a
500. It's `1` if the request couldn't be made or the connection failed.

## `--ping`

Sends a protocol-level ping and prints the round-trip time:

```text
$ jlc --unix /tmp/j1.sock --secret s --ping
CPONG in 21.6 us
```

The first ping to a new worker takes a few milliseconds while Julia compiles
the code path. Later ones take tens of microseconds.

## `bench`

Opens `-c` connections (default 16), each sending the request back to back
for `-d` seconds, then reports:

```text
GET /hello/world, 64 connections, 10.0 s
  requests 317199  errors 0  31700 req/s  0.4 MB/s body
  latency us: p50 1561.9  p90 3663.0  p99 8239.6  p99.9 23374.6  max 34728.0
```

- **errors** counts requests that failed or got a 5xx response. The first
  failure on each connection is described on stderr.
- **reconnects refused after the worker closed** only appears when the
  worker shut down during the run. It counts connections that ended
  cleanly (the worker said not to reuse them) and then couldn't reconnect.
  These are **not** errors.

The exit status is `1` if there were any errors, so `bench` can be used in
scripts and tests.

Build `jlc` in Release mode for benchmarking. A Debug build with sanitizers
measures the sanitizers, not the worker.
