# Difficult: an upload digest service with live progress.
#
#   POST /uploads/{id}         streams the body through SHA-256 in constant
#                              memory, writing progress lines back as it goes
#   GET  /uploads/{id}/events  server-sent events for that upload, from any
#                              other connection, until it finishes
#   GET  /uploads/{id}         the result: 202 while running, 200 when done
#
# It shows streaming in both directions on one request, flushing, tasks that
# wait on each other across connections (Threads.Condition), a heartbeat that
# notices a client that went away, and failing cleanly mid-stream. Run it
# from the repository root:
#
#   julia --project=jl_serv/JlServ -t 4,1 jl_serv/JlServ/bin/jlserv \
#       jl_serv/JlServ/examples/difficult/uploads.jl --unix /tmp/up.sock
#   jlc --unix /tmp/up.sock /uploads/u1/events &
#   head -c 50M /dev/urandom | jlc --unix /tmp/up.sock -X POST --chunked --data-file - /uploads/u1

using JlServ
using SHA

const PROGRESS_EVERY = 4 << 20  # bytes between progress lines in the POST reply
const HEARTBEAT = 15.0          # seconds between keep-alive comments on idle event streams

mutable struct Upload
    bytes::Int
    lines::Int
    sha256::Union{Nothing,String}  # set when the upload is complete
    error::Union{Nothing,String}   # set if it failed
    started::Bool
    const cond::Threads.Condition  # guards the fields above
end
Upload() = Upload(0, 0, nothing, nothing, false, Threads.Condition())

finished(u::Upload) = u.sha256 !== nothing || u.error !== nothing

const UPLOADS = Dict{String,Upload}()
const UPLOADS_LOCK = ReentrantLock()

# Either side may arrive first: a subscriber can wait for an upload that has
# not started yet.
upload!(id) = lock(() -> get!(Upload, UPLOADS, id), UPLOADS_LOCK)
lookup(id) = lock(() -> get(UPLOADS, id, nothing), UPLOADS_LOCK)

valid_id(id) = occursin(r"^[A-Za-z0-9_-]{1,64}$", id)

function publish!(f, u::Upload)
    lock(u.cond) do
        f(u)
        notify(u.cond)
    end
end

app = Router()

stream!(app, "POST", "/uploads/{id}") do s, params
    id = params[:id]
    if !valid_id(id)
        setstatus!(s, 400)
        return write(s, "ids are 1-64 of A-Z a-z 0-9 _ -\n")
    end
    u = upload!(id)
    claimed = lock(u.cond) do
        u.started ? false : (u.started = true)
    end
    if !claimed
        setstatus!(s, 409)
        return write(s, "upload $id already exists\n")
    end

    # Headers go out now; the body is read while the reply streams.
    setheader!(s, "content-type" => "text/plain")
    startwrite(s)

    ctx = SHA2_256_CTX()
    buf = Vector{UInt8}(undef, 64 * 1024)
    body = s.request.body  # read the RequestBody directly, in large blocks
    total = lines = 0
    next = PROGRESS_EVERY
    try
        while !eof(body)
            n = readbytes!(body, buf)
            chunk = view(buf, 1:n)
            SHA.update!(ctx, chunk)
            total += n
            lines += count(==(UInt8('\n')), chunk)
            publish!(u) do u
                u.bytes = total
                u.lines = lines
            end
            if total >= next
                println(s, "received $total bytes")
                flush(s)  # send it now instead of when the buffer fills
                next += PROGRESS_EVERY
            end
        end
    catch err
        # Tell the subscribers, then let JlServ handle the broken request.
        publish!(u -> u.error = sprint(showerror, err), u)
        rethrow()
    end
    digest = bytes2hex(SHA.digest!(ctx))
    publish!(u -> u.sha256 = digest, u)
    println(s, "done: $total bytes, $lines lines, sha256 $digest")
end

stream!(app, "GET", "/uploads/{id}/events") do s, params
    id = params[:id]
    valid_id(id) || (setstatus!(s, 400); return write(s, "bad id\n"))
    u = upload!(id)
    setheader!(s, "content-type" => "text/event-stream")
    setheader!(s, "cache-control" => "no-cache")
    startwrite(s)

    # Wake up now and then even if nothing happens: writing the keep-alive is
    # how a client that disconnected is noticed (the write throws).
    beat = Timer(_ -> lock(() -> notify(u.cond), u.cond), HEARTBEAT; interval = HEARTBEAT)
    try
        seen = -1
        while true
            bytes, lines, digest, err = lock(u.cond) do
                (u.bytes == seen && !finished(u)) && wait(u.cond)
                (u.bytes, u.lines, u.sha256, u.error)
            end
            if err !== nothing
                write(s, "event: failed\ndata: $err\n\n")
                break
            elseif digest !== nothing
                write(s, "event: done\ndata: bytes=$bytes lines=$lines sha256=$digest\n\n")
                break
            elseif bytes == seen
                write(s, ": keep-alive\n\n")
            else
                write(s, "event: progress\ndata: bytes=$bytes lines=$lines\n\n")
                seen = bytes
            end
            flush(s)
        end
    finally
        close(beat)
    end
end

route!(app, "GET", "/uploads/{id}") do req, params
    u = lookup(params[:id])
    u === nothing && return Response(404, ["content-type" => "text/plain"], "no such upload\n")
    status, msg = lock(u.cond) do
        u.error !== nothing ? (500, "failed: $(u.error)") :
        u.sha256 !== nothing ? (200, "bytes=$(u.bytes) lines=$(u.lines) sha256=$(u.sha256)") :
        (202, "in progress: bytes=$(u.bytes)")
    end
    Response(status, ["content-type" => "text/plain"], msg * "\n")
end

app
