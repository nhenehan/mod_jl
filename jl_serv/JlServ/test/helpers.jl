# Request helpers (src/helpers.jl) and warmup (src/warmup.jl).

@testset "unescapeuri" begin
    @test unescapeuri("a%20b") == "a b"
    @test unescapeuri("caf%C3%A9") == "café"
    @test unescapeuri("a+b") == "a+b"
    @test unescapeuri("a+b"; plus = true) == "a b"
    @test unescapeuri("%2B"; plus = true) == "+"
    @test unescapeuri("100%") == "100%"          # malformed escapes are kept
    @test unescapeuri("%zz%4") == "%zz%4"
    @test unescapeuri("%4a%4A") == "JJ"           # either case of hex
    @test !isvalid(unescapeuri("%FF"))           # not UTF-8, still a String
    s = "plain"
    @test unescapeuri(s) == s
end

@testset "queryparams" begin
    @test queryparams("q=caf%C3%A9+au+lait&tag=a&tag=b") == ["q" => "café au lait", "tag" => "a", "tag" => "b"]
    @test queryparams("a=1&&b&c=") == ["a" => "1", "b" => "", "c" => ""]
    @test queryparams("k=a=b") == ["k" => "a=b"]
    @test queryparams("") == Pair{String,String}[]
    @test queryparams(nothing) == Pair{String,String}[]
    @test Dict(queryparams("x=1&x=2"))["x"] == "2"
end

@testset "session_id and warmup" begin
    seen = String[]
    app = Router()
    route!(app, "GET", "/s") do req, p
        push!(seen, session_id(req), string(queryparams(req)))
        "ok\n"
    end
    warmup(app; paths = ["/s?a=1"])
    id, q = seen
    @test occursin(r"^[0-9a-f]{32}$", id)           # warmup requests carry no route
    @test q == string(["a" => "1"])
    warmup(app; paths = ["/s"])
    @test seen[3] != id                               # fresh each time

    head = JlServ.RequestHead(target = "/s", route = "j2", remote_addr = "127.0.0.1", server_name = "t")
    c = JlServ.replay_conn(app, JlServ.forward_frame(JlServ.encode_forward(head)))
    JlServ.handle_request(c, JlServ.read_frame!(c))
    @test occursin(r"^[0-9a-f]{32}\.j2$", seen[5])
end

@testset "router: 404, and 405 with Allow" begin
    app = Router()
    route!((req, p) -> "x", app, "GET", "/cars/{vin}")
    route!((req, p) -> "x", app, "DELETE", "/cars/{vin}")
    route!((req, p) -> "x", app, "PUT", "/cars/{vin}")
    stream!((s, p) -> nothing, app, "POST", "/cars/{vin}/inspections")
    @test JlServ.allowed_methods(app, "/cars/X") == ["GET", "HEAD", "PUT", "DELETE"]
    @test JlServ.allowed_methods(app, "/cars/X/inspections") == ["POST"]
    @test isempty(JlServ.allowed_methods(app, "/nope"))
    r = JlServ.unmatched_response(app, "/cars/X")
    @test r.status == 405
    @test ("allow" => "GET, HEAD, PUT, DELETE") in r.headers
    @test JlServ.unmatched_response(app, "/nope").status == 404
    route!((req, p) -> "x", app, "*", "/any")
    @test JlServ.allowed_methods(app, "/any") == ["*"]
end

@testset "draining(s) tells a stream the worker is stopping" begin
    seen = Bool[]
    app = Router()
    stream!(app, "GET", "/app/hello/{name}") do s, p
        push!(seen, draining(s))
        write_response(s, Response("ok\n"))
    end
    head = JlServ.RequestHead(target = "/app/hello/x", remote_addr = "127.0.0.1", server_name = "t")
    c = JlServ.replay_conn(app, JlServ.forward_frame(JlServ.encode_forward(head)))
    JlServ.handle_request(c, JlServ.read_frame!(c))
    c.server.draining = true
    JlServ.handle_request(c, JlServ.read_frame!(c))
    @test seen == [false, true]
end
