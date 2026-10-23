# The example and test application. Run it with bin/jlserv:
#   julia --project=jl_serv/JlServ -t 4,1 jl_serv/JlServ/bin/jlserv jl_serv/JlServ/examples/app.jl --unix /tmp/j1.sock

using JlServ

const NAME = get(ENV, "JLSERV_NAME", "jlserv")

app = Router()

route!(app, "GET", "/hello/{name}") do req, p
    Response("hello $(p[:name])\n")
end

# Which worker answered; mod_jl's load-balancing tests count these.
route!(app, "GET", "/whoami") do req, p
    Response("$NAME\n")
end

# The request head as text, in arrival order.
route!(app, "GET", "/headers") do req, p
    io = IOBuffer()
    println(io, req.method, " ", req.target, req.query === nothing ? "" : "?" * req.query)
    for (k, v) in req.headers
        println(io, k, ": ", v)
    end
    Response(200, ["content-type" => "text/plain"], take!(io))
end

# Streams the request body back as it arrives, in constant memory.
stream!(app, "POST", "/echo") do s, p
    setheader!(s, "content-type" => something(header(s.request.headers, "content-type"), "application/octet-stream"))
    startwrite(s)
    while !eof(s)
        write(s, readavailable(s))
    end
end

# Reads the whole body, answers with its length.
route!(app, "POST", "/count") do req, p
    Response("$(length(read(req.body)))\n")
end

route!(app, "GET", "/bytes/{n}") do req, p
    n = parse(Int, p[:n])
    Response(200, ["content-type" => "application/octet-stream"], fill(UInt8('x'), n))
end

# n lines, each flushed through to the client.
stream!(app, "GET", "/stream/{n}") do s, p
    setheader!(s, "content-type" => "text/plain")
    for i in 1:parse(Int, p[:n])
        println(s, "line $i")
        flush(s)
    end
end

route!(app, "GET", "/sleep/{ms}") do req, p
    sleep(parse(Int, p[:ms]) / 1000)
    Response("slept $(p[:ms]) ms\n")
end

route!(app, "GET", "/boom") do req, p
    error("boom")
end

route!(app, "GET", "/status/{code}") do req, p
    Response(parse(Int, p[:code]), ["content-type" => "text/plain"], "status $(p[:code])\n")
end

app
