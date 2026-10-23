# The application tests/httpd/test_pool.sh runs behind httpd. Every route
# sits under /{w}/, so one worker can serve every JlWorker of the test, each
# mounted on its own prefix.

using JlServ

const app = Router()

route!(app, "GET", "/{w}/hello") do req, p
    Response("hello\n")
end

# The worker's pid: which process answered.
route!(app, "GET", "/{w}/pid") do req, p
    Response("$(getpid())\n")
end

route!(app, "GET", "/{w}/sleep/{ms}") do req, p
    sleep(parse(Int, p[:ms]) / 1000)
    Response("slept\n")
end

route!(app, "POST", "/{w}/count") do req, p
    Response("$(length(read(req.body)))\n")
end

# Dies with the request read and no answer sent: what a crash in a handler
# looks like from httpd.
for method in ("GET", "POST")
    route!(app, method, "/{w}/die") do req, p
        method == "POST" && read(req.body)
        println(stderr, "jlserv: dying on $method /die")
        flush(stderr)
        ccall(:_exit, Cvoid, (Cint,), 3)
    end
end

app
