-- wrk: POST 1 KiB to /count. Lua because wrk takes a request body only from
-- a script; it is wrk's config format, not part of mod_jl.
wrk.method = "POST"
wrk.body = string.rep("x", 1024)
wrk.headers["Content-Type"] = "application/octet-stream"
