# The application tests/httpd/test_lb.sh runs on each of its workers. Every
# route sits under /{b}/, so each JlBalancer of the test can have its own
# prefix.
# JL_T_NAME is the worker's name (j1, j2, j3).

using JlServ

const NAME = get(ENV, "JL_T_NAME", "j?")
const app = Router()

whoami(req, p) = Response("$NAME\n")
route!(whoami, app, "GET", "/{b}/whoami")
route!(whoami, app, "POST", "/{b}/whoami")
# For a session id in a path parameter: /{b}/p/;jsessionid=x.j2/whoami
route!(whoami, app, "GET", "/{b}/p/{param}/whoami")

# A new session id: it ends in the route mod_jl elected.
route!(app, "GET", "/{b}/session") do req, p
    Response(session_id(req) * "\n")
end

route!(app, "GET", "/{b}/sleep/{ms}") do req, p
    sleep(parse(Int, p[:ms]) / 1000)
    Response("$NAME\n")
end

# j1 answers with the status in the path, the others are fine.
route!(app, "GET", "/{b}/flaky/{code}") do req, p
    NAME == "j1" ? Response(parse(Int, p[:code]), ["content-type" => "text/plain"], "$NAME is unwell\n") :
    Response("$NAME\n")
end

# The worker named in ?who= dies with the request read and no answer sent;
# the others answer.
for method in ("GET", "POST")
    route!(app, method, "/{b}/die") do req, p
        method == "POST" && read(req.body)
        if req.query == "who=$NAME"
            println(stderr, "jlserv: $NAME dying on $method /die")
            flush(stderr)
            ccall(:_exit, Cvoid, (Cint,), 3)
        end
        Response("$NAME\n")
    end
end

app
