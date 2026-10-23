# Easy: a greeter. A Router, a path parameter, a request header, and an
# explicit Response. Run it from the repository root:
#
#   julia --project=jl_serv/JlServ -t 4,1 jl_serv/JlServ/bin/jlserv \
#       jl_serv/JlServ/examples/easy/greeter.jl --unix /tmp/greeter.sock
#   jlc --unix /tmp/greeter.sock /hello/ada

using JlServ

app = Router()

# A handler may return a string: it becomes a 200 text/plain response.
route!(app, "GET", "/") do req, params
    "Try /hello/yourname or /teapot\n"
end

# `{name}` captures one path segment into params[:name].
route!(app, "GET", "/hello/{name}") do req, params
    agent = header(req.headers, "user-agent", "an unknown client")
    "Hello, $(params[:name])! You are using $agent.\n"
end

# Response(status, headers, body) when you need control of the status line.
route!(app, "GET", "/teapot") do req, params
    Response(418, ["content-type" => "text/plain", "x-brewing" => "earl grey"], "I'm a teapot\n")
end

# The file's last expression is the handler jlserv serves.
app
