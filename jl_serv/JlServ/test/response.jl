# Response bodies through a whole request (replay_conn is in alloc.jl).

@testset "an IO response body is streamed and closed" begin
    io = IOBuffer("from an IO\n")
    c = replay_conn(req -> Response(200, ["content-type" => "text/plain"], io))
    JlServ.handle_request(c, JlServ.read_frame!(c))
    @test !isopen(io)
    @test c.io.written > length("from an IO\n")
end
