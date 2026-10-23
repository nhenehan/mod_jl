"""
    JlServ

Julia worker for mod_jl: Apache httpd forwards requests over the jlp v2
protocol (12-byte frames carrying strict, byte-aligned EXI documents of
schema/jlp2.xsd) and JlServ runs them through a handler.
"""
module JlServ

import JExi as J
import Sockets
using SHA: sha256

export Request, Response, Headers, Router, Stream, RequestBody
export route!, stream!, setstatus!, setheader!, startwrite, header, headers, hasheader
export serve, start, shutdown!, warmup
export unescapeuri, queryparams, session_id

include("gen/vocab.jl")
include("gen/jlp_tables.jl")
# The generated module exports nothing; bring in its tables and LN_* QName ids.
for n in names(JlpTables; all = true)
    s = string(n)
    (s == "TABLES" || startswith(s, "LN_")) && @eval const $n = JlpTables.$n
end

include("frame.jl")
include("types.jl")
include("codec.jl")
include("conn.jl")
include("bodyio.jl")
include("router.jl")
include("server.jl")
include("helpers.jl")
include("warmup.jl")

using PrecompileTools: @setup_workload, @compile_workload

@setup_workload begin
    app = Router()
    route!((req, p) -> "hello $(p[:name])\n", app, "GET", "/hello/{name}")
    @compile_workload begin
        warmup(app; paths = ["/hello/world?a=1", "/nope"])
        warmup(req -> Response(200, ["content-type" => "text/plain"], UInt8[0x6f, 0x6b]))
        queryparams("a=1&b=%20x+y")
    end
end

end # module JlServ
