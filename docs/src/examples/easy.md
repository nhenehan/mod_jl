# Easy: a greeter

**Shows:** a `Router`, a path parameter, reading a request header, returning a
string, and an explicit `Response` with a custom status and headers.

**File:** `jl_serv/JlServ/examples/easy/greeter.jl`

## Run it

From the repository root:

```bash
export JL_SECRET=change-me
julia --project=jl_serv/JlServ -t 4,1 jl_serv/JlServ/bin/jlserv \
    jl_serv/JlServ/examples/easy/greeter.jl --unix /tmp/greeter.sock
```

## Try it

```bash
$ J="jlc --unix /tmp/greeter.sock --secret change-me"

$ $J /
Try /hello/yourname or /teapot

$ $J -H 'user-agent: my-terminal' /hello/ada
Hello, ada! You are using my-terminal.

$ $J -i /teapot
< 418
< content-type: text/plain
< x-brewing: earl grey
< content-length: 13
I'm a teapot
```

## The code

```julia
{{#include ../../../jl_serv/JlServ/examples/easy/greeter.jl}}
```

## How it works

- **`app = Router()`** starts an empty router. Each `route!` adds a method
  and path pattern and the function that answers it.
- **`do req, params`** is how every route function looks: the `Request`, and
  a `Dict` of the values the pattern captured.
- **Returning a string** gives a `200` with `content-type: text/plain;
  charset=utf-8` and a `content-length`.
- **`header(req.headers, "user-agent", "an unknown client")`** looks the
  header up ignoring case, and returns the default if it's missing.
- **`Response(418, [...], "...")`** is for when you need to choose the
  status or add headers.
- **The last line, `app`,** is the value `jlserv` serves.

## Things to try

- Add `route!(app, "GET", "/hello/{first}/{last}")` and greet both names.
- Return `nothing` from a route and look at the status with `jlc -i`.
- Ask for `/hello` with no name. You get `404`, not `405`: no route has a
  pattern of that length. (See
  [Routing](../guide/routing.md#when-nothing-matches).)
