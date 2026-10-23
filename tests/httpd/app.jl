# The application tests/httpd/test_basic.sh runs behind httpd:
# examples/app.jl plus routes for what only a proxy in front makes visible
# (header order, large streams, failures after the headers went out).

using JlServ

include(joinpath(@__DIR__, "..", "..", "jl_serv", "JlServ", "examples", "app.jl"))

# Response headers in a known order, with two Set-Cookie among them.
route!(app, "GET", "/order") do req, p
    Response(200, ["content-type" => "text/plain", "x-order-1" => "a", "set-cookie" => "one=1; Path=/",
            "x-order-2" => "b", "x-order-3" => "c", "set-cookie" => "two=2; Path=/", "x-order-4" => "d",
            "x-order-5" => "e"], "ordered\n")
end

# n zero bytes, streamed in 64 KiB pieces with a Content-Length.
stream!(app, "GET", "/zeros/{n}") do s, p
    n = parse(Int, p[:n])
    setheader!(s, "content-type" => "application/octet-stream")
    setheader!(s, "content-length" => string(n))
    buf = zeros(UInt8, 65536)
    while n > 0
        k = min(n, length(buf))
        write(s, view(buf, 1:k))
        n -= k
    end
end

# Answers without reading the body: with Expect: 100-continue, the client
# must never be told to send it.
route!(app, "POST", "/reject") do req, p
    Response(413, ["content-type" => "text/plain"], "too large\n")
end

# n bytes, flushed to the client, then a failure: the response must reach the
# client visibly cut short.
for (path, with_length) in (("/fail-after/{n}", false), ("/fail-after-cl/{n}", true))
    stream!(app, "GET", path) do s, p
        n = parse(Int, p[:n])
        setheader!(s, "content-type" => "application/octet-stream")
        with_length && setheader!(s, "content-length" => string(2n))
        write(s, fill(UInt8('x'), n))
        flush(s)
        error("deliberate failure after $n bytes")
    end
end

app
