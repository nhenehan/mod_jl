# Your first worker

This chapter takes you from nothing to a running worker answering requests.

## What you need

- **Julia 1.11 or newer.**
- A checkout of this repository. It needs nothing next to it: JlServ gets
  its EXI codec, [jexi](https://github.com/nhenehan/jexi), through Julia's
  package manager, and the C side's,
  [rexi](https://github.com/nhenehan/rexi), is fetched by CMake. Both come
  from a tagged release.

- To talk to the worker without Apache, **`jlc`**. Building it needs CMake,
  a C compiler, git and a Rust toolchain (`cargo` builds rexi as part of
  the CMake build):

  ```bash
  cd mod_jl
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build --target jlc      # gives build/jl_client/jlc
  ```

  If you'd rather not install these, `ct/dev.sh` runs commands in a
  container that has them all (see the repository README).

## 1. Make a project for your application

Your application gets its own Julia environment, with JlServ added as a
development dependency:

```bash
mkdir myapp && cd myapp
julia --project=. -e 'using Pkg; Pkg.develop(path="../mod_jl/jl_serv/JlServ")'
```

Add any other packages your application needs in the same way
(`Pkg.add("JSON")`, ...).

## 2. Write the application

Create `app.jl`. The **last expression in the file is the handler** that
`jlserv` will serve:

```julia
using JlServ

app = Router()

route!(app, "GET", "/") do req, params
    "Hello from Julia $(VERSION)\n"
end

route!(app, "GET", "/hello/{name}") do req, params
    "Hello, $(params[:name])!\n"
end

app
```

## 3. Run it

```bash
export JL_SECRET=change-me
julia --project=. -t 4,1 ../mod_jl/jl_serv/JlServ/bin/jlserv app.jl --unix /tmp/myapp.sock
```

- `--project=.` makes your environment the active one, so `using` finds
  your packages.
- `-t 4,1` gives four threads for requests and one interactive thread for
  accepting connections. See [Running workers](running/jlserv.md#threads)
  for why the interactive thread is worth having.
- `JL_SECRET` is a shared secret the client has to present.

The worker logs a line like this when it's ready:

```text
┌ Info: jlserv: jlserv listening
│   listeners = ["Sockets.PipeServer(RawFD(16) active)"]
└   threads = 4
```

## 4. Send it a request

In another terminal:

```bash
$ jlc --unix /tmp/myapp.sock --secret change-me /hello/world
Hello, world!

$ jlc --unix /tmp/myapp.sock --secret change-me -i /nope
< 404
< content-type: text/plain
< content-length: 10
Not Found
```

`-i` prints the status and headers (to stderr), the way `curl -i` does.

## 5. Stop it

Press **Ctrl-C** in the worker's terminal. The worker stops accepting,
finishes the requests it's in the middle of, and exits with status 0:

```text
[ Info: jlserv: draining
```

## Next steps

- [Handlers and responses](guide/handlers.md) covers everything a handler
  can return.
- The three [examples](examples/easy.md) build up from this point to
  streaming and cross-connection events.
