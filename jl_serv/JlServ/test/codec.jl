# Interop with libjlproto: both sides must produce exactly the rexi CLI reference
# vectors in testdata/vectors (made from schema/samples by tools/make-vectors.sh).

const VECTORS = joinpath(@__DIR__, "..", "..", "..", "testdata", "vectors")
vector(name) = read(joinpath(VECTORS, name * ".exi"))

@testset "vocab matches the schema" begin
    xsd = joinpath(@__DIR__, "..", "..", "..", "schema", "jlp2.xsd")
    @test JlServ.SCHEMA_SHA256 == JlServ.sha256(read(xsd))
end

@testset "decode ForwardRequest vectors" begin
    r = JlServ.decode_forward(vector("forward_get"))
    @test r.method == "GET"
    @test r.target == "/app/hello/world"
    @test r.query == "a=1&b=2"
    @test r.proto == 1001 && !r.is_ssl
    @test r.remote_addr == "127.0.0.1" && r.remote_port == 51234
    @test r.server_name == "localhost" && r.server_port == 18080
    @test r.route == "j1" && r.body_length === nothing && r.inline_body === nothing
    @test collect(r.headers) == ["host" => "localhost:18080", "user-agent" => "curl/8.5.0", "accept" => "*/*",
        "X-Trace" => "abc123", "cookie" => "JSESSIONID=abc.j1"]
    @test JlServ.header(r.headers, "Host") == "localhost:18080"
    @test JlServ.header(r.headers, "x-trace") == "abc123"

    p = JlServ.decode_forward(vector("forward_post"))
    @test p.method == "BREW"
    @test p.is_ssl && p.proto == 2000
    @test p.auth_type == "Basic" && p.remote_user == "alice"
    @test p.local_addr == "10.0.0.1" && p.local_port == 443
    @test p.body_length == 11
    @test p.ssl.cipher == "TLS_AES_128_GCM_SHA256" && p.ssl.key_size == 128 && p.ssl.protocol == "TLSv1.3"
    @test p.env == ["UNIQUE_ID" => "Zx1"]
    @test String(copy(p.inline_body)) == "Hello World"

    # Re-encoding what was decoded gives the same bytes.
    @test JlServ.encode_forward(r) == vector("forward_get")
    @test JlServ.encode_forward(p) == vector("forward_post")
end

@testset "encode worker messages = vectors" begin
    @test JlServ.encode_send_headers(200, "OK", ["Content-Type" => "text/plain; charset=utf-8",
        "set-cookie" => "a=1", "Set-Cookie" => "b=2", "X-Powered-By" => "JlServ"]) == vector("send_headers")
    @test JlServ.encode_end_response(true) == vector("end_response")
    @test JlServ.encode_shutdown(true, "restart") == vector("shutdown")
    @test JlServ.encode_error(500, "handler threw") == vector("error")
    @test JlServ.CPING_DOC == vector("cping")
    @test JlServ.CPONG_DOC == vector("cpong")

    b = zeros(UInt8, 32)
    n = JlServ.get_body_chunk!(b, 1, 65000) - 1
    @test b[1:n] == vector("get_body_chunk")

    v = vector("body_chunk")
    start, len = JlServ.body_view(v, 1, length(v))
    @test String(v[start:start+len-1]) == "Hello World"
    n = JlServ.body_prefix!(b, 1, 11) - 1
    @test b[1:n] == v[1:start-1]
    @test JlServ.body_overhead(11) == n
    @test_throws JlServ.ProtocolError JlServ.body_view(v, 1, length(v) - 1)
end

@testset "edge cases round-trip" begin
    r = RequestHead(method = "M-SEARCH", target = "/caf\xe9/x", headers = Headers([
            "X-Empty" => "", "X-Space" => "  ", "ACCEPT" => "text/html", "X-Latin1" => "caf\xe9", "x-\xff" => "v"]),
        remote_addr = "1.2.3.4", remote_port = 1, server_name = "h", server_port = 80, body_length = -1,
        env = ["E1" => "", "E2" => "x"])
    back = JlServ.decode_forward(JlServ.encode_forward(r))
    @test back.method == "M-SEARCH"
    @test codeunits(back.target) == codeunits("/caf\xe9/x")
    @test collect(back.headers) == ["X-Empty" => "", "X-Space" => "  ", "accept" => "text/html",
        "X-Latin1" => "caf\xe9", "x-\xff" => "v"]
    @test back.env == ["E1" => "", "E2" => "x"]
    @test back.body_length == -1

    # Out-of-schema values fail loudly rather than corrupting a frame.
    @test_throws ArgumentError JlServ.encode_send_headers(1000, nothing, [])
end

@testset "framing and handshake" begin
    b = zeros(UInt8, 12)
    JlServ.pack_header!(b, 1, JlServ.T_SEND_HEADERS, JlServ.F_FLUSH, 0xa1b2c3d4, 70)
    h = JlServ.unpack_header(b, 65536)
    @test h.type == UInt8(JlServ.T_SEND_HEADERS) && h.flags == JlServ.F_FLUSH && h.req_id == 0xa1b2c3d4 && h.len == 70
    @test_throws JlServ.ProtocolError JlServ.unpack_header(b, 69)

    hello = JlServ.Hello(0, JlServ.FEAT_BYTE_ALIGNED | JlServ.FEAT_SECRET, 65536, JlServ.SCHEMA_SHA256,
        Vector{UInt8}("s3cret"), "mod_jl/0.1")
    p = JlServ.pack_hello(hello)
    @test length(p) == 60
    h2 = JlServ.unpack_hello(p)
    @test h2.secret == hello.secret && h2.agent == "mod_jl/0.1" && h2.max_frame == 65536
    @test_throws JlServ.ProtocolError JlServ.unpack_hello(p[1:end-1])
end
