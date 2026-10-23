# Unix socket takeover (src/server.jl): a new server on a path in use takes
# it over, and the old one leaves it alone when it stops.

using JlServ: Sockets

@testset "unix socket takeover" begin
    dir = mktempdir()
    path = joinpath(dir, "j.sock")
    app = Router()
    old = start(app; unix = path, name = "old")
    @test issocket(path)
    new = start(app; unix = path, name = "new")
    @test stat(path).inode == new.unix_paths[1][3]
    @test readdir(dir) == ["j.sock"]     # the bound names were renamed away
    close(Sockets.connect(path))  # reaches a listener
    shutdown!(old; grace = 0, timeout = 1)
    @test issocket(path)                 # the old server left the new one's socket
    close(Sockets.connect(path))
    shutdown!(new; grace = 0, timeout = 1)
    @test !ispath(path)                  # its own, it removes
    @test isempty(readdir(dir))
end
