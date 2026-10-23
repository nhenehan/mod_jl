/* Frames over one worker socket: a read-ahead buffer, so a small response is
 * one recv, and one sendv per frame. */
#include "mod_jl.h"

#include <string.h>

void jl_io_init(jl_io_t *io, apr_socket_t *sock, apr_pool_t *p, uint32_t max_frame, apr_interval_time_t timeout)
{
    size_t cap = 2 * ((size_t)JLP_FRAME_HDR + max_frame);
    io->sock = sock;
    io->max_frame = max_frame;
    if (!io->buf || io->cap != cap) { /* a pooled connection keeps its buffer across reconnects */
        io->cap = cap;
        io->buf = apr_palloc(p, io->cap);
    }
    io->start = io->end = 0;
    io->timeout = timeout;
}

/* Reads until at least need bytes are buffered. */
static apr_status_t fill(jl_io_t *io, size_t need)
{
    while (io->end - io->start < need) {
        if (io->cap - io->start < need) { /* not enough room after start: move the unread bytes to the front */
            memmove(io->buf, io->buf + io->start, io->end - io->start);
            io->end -= io->start;
            io->start = 0;
        }
        apr_size_t n = io->cap - io->end;
        apr_status_t rv = apr_socket_recv(io->sock, (char *)io->buf + io->end, &n);
        io->end += n;
        if (rv != APR_SUCCESS && !(APR_STATUS_IS_EOF(rv) && n > 0))
            return rv;
        if (n == 0)
            return APR_EOF;
    }
    return APR_SUCCESS;
}

apr_status_t jl_io_read_frame(jl_io_t *io, jlp_frame_hdr *h, const uint8_t **payload)
{
    apr_status_t rv = fill(io, JLP_FRAME_HDR);
    if (rv != APR_SUCCESS)
        return rv;
    if (jlp_hdr_unpack(io->buf + io->start, io->max_frame, h) != JLP_OK)
        return JL_IO_EPROTO;
    if ((rv = fill(io, JLP_FRAME_HDR + (size_t)h->len)) != APR_SUCCESS)
        return rv;
    *payload = io->buf + io->start + JLP_FRAME_HDR;
    io->start += JLP_FRAME_HDR + h->len;
    if (io->start == io->end)
        io->start = io->end = 0;
    return APR_SUCCESS;
}

apr_status_t jl_io_read_frame_within(jl_io_t *io, jlp_frame_hdr *h, const uint8_t **payload,
                                     apr_interval_time_t timeout)
{
    apr_socket_timeout_set(io->sock, timeout);
    apr_status_t rv = jl_io_read_frame(io, h, payload);
    apr_socket_timeout_set(io->sock, io->timeout);
    return rv;
}

int jl_io_ready(jl_io_t *io)
{
    if (io->end > io->start)
        return 1;
    io->start = io->end = 0;
    apr_size_t n = io->cap;
    apr_socket_timeout_set(io->sock, 0);
    apr_status_t rv = apr_socket_recv(io->sock, (char *)io->buf, &n);
    apr_socket_timeout_set(io->sock, io->timeout);
    io->end = n;
    /* Data, EOF or an error: the next read will not wait for the worker. */
    return n > 0 || !APR_STATUS_IS_EAGAIN(rv);
}

apr_status_t jl_io_send(jl_io_t *io, uint8_t type, uint8_t flags, uint32_t req_id, const struct iovec *parts,
                        int n_parts)
{
    uint8_t hdr[JLP_FRAME_HDR];
    size_t len = 0;
    for (int i = 0; i < n_parts; i++)
        len += parts[i].iov_len;
    if (len > io->max_frame)
        return JL_IO_EPROTO;
    jlp_frame_hdr h = {.type = type, .flags = flags, .req_id = req_id, .len = (uint32_t)len};
    jlp_hdr_pack(hdr, &h);

    struct iovec iov[8];
    int n = 0;
    iov[n++] = (struct iovec){.iov_base = hdr, .iov_len = sizeof hdr};
    for (int i = 0; i < n_parts && n < 8; i++)
        if (parts[i].iov_len)
            iov[n++] = parts[i];

    /* sendv may write part of it: advance through the pieces until all is out. */
    struct iovec *v = iov;
    while (n > 0) {
        apr_size_t sent;
        apr_status_t rv = apr_socket_sendv(io->sock, v, n, &sent);
        if (rv != APR_SUCCESS && !(APR_STATUS_IS_EAGAIN(rv) && sent > 0))
            return rv;
        while (n > 0 && sent >= v->iov_len) {
            sent -= v->iov_len;
            v++;
            n--;
        }
        if (n > 0) {
            v->iov_base = (char *)v->iov_base + sent;
            v->iov_len -= sent;
        }
    }
    return APR_SUCCESS;
}
