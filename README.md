# mod_jl

A tribute to **mod_jk**, for Julia. mod_jk has put Apache httpd in front of
Tomcat for over twenty years. Its tricks are still the reason people reach for
it: mount an application at a URL; spread its requests over workers on
different machines, VMs and containers; keep each user's session on the worker
that has it; route around a worker that fails, and take it back when it
recovers. mod_jl does the same for Julia worker processes (JlServ).

It is a for-fun project, and a workout for two EXI codecs: rexi (Rust, on the
Apache side) and jexi (Julia, on the worker side). Where mod_jk speaks AJP, a
binary protocol, mod_jl speaks **jlp**: HTTP-as-XML (`schema/jlp2.xsd`)
encoded as strict, byte-aligned, schema-informed EXI. So a request head
travels smaller than its HTTP/1.1 text, which helps on thin links between
Apache and remote workers, and every message is checked against the schema
on both ends.

What you get, mod_jk style:

- `JlWorker` and `JlMount`: a worker over a Unix socket or TCP, mounted at
  `/app/*`.
- `JlBalancer`: round robin (Requests), Traffic, Busyness or Next, with
  lbfactors for bigger machines, distances for standbys, sticky sessions by
  route, failover, recovery, activation A/D/S, redirect and domain.
- A connection pool with CPing health checks, a `jl-status` page (HTML, JSON,
  key=value) that can change members live, and a watchdog.

Start with the user guide in `docs/` (`mdbook serve docs`); *Coming from
mod_jk* maps the familiar `workers.properties` settings to mod_jl's.

## Layout

| Path | What |
|---|---|
| `schema/jlp2.xsd` | The vocabulary. Enum order is the wire format. `schema/v1/` keeps the original draft. |
| `schema/samples/` | One instance of every message. `tools/make-vectors.sh` turns them into `testdata/vectors/*.exi` with the rexi CLI; these are the reference bytes both sides must match. |
| `tools/gen.jl` | Generates the C/Julia vocab tables and the JExi static tables from the XSD (`--check` to verify). |
| `libjlproto/` | C: framing, HELLO, ForwardRequest encode, worker-message decode, and the byte templates. Built on rexi-capi. |
| `jl_serv/JlServ/` | Julia worker package, built on JExi. |
| `jl_client/` | `jlc`, the C test client: talks to a worker without Apache. |
| `mod_jl/` | The httpd module: directives, mounts, the connection pool, balancing, shared state, `jl-status` and the watchdog. |
| `tests/` | Codec tests against the vectors, `jlc`-to-worker end-to-end tests, and the valgrind wrappers. |
| `tests/httpd/` | httpd tests of the module, curl and wrk against httpd and real workers: `test_basic.sh`, `test_pool.sh`, `test_lb.sh`, `test_status.sh`, `test_examples.sh`. |
| `examples/httpd/` | Ready-made httpd configs: round robin with sticky sessions (with a systemd template for the workers), and load-based balancing. |
| `bench/` | `run.sh`: one benchmark, httpd + mod_jl in front of a worker. |
| `jl_serv/JlServ/examples/` | `app.jl` (the test app) and three teaching examples: `easy/`, `medium/`, `difficult/`. |
| `docs/` | The user guide, an mdbook: `mdbook serve docs` or `mdbook build docs`. |
| `ct/` | A podman dev container with Julia, Rust and CMake: `ct/dev.sh <cmd>` runs a command in it. |

## Framing

Every message is a 12-byte header followed by one EXI document:

```
magic 0x4A | major 0x02 | type u8 | flags u8 | req_id u32 BE | len u32 BE
```

- HELLO and HELLO_ACK are the only fixed-binary payloads. They carry the
  schema's SHA-256, the negotiated max frame and the shared secret.
- The exchange follows AJP: FORWARD_REQUEST, then GET_BODY_CHUNK/BODY_CHUNK on
  demand, then SEND_HEADERS, BODY_CHUNK*, END_RESPONSE.
- A BodyChunk document is `80 00 00 ‖ uvarint(len) ‖ bytes`. Both sides learn
  that prefix from their codec at startup and check it before use, then write
  and read body bytes without a codec call.

## Commands

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DJL_SANITIZE=ON
cmake --build build && ctest --test-dir build --output-on-failure
JL_E2E_ECHO_MIB=1024 ctest --test-dir build -R e2e -V   # e2e with the 1 GiB echo
cmake -S . -B build-vg -G Ninja -DCMAKE_BUILD_TYPE=Debug -DJL_VALGRIND=ON   # not with JL_SANITIZE
cmake --build build-vg && ctest --test-dir build-vg -L valgrind --output-on-failure
julia --project=jl_serv/JlServ -e 'using Pkg; Pkg.test()'
cmake --build build --target vocab-check          # generated files up to date?
cmake --build build --target rexi-cli && tools/make-vectors.sh   # after changing schema/samples
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel && (cd . && build-rel/tests/bench_codec)
```

rexi and jexi come from tagged releases on GitHub: CMake fetches rexi
(`cmake/Rexi.cmake`) and builds it inside the build directory, and JlServ's
`Project.toml` names jexi in its `[sources]`. To build against a local rexi
checkout instead, configure with `-DFETCHCONTENT_SOURCE_DIR_REXI=/path/to/rexi`.

The valgrind tests run `test_codec` and every `jlc` of the e2e and examples
tests under memcheck (the Julia workers run natively); logs land in
`build-vg/tests/valgrind/`.

`-DJL_BUILD_MODULE=ON` builds `mod_jl.so` and adds the ctests `module_symbols`,
`httpd_basic`, `httpd_basic_pool1`, `httpd_pool`, `httpd_lb`, `httpd_status`,
`httpd_lb_prefork`, `httpd_status_prefork` and `examples_httpd`. With
`-DJL_SANITIZE=ON` the `httpd_*` tests become `_asan` variants (no prefork
ones), which run `apache2 -X` with libasan preloaded. `JL_T_BIG_MIB` shrinks
the 1 GiB transfers. The module build needs
`sudo apt install apache2-bin apache2-dev libapr1-dev libaprutil1-dev`.

## License

Apache-2.0, see [LICENSE](LICENSE). rexi and jexi, which the build fetches,
are BSD-3-Clause.
