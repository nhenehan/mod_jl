# Medium: a notes REST API. Shared state behind a lock, reading a request
# body with a size limit, status codes, and conditional requests (ETag,
# If-None-Match, If-Match). Run it from the repository root:
#
#   julia --project=jl_serv/JlServ -t 4,1 jl_serv/JlServ/bin/jlserv \
#       jl_serv/JlServ/examples/medium/notes.jl --unix /tmp/notes.sock
#   jlc --unix /tmp/notes.sock -X PUT --data 'buy milk' /notes/shopping
#   jlc --unix /tmp/notes.sock -i /notes/shopping

using JlServ
using SHA: sha1

const MAX_NOTE = 1 << 20  # bytes

# Handlers run concurrently, one task per connection, so shared state needs
# a lock.
const NOTES = Dict{String,Vector{UInt8}}()
const NOTES_LOCK = ReentrantLock()

# Path parameters arrive still %-encoded; accept a safe alphabet only.
valid_id(id) = occursin(r"^[A-Za-z0-9_-]{1,64}$", id)

etag(body) = "\"" * bytes2hex(sha1(body))[1:16] * "\""

text(status, msg) = Response(status, ["content-type" => "text/plain"], msg * "\n")

"""The whole body, or `nothing` if it is longer than `limit` bytes."""
function read_limited(body, limit)
    out = UInt8[]
    buf = Vector{UInt8}(undef, 64 * 1024)
    while !eof(body)
        n = readbytes!(body, buf)
        length(out) + n > limit && return nothing
        append!(out, view(buf, 1:n))
    end
    return out
end

app = Router()

route!(app, "GET", "/notes") do req, params
    ids = lock(() -> sort!(collect(keys(NOTES))), NOTES_LOCK)
    text(200, join(ids, "\n"))
end

# GET also answers HEAD: the router maps it, and JlServ drops the body.
route!(app, "GET", "/notes/{id}") do req, params
    note = lock(() -> get(NOTES, params[:id], nothing), NOTES_LOCK)
    note === nothing && return text(404, "no such note")
    tag = etag(note)
    header(req.headers, "if-none-match") == tag && return Response(304, ["etag" => tag], nothing)
    Response(200, ["content-type" => "text/plain", "etag" => tag], note)
end

route!(app, "PUT", "/notes/{id}") do req, params
    id = params[:id]
    valid_id(id) || return text(400, "ids are 1-64 of A-Z a-z 0-9 _ -")
    # body_length is the declared length, -1 when chunked, nothing if none.
    n = req.body_length
    n !== nothing && n > MAX_NOTE && return text(413, "notes are limited to $MAX_NOTE bytes")
    body = read_limited(req.body, MAX_NOTE)
    body === nothing && return text(413, "notes are limited to $MAX_NOTE bytes")

    expected = header(req.headers, "if-match")
    lock(NOTES_LOCK) do
        old = get(NOTES, id, nothing)
        # Optimistic concurrency: only replace the version the client saw.
        if expected !== nothing && (old === nothing || etag(old) != expected)
            return text(412, "the note has changed")
        end
        NOTES[id] = body
        tag = etag(body)
        old === nothing ? Response(201, ["location" => "/notes/$id", "etag" => tag], nothing) :
        Response(204, ["etag" => tag], nothing)
    end
end

route!(app, "DELETE", "/notes/{id}") do req, params
    found = lock(() -> pop!(NOTES, params[:id], nothing), NOTES_LOCK)
    found === nothing ? text(404, "no such note") : Response(204, Pair{String,String}[], nothing)
end

app
