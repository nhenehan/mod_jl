# Allocations per simple GET, worker side only. The transport replays one
# ForwardRequest frame forever and discards the output, so nothing but the
# worker's own work is counted. The plan's budget is 40; the connection's
# reused decoder and encoder (JExi reset!) brought a GET from 62 to 29.
# The Router adds path splitting and the params Dict on top.

# The transport (JlServ.ReplayIO) lives in src/warmup.jl.
replay_conn(handler) =
    JlServ.replay_conn(handler, JlServ.forward_frame(read(joinpath(VECTORS, "forward_get.exi")); req_id = 0x00000007))

function run_requests(c, n)
    for _ in 1:n
        h = JlServ.read_frame!(c)
        JlServ.handle_request(c, h) || error("connection not reusable")
    end
end

allocs_per_get(c; n = 1000) = (run_requests(c, 100); (@allocations run_requests(c, n)) / n)

const ALLOC_BUDGET = 40
const ALLOC_CEILING = (plain = 32, router = 48)

@testset "allocation budget per simple GET" begin
    plain = replay_conn(req -> "hello\n")
    app = Router()
    route!((req, p) -> Response("hello $(p[:name])\n"), app, "GET", "/app/hello/{name}")
    routed = replay_conn(app)

    a_plain, a_routed = allocs_per_get(plain), allocs_per_get(routed)
    @info "allocations per GET" plain = a_plain router = a_routed
    @test plain.io.written > 0
    @test a_plain <= ALLOC_BUDGET
    @test a_plain <= ALLOC_CEILING.plain
    @test a_routed <= ALLOC_CEILING.router
end

# The reused decoder must not carry anything from one request to the next.
head_fields(r) = (r.method, r.target, r.query, collect(r.headers), r.proto, r.is_ssl, r.remote_addr, r.remote_port,
    r.local_addr, r.local_port, r.server_name, r.server_port, r.auth_type, r.remote_user, r.route, r.body_length,
    r.env, r.ssl === nothing ? nothing : (r.ssl.cipher, r.ssl.key_size, r.ssl.protocol, r.ssl.session_id))

@testset "one connection decodes a series of different requests" begin
    many = JlServ.RequestHead(method = "PUT", target = "/a/b%20c", query = "x=1&y=2", remote_addr = "10.0.0.9",
        server_name = "example.org", route = "j3", body_length = -1, remote_user = "ada",
        headers = JlServ.Headers(vcat(["host" => "example.org", "accept" => "*/*"],
            ["x-h$i" => "value $i" for i in 1:24])),   # well past JExi's 16-value scan limit
        env = ["UNIQUE_ID" => "abc", "SSL_X" => "y"])
    few = JlServ.RequestHead(target = "/", remote_addr = "127.0.0.1", server_name = "localhost",
        headers = JlServ.Headers(["x-h3" => "value 3", "host" => "localhost"]))  # values seen in `many`
    payloads = [read(joinpath(VECTORS, "forward_get.exi")), JlServ.encode_forward(many),
        read(joinpath(VECTORS, "forward_post.exi")), JlServ.encode_forward(few), JlServ.encode_forward(many),
        read(joinpath(VECTORS, "forward_get.exi"))]
    seen = Any[]
    stream = reduce(vcat, [JlServ.forward_frame(p; req_id = UInt32(i)) for (i, p) in enumerate(payloads)])
    c = JlServ.replay_conn(req -> (push!(seen, head_fields(req.head)); "ok"), stream)
    for _ in payloads
        JlServ.handle_request(c, JlServ.read_frame!(c))
    end
    @test length(seen) == length(payloads)
    for (i, p) in enumerate(payloads)
        @test seen[i] == head_fields(JlServ.decode_forward(p))
    end
end
