/* The request handler: choose a worker, forward the request to it and relay
 * the answer; on failure, perhaps try another.
 *
 * The exchange follows AJP: FORWARD_REQUEST (with the first body block inline),
 * then GET_BODY_CHUNK/BODY_CHUNK while the worker reads the body, then
 * SEND_HEADERS, BODY_CHUNK..., END_RESPONSE.
 *
 * Nothing goes to the client until the first body bytes (or the end), so a
 * worker failure before that can still become a clean 502. After that, a
 * failure can only cut the response short, which the client can see: the
 * connection closes before the declared length, or a chunked response ends
 * without its last chunk.
 *
 * Each attempt ends with an outcome, which says what the worker did (and so
 * what its state becomes) and whether another worker may try the request. */
#include "mod_jl.h"

#include "apr_atomic.h"
#include "apr_lib.h"
#include "apr_strings.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "http_ssl.h"
#include "util_filter.h"

#include <string.h>

APLOG_USE_MODULE(jl);

/* What exchange() and send_forward() return when the request may go again on
 * a new connection: a pooled one turned out dead before the worker saw it. */
#define RETRY (-100)

/* How an attempt ended. */
#define OUT_ANSWERED 0    /* the worker answered, whatever it said */
#define OUT_UNDELIVERED 1 /* the request never reached it: connect, HELLO, CPing, sending */
#define OUT_POOL 2        /* no free connection to it: nothing wrong with the worker */
#define OUT_NOANSWER 3    /* it had the request, but failed before anything reached the client */
#define OUT_FAIL_STATUS 4 /* it answered with a positive fail_on_status */
#define OUT_BROKEN 5      /* it broke the protocol */
#define OUT_CUT 6         /* it failed after the response started */
#define OUT_CLIENT 7      /* the client went away, or its body broke off */
#define OUT_LOCAL 8       /* a problem on our side (encoding) */

typedef struct {
    request_rec *r;
    conn_rec *c;
    jl_balancer_t *lb;
    jl_member_t *m;    /* this attempt's member */
    int probe;         /* ... which this request is probing (recovery) */
    int busy;          /* ... whose busy count this request holds */
    const jl_worker_t *w; /* m->w */
    int outcome;          /* OUT_* */
    int reply_timedout;   /* the NOANSWER was a reply_timeout */
    int body_streamed;    /* body read from the client after the inline block: no replay */
    apr_off_t body_sent;  /* request body bytes sent to the worker */
    jl_conn_t *conn; /* NULL once released */
    jl_io_t *io;     /* &conn->io */
    uint32_t id;     /* this exchange's req_id */
    int may_retry;   /* a reused connection, first attempt: see send_forward and exchange */
    int idempotent;
    int frames;       /* received from the worker */
    int saw_shutdown; /* the worker is draining: do not reuse the connection */
    apr_bucket_brigade *bb; /* to the client */
    apr_bucket_brigade *ib; /* from the client */

    /* Request body */
    int has_body;           /* a body to forward (not none, not empty) */
    int body_eos;           /* the client's body has ended */
    uint8_t *bodybuf;       /* max_frame bytes: what was last read from the client */
    const uint8_t *pending; /* read but not yet sent (an inline block that did not fit) */
    size_t pending_len;

    /* Response */
    int headers_seen;
    int committed;         /* something has gone to the client */
    int unflushed;         /* data passed since the last flush */
    apr_off_t declared_cl; /* the worker's Content-Length, or -1 */
    apr_off_t sent_bytes;
    const char *bad; /* why a header was refused */
} jl_req_t;

/* ===== Request: headers ===== */

static int is_hop_by_hop(const char *name)
{
    static const char *const hop[] = {"connection", "keep-alive", "te",    "trailer", "transfer-encoding",
                                      "upgrade",    "expect",     "proxy", NULL};
    for (const char *const *h = hop; *h; h++)
        if (!strcasecmp(name, *h))
            return 1;
    return !strncasecmp(name, "proxy-", 6);
}

/* Headers named in Connection: are hop-by-hop too (RFC 9110 7.6.1). */
static int named_by_connection(apr_pool_t *p, const char *connection, const char *name)
{
    if (!connection)
        return 0;
    const char *list = connection;
    char *tok;
    while ((tok = ap_get_list_item(p, &list)) != NULL)
        if (!strcasecmp(tok, name))
            return 1;
    return 0;
}

static void collect_headers(jl_req_t *q, jlp_forward_request *fr)
{
    request_rec *r = q->r;
    const apr_array_header_t *arr = apr_table_elts(r->headers_in);
    const apr_table_entry_t *e = (const apr_table_entry_t *)arr->elts;
    const char *connection = apr_table_get(r->headers_in, "Connection");
    jlp_header *h = apr_palloc(r->pool, sizeof *h * (arr->nelts ? arr->nelts : 1));
    size_t n = 0;
    for (int i = 0; i < arr->nelts; i++) {
        if (!e[i].key || is_hop_by_hop(e[i].key) || named_by_connection(r->pool, connection, e[i].key))
            continue;
        h[n].name = e[i].key;
        h[n].value = e[i].val ? e[i].val : "";
        n++;
    }
    fr->headers = h;
    fr->n_headers = n;
}

static int fill_forward(jl_req_t *q, jlp_forward_request *fr)
{
    request_rec *r = q->r;
    conn_rec *c = q->c;
    memset(fr, 0, sizeof *fr);
    fr->method = r->method;
    /* The decoded, normalised path that mounts and <Location> matched,
     * escaped again: the worker routes on the same path httpd authorised. */
    fr->uri = ap_escape_uri(r->pool, r->uri);
    fr->query = r->args;
    collect_headers(q, fr);
    fr->proto = (uint16_t)r->proto_num;
    fr->is_ssl = ap_ssl_conn_is_ssl(c);
    fr->remote_addr = r->useragent_ip;
    fr->remote_port = r->useragent_addr ? r->useragent_addr->port : c->client_addr->port;
    fr->local_addr = c->local_ip;
    fr->local_port = c->local_addr ? c->local_addr->port : -1;
    fr->server_name = ap_get_server_name(r);
    fr->server_port = ap_get_server_port(r);
    fr->auth_type = r->ap_auth_type;
    fr->remote_user = r->user;

    const char *te = apr_table_get(r->headers_in, "Transfer-Encoding");
    const char *cl = apr_table_get(r->headers_in, "Content-Length");
    if (r->main) {
        fr->body_length = JLP_NO_BODY; /* a subrequest has no body of its own */
    } else if (te) {
        fr->body_length = -1; /* httpd has already refused anything but a final chunked */
    } else if (cl) {
        apr_off_t len;
        if (!ap_parse_strict_length(&len, cl)) {
            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "jl: invalid Content-Length %s", cl);
            return HTTP_BAD_REQUEST;
        }
        fr->body_length = len;
    } else {
        fr->body_length = JLP_NO_BODY;
    }
    q->has_body = fr->body_length != JLP_NO_BODY && fr->body_length != 0;
    q->body_eos = !q->has_body;
    return OK;
}

/* ===== Request: body ===== */

/* Reads up to want bytes of the client's body into bodybuf: one blocking read,
 * then whatever more is already there. Sets body_eos at the end of the body. */
static apr_status_t read_body(jl_req_t *q, size_t want, size_t *got)
{
    apr_read_type_e block = APR_BLOCK_READ;
    *got = 0;
    while (*got < want && !q->body_eos) {
        apr_status_t rv = ap_get_brigade(q->r->input_filters, q->ib, AP_MODE_READBYTES, block, want - *got);
        if (block == APR_NONBLOCK_READ &&
            (APR_STATUS_IS_EAGAIN(rv) || (rv == APR_SUCCESS && APR_BRIGADE_EMPTY(q->ib)))) {
            apr_brigade_cleanup(q->ib);
            break;
        }
        if (rv != APR_SUCCESS) {
            apr_brigade_cleanup(q->ib);
            return rv;
        }
        if (!APR_BRIGADE_EMPTY(q->ib) && APR_BUCKET_IS_EOS(APR_BRIGADE_LAST(q->ib)))
            q->body_eos = 1;
        apr_size_t n = want - *got;
        rv = apr_brigade_flatten(q->ib, (char *)q->bodybuf + *got, &n);
        apr_brigade_cleanup(q->ib);
        if (rv != APR_SUCCESS)
            return rv;
        *got += n;
        block = APR_NONBLOCK_READ;
    }
    return APR_SUCCESS;
}

/* ===== Response: headers ===== */

static int bad_header(jl_req_t *q, const char *why)
{
    q->bad = why;
    return 1;
}

static int on_status(void *ud, unsigned status, const char *reason, size_t reason_len)
{
    jl_req_t *q = ud;
    request_rec *r = q->r;
    if (status < 200 || status > 599)
        return bad_header(q, apr_psprintf(r->pool, "status %u", status));
    r->status = (int)status;
    int clean = reason_len > 0;
    for (size_t i = 0; i < reason_len && clean; i++)
        clean = (unsigned char)reason[i] >= 0x20 && reason[i] != 0x7f;
    r->status_line = clean ? apr_psprintf(r->pool, "%03u %.*s", status, (int)reason_len, reason)
                           : ap_get_status_line_ex(r->pool, (int)status);
    return 0;
}

static int is_token(const char *s, size_t n)
{
    static const char tchar[] = "!#$%&'*+-.^_`|~";
    if (n == 0)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!apr_isalnum(s[i]) && !strchr(tchar, s[i]))
            return 0;
    return 1;
}

static int clean_value(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\r' || s[i] == '\n' || s[i] == '\0')
            return 0;
    return 1;
}

static int on_header(void *ud, int ordinal, const char *name, size_t name_len, const char *value, size_t value_len,
                     int raw)
{
    jl_req_t *q = ud;
    request_rec *r = q->r;
    if (!is_token(name, name_len) || !clean_value(value, value_len))
        return bad_header(q, "a header name or value with forbidden characters");
    char *k = apr_pstrmemdup(r->pool, name, name_len);
    char *v = apr_pstrmemdup(r->pool, value, value_len);
    if (is_hop_by_hop(k)) /* httpd frames the response itself */
        return 0;
    if (!strcasecmp(k, "content-type")) {
        ap_set_content_type_ex(r, v, 0);
    } else if (!strcasecmp(k, "content-length")) {
        apr_off_t len;
        if (!ap_parse_strict_length(&len, v) || (q->declared_cl >= 0 && len != q->declared_cl))
            return bad_header(q, apr_psprintf(r->pool, "Content-Length %s", v));
        q->declared_cl = len;
        apr_table_setn(r->headers_out, "Content-Length", v);
        return 0;
    }
    /* addn keeps the worker's order and repeated headers (Set-Cookie). */
    apr_table_addn(r->headers_out, k, v);
    return 0;
}

static int on_trailer(void *ud, int ordinal, const char *name, size_t name_len, const char *value, size_t value_len,
                      int raw)
{
    jl_req_t *q = ud;
    if (!is_token(name, name_len) || !clean_value(value, value_len))
        return bad_header(q, "a trailer with forbidden characters");
    apr_table_addn(q->r->trailers_out, apr_pstrmemdup(q->r->pool, name, name_len),
                   apr_pstrmemdup(q->r->pool, value, value_len));
    return 0;
}

static const jlp_headers_visitor header_visitor = {.status = on_status, .header = on_header};
static const jlp_headers_visitor trailer_visitor = {.status = NULL, .header = on_trailer};

/* ===== Ending ===== */

/* Gives the connection back: kept for the next request only if keep. */
static void release_worker(jl_req_t *q, int keep)
{
    if (q->conn) {
        jl_conn_release(q->conn, keep);
        q->conn = NULL;
        q->io = NULL;
    }
}

static void close_worker(jl_req_t *q)
{
    release_worker(q, 0);
}

/* The member is no longer this request's: its busy count drops, and a probe
 * that ended without a verdict goes back to ERROR. */
static void leave_member(jl_req_t *q)
{
    if (q->busy) {
        apr_atomic_dec32(&q->m->w->shm->busy);
        q->busy = 0;
    }
    if (q->probe) {
        jl_lb_abandon_probe(q->m);
        q->probe = 0;
    }
}

/* However the handler ends, the slot goes back to the pool and the member
 * is left. */
static apr_status_t release_cleanup(void *data)
{
    close_worker(data);
    leave_member(data);
    return APR_SUCCESS;
}

/* The attempt failed with outcome. Before anything reached the client,
 * answer with status; after, cut the response short so the client sees it
 * is incomplete. */
static int fail(jl_req_t *q, int outcome, int status, apr_status_t rv, const char *what)
{
    request_rec *r = q->r;
    ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, "jl: worker %s (%s): %s", q->w->name, q->w->target, what);
    q->outcome = outcome;
    close_worker(q);
    if (!q->committed) {
        /* Forget what the worker said, or the error page inherits it. */
        r->status = HTTP_OK;
        r->status_line = NULL;
        r->content_type = NULL;
        apr_table_clear(r->headers_out);
        apr_table_clear(r->trailers_out);
        return status;
    }
    /* As mod_proxy does when a backend breaks mid-response. */
    r->no_cache = 1;
    q->c->keepalive = AP_CONN_CLOSE;
    apr_brigade_cleanup(q->bb);
    APR_BRIGADE_INSERT_TAIL(q->bb, ap_bucket_error_create(HTTP_BAD_GATEWAY, NULL, r->pool, q->c->bucket_alloc));
    APR_BRIGADE_INSERT_TAIL(q->bb, apr_bucket_eos_create(q->c->bucket_alloc));
    ap_pass_brigade(r->output_filters, q->bb);
    apr_brigade_cleanup(q->bb);
    return OK;
}

static int protocol_error(jl_req_t *q, const char *what)
{
    return fail(q, OUT_BROKEN, HTTP_BAD_GATEWAY, JL_IO_EPROTO, what);
}

/* Passes bb to the client. Returns OK to go on, or the handler's result. */
static int pass(jl_req_t *q, int *done)
{
    apr_status_t rv = ap_pass_brigade(q->r->output_filters, q->bb);
    apr_brigade_cleanup(q->bb);
    q->committed = 1;
    if (rv == AP_FILTER_ERROR) {
        q->outcome = OUT_CLIENT;
        close_worker(q);
        *done = 1;
        return AP_FILTER_ERROR;
    }
    if (rv != APR_SUCCESS || q->c->aborted) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, rv, q->r, "jl: client aborted");
        q->outcome = OUT_CLIENT;
        close_worker(q);
        *done = 1;
        return OK;
    }
    return OK;
}

/* ===== The exchange ===== */

static int send_body_chunk(jl_req_t *q, uint32_t max)
{
    size_t cap = q->io->max_frame - jlp_body_overhead(jl_ctx, q->io->max_frame);
    if (max > cap)
        max = (uint32_t)cap;
    const uint8_t *data = NULL;
    size_t n = 0;
    if (q->pending_len) {
        n = q->pending_len < max ? q->pending_len : max;
        data = q->pending;
        q->pending += n;
        q->pending_len -= n;
    } else if (!q->body_eos) {
        q->body_streamed = 1;
        apr_status_t rv = read_body(q, max, &n);
        if (rv != APR_SUCCESS) {
            ap_log_rerror(APLOG_MARK, APLOG_INFO, rv, q->r, "jl: reading the request body");
            q->c->keepalive = AP_CONN_CLOSE;
            if (!q->committed) {
                q->outcome = OUT_CLIENT;
                close_worker(q);
                return ap_map_http_request_error(rv, HTTP_BAD_REQUEST);
            }
            return fail(q, OUT_CLIENT, HTTP_BAD_GATEWAY, rv, "the client's body broke off");
        }
        data = q->bodybuf;
    }
    int last = q->body_eos && q->pending_len == 0;
    uint8_t pre[JLP_PREFIX_MAX];
    struct iovec parts[2] = {{.iov_base = pre, .iov_len = jlp_body_prefix(jl_ctx, n, pre)},
                             {.iov_base = (void *)data, .iov_len = n}};
    apr_status_t rv = jl_io_send(q->io, JLP_T_BODY_CHUNK, last ? JLP_F_LAST : 0, q->id, parts, 2);
    if (rv != APR_SUCCESS)
        return fail(q, q->committed ? OUT_CUT : OUT_NOANSWER, HTTP_BAD_GATEWAY, rv, "sending a body chunk");
    q->body_sent += (apr_off_t)n;
    return -1; /* go on */
}

/* fail_on_status: the entry for status (positive or negative), or 0. */
static int fail_on_status(const jl_worker_t *w, int status)
{
    if (!w->fail_on_status)
        return 0;
    const int *v = (const int *)w->fail_on_status->elts;
    for (int i = 0; i < w->fail_on_status->nelts; i++)
        if (v[i] == status || -v[i] == status)
            return v[i];
    return 0;
}

static int exchange(jl_req_t *q)
{
    request_rec *r = q->r;
    conn_rec *c = q->c;
    int done = 0;
    for (;;) {
        /* Before waiting on the worker, let the client have what it was sent. */
        if (q->unflushed && !jl_io_ready(q->io)) {
            APR_BRIGADE_INSERT_TAIL(q->bb, apr_bucket_flush_create(c->bucket_alloc));
            q->unflushed = 0;
            int st = pass(q, &done);
            if (done)
                return st;
        }
        jlp_frame_hdr h;
        const uint8_t *p;
        apr_status_t rv = q->w->reply_timeout > 0 ? jl_io_read_frame_within(q->io, &h, &p, q->w->reply_timeout)
                                                  : jl_io_read_frame(q->io, &h, &p);
        /* EOF before any answer on a reused connection: the worker went away
         * while it sat in the pool, most likely before it read the request. */
        if (rv != APR_SUCCESS && q->frames == 0 && q->may_retry && q->idempotent &&
            (APR_STATUS_IS_EOF(rv) || APR_STATUS_IS_ECONNRESET(rv)))
            return RETRY;
        if (rv == APR_SUCCESS)
            q->frames++;
        if (rv == JL_IO_EPROTO)
            return protocol_error(q, "a malformed frame");
        if (rv != APR_SUCCESS) {
            q->reply_timedout = APR_STATUS_IS_TIMEUP(rv);
            return fail(q, q->committed ? OUT_CUT : OUT_NOANSWER,
                        q->reply_timedout ? HTTP_GATEWAY_TIME_OUT : HTTP_BAD_GATEWAY, rv,
                        q->reply_timedout ? "no answer in time" : "reading the response");
        }
        if (h.type != JLP_T_SHUTDOWN && h.req_id != q->id)
            return protocol_error(q, apr_psprintf(r->pool, "frame for request %u", h.req_id));

        switch (h.type) {
        case JLP_T_GET_BODY_CHUNK: {
            uint32_t max;
            if (jlp_get_body_decode(jl_ctx, p, h.len, &max) != JLP_OK)
                return protocol_error(q, "a malformed GET_BODY_CHUNK");
            int st = send_body_chunk(q, max);
            if (st != -1)
                return st;
            break;
        }
        case JLP_T_SEND_HEADERS:
            if (q->headers_seen)
                return protocol_error(q, "a second SEND_HEADERS");
            q->headers_seen = 1;
            if (jlp_decode_send_headers(jl_ctx, p, h.len, &header_visitor, q) != JLP_OK)
                return protocol_error(q, q->bad ? q->bad : jlp_last_error());
            int fos = fail_on_status(q->w, r->status);
            if (fos)
                /* Positive: the worker is unwell, another may try. Negative:
                 * only this request fails. The worker's body is never read. */
                return fail(q, fos > 0 ? OUT_FAIL_STATUS : OUT_ANSWERED, r->status, APR_SUCCESS,
                            apr_psprintf(r->pool, "fail_on_status %d", fos));
            /* The client may still be waiting to send a body nobody read:
             * close after this response instead of reading it. */
            if (r->expecting_100)
                c->keepalive = AP_CONN_CLOSE;
            break;
        case JLP_T_BODY_CHUNK: {
            const uint8_t *data;
            size_t len;
            if (!q->headers_seen)
                return protocol_error(q, "BODY_CHUNK before SEND_HEADERS");
            if (jlp_body_view(jl_ctx, p, h.len, &data, &len) != JLP_OK)
                return protocol_error(q, "a malformed BODY_CHUNK");
            if (r->header_only)
                break;
            q->sent_bytes += len;
            if (q->declared_cl >= 0 && q->sent_bytes > q->declared_cl)
                return protocol_error(q, "more body than its Content-Length");
            if (len)
                APR_BRIGADE_INSERT_TAIL(q->bb, apr_bucket_transient_create((const char *)data, len, c->bucket_alloc));
            if (h.flags & JLP_F_FLUSH)
                APR_BRIGADE_INSERT_TAIL(q->bb, apr_bucket_flush_create(c->bucket_alloc));
            q->unflushed = !(h.flags & JLP_F_FLUSH);
            /* Transient buckets point into the read buffer: they are passed
             * (or set aside, which copies them) before the next read. */
            int st = pass(q, &done);
            if (done)
                return st;
            break;
        }
        case JLP_T_END_RESPONSE: {
            int reuse;
            if (!q->headers_seen)
                return protocol_error(q, "END_RESPONSE before SEND_HEADERS");
            if (jlp_decode_end_response(jl_ctx, p, h.len, &reuse, &trailer_visitor, q) != JLP_OK)
                return protocol_error(q, q->bad ? q->bad : "a malformed END_RESPONSE");
            if (!r->header_only && q->declared_cl >= 0 && r->status != HTTP_NO_CONTENT &&
                r->status != HTTP_NOT_MODIFIED && q->sent_bytes != q->declared_cl)
                return protocol_error(
                    q, apr_psprintf(r->pool, "%" APR_OFF_T_FMT " body bytes for Content-Length %" APR_OFF_T_FMT,
                                    q->sent_bytes, q->declared_cl));
            /* Anything after END_RESPONSE is out of step: do not reuse. The
             * transient buckets pointing into the read buffer have all been
             * passed (set aside, so copied, if held), so another request may
             * have the buffer now. */
            release_worker(q, reuse && !q->saw_shutdown && q->io->end == q->io->start);
            /* The worker did not read all of the body: close rather than have
             * httpd read the rest only to throw it away. */
            if (q->has_body && !q->body_eos)
                c->keepalive = AP_CONN_CLOSE;
            APR_BRIGADE_INSERT_TAIL(q->bb, apr_bucket_eos_create(c->bucket_alloc));
            int st = pass(q, &done);
            return st;
        }
        case JLP_T_ERROR: {
            unsigned code;
            char msg[256];
            if (jlp_decode_error(jl_ctx, p, h.len, &code, msg, sizeof msg) != JLP_OK)
                return protocol_error(q, "a malformed ERROR");
            int status = code >= 400 && code <= 599 ? (int)code : HTTP_BAD_GATEWAY;
            return fail(q, OUT_ANSWERED, status, APR_SUCCESS, apr_psprintf(r->pool, "ERROR %u: %s", code, msg));
        }
        case JLP_T_SHUTDOWN:
            q->saw_shutdown = 1; /* the worker is draining; this request is still served */
            break;
        default:
            return protocol_error(q, apr_psprintf(r->pool, "unexpected %s frame", jlp_type_name(h.type)));
        }
    }
}

/* Encodes and sends FORWARD_REQUEST, with the inline body block if it fits.
 * Returns OK, RETRY, or the handler's result. */
static int send_forward(jl_req_t *q, jlp_forward_request *fr, size_t inline_len)
{
    request_rec *r = q->r;
    fr->body = q->bodybuf;
    fr->body_len = inline_len;
    q->pending = NULL;
    q->pending_len = 0;
    jlp_buf eb;
    if (jlp_encode_forward(jl_ctx, fr, &eb) != JLP_OK)
        return fail(q, OUT_LOCAL, HTTP_INTERNAL_SERVER_ERROR, APR_SUCCESS,
                    apr_psprintf(r->pool, "encoding the request: %s", jlp_last_error()));
    if (eb.len > q->io->max_frame && inline_len) {
        /* Too big with the body block: send the head alone, the block on demand. */
        jlp_buf_free(&eb);
        q->pending = q->bodybuf;
        q->pending_len = inline_len;
        fr->body_len = 0;
        if (jlp_encode_forward(jl_ctx, fr, &eb) != JLP_OK)
            return fail(q, OUT_LOCAL, HTTP_INTERNAL_SERVER_ERROR, APR_SUCCESS,
                        apr_psprintf(r->pool, "encoding the request: %s", jlp_last_error()));
    }
    if (eb.len > q->io->max_frame) {
        jlp_buf_free(&eb);
        return fail(q, OUT_LOCAL, HTTP_INTERNAL_SERVER_ERROR, APR_SUCCESS,
                    "the request head is larger than max_packet_size");
    }
    int last = q->body_eos && q->pending_len == 0;
    struct iovec part = {.iov_base = eb.data, .iov_len = eb.len};
    apr_status_t rv = jl_io_send(q->io, JLP_T_FORWARD_REQUEST, last ? JLP_F_LAST : 0, q->id, &part, 1);
    jlp_buf_free(&eb);
    /* A pooled connection the worker had closed: it never saw the request. */
    if (rv != APR_SUCCESS && q->may_retry)
        return RETRY;
    if (rv != APR_SUCCESS)
        return fail(q, OUT_UNDELIVERED, HTTP_BAD_GATEWAY, rv, "sending the request");
    q->body_sent += (apr_off_t)fr->body_len;
    return OK;
}

/* One attempt on q->m's worker: a connection, the request, the answer. A
 * pooled connection found dead before the worker saw the request is
 * replaced once, on the same worker; that stays inside the attempt. */
static int attempt(jl_req_t *q, jlp_forward_request *fr, size_t inline_len)
{
    request_rec *r = q->r;
    const jl_worker_t *w = q->w;
    for (int again = 0;; again++) {
        const char *why = "connect";
        apr_status_t rv = jl_conn_acquire(w, r, again ? JL_ACQUIRE_FRESH : 0, &q->conn, &why);
        if (rv != APR_SUCCESS) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, rv == JL_EPOOL ? 0 : rv, r, "jl: worker %s (%s): %s", w->name,
                          w->target, why);
            q->outcome = rv == JL_EPOOL ? OUT_POOL : OUT_UNDELIVERED;
            return HTTP_SERVICE_UNAVAILABLE;
        }
        q->io = &q->conn->io;
        q->id = jl_conn_next_id(q->conn);
        q->may_retry = again == 0 && q->conn->reused;
        q->outcome = OUT_ANSWERED;
        int st = send_forward(q, fr, inline_len);
        if (st == OK)
            st = exchange(q);
        if (st != RETRY)
            return st;
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                      "jl: worker %s (%s): a pooled connection was dead; trying again on a new one", w->name,
                      w->target);
        close_worker(q);
    }
}

/* What the attempt says about the worker, and whether another member may
 * take the request. */
static int judge(jl_req_t *q)
{
    request_rec *r = q->r;
    jl_member_t *m = q->m;
    int replayable = !q->body_streamed;
    int recover = q->w->recovery == JL_RECOVER_ALWAYS ||
                  (q->w->recovery == JL_RECOVER_IDEMPOTENT && q->idempotent);
    switch (q->outcome) {
    case OUT_ANSWERED:
        jl_lb_ok(m, r);
        return 0;
    case OUT_CLIENT:
    case OUT_LOCAL:
        if (q->frames > 0) /* it answered before things went wrong here */
            jl_lb_ok(m, r);
        return 0;
    case OUT_POOL:
        return 1;
    case OUT_UNDELIVERED:
        jl_lb_hard_error(m, r, "the request did not reach it");
        return 1;
    case OUT_NOANSWER:
        if (q->reply_timedout)
            jl_lb_reply_timeout(m, r);
        else
            jl_lb_soft_error(m, r, "no answer");
        return replayable && recover;
    case OUT_FAIL_STATUS:
        jl_lb_hard_error(m, r, apr_psprintf(r->pool, "answered %d (fail_on_status)", r->status));
        return replayable;
    case OUT_BROKEN:
        jl_lb_hard_error(m, r, "protocol error");
        return 0;
    default: /* OUT_CUT */
        jl_lb_soft_error(m, r, "failed during the response");
        return 0;
    }
}

/* Back to how the request was before the attempt, for the next member. */
static void reset_attempt(jl_req_t *q)
{
    q->frames = 0;
    q->saw_shutdown = 0;
    q->reply_timedout = 0;
    q->headers_seen = 0;
    q->unflushed = 0;
    q->declared_cl = -1;
    q->sent_bytes = 0;
    q->body_sent = 0;
    q->bad = NULL;
}

int jl_handler(request_rec *r)
{
    if (!r->handler || strncmp(r->handler, "jl:", 3) != 0)
        return DECLINED;
    jl_balancer_t *lb = ap_get_module_config(r->request_config, &jl_module);
    if (!lb)
        lb = jl_find_target(r->server, r->handler + 3); /* SetHandler jl:NAME */
    if (!lb) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "jl: no JlWorker or JlBalancer for handler %s", r->handler);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    jl_req_t *q = apr_pcalloc(r->pool, sizeof *q);
    q->r = r;
    q->c = r->connection;
    q->lb = lb;
    q->declared_cl = -1;
    q->bb = apr_brigade_create(r->pool, q->c->bucket_alloc);
    q->ib = apr_brigade_create(r->pool, q->c->bucket_alloc);

    jlp_forward_request fr;
    int st = fill_forward(q, &fr);
    if (st != OK)
        return st;

    /* The first block of the body goes inline, unless the client asked to
     * hear first (Expect: 100-continue): then nothing is read until the worker
     * asks, so a worker that answers without reading never sends 100. It is
     * read before a connection is taken, so a slow client holds none while it
     * sends, and it can be sent again to another worker. Every member's
     * max_packet_size bounds it; the smallest one is used. */
    size_t inline_len = 0;
    if (q->has_body) {
        uint32_t frame = JLP_MAX_FRAME_MAX;
        const jl_member_t *ms = (const jl_member_t *)lb->members->elts;
        for (int i = 0; i < lb->members->nelts; i++)
            if (ms[i].w->max_frame < frame)
                frame = ms[i].w->max_frame;
        uint32_t biggest = 0;
        for (int i = 0; i < lb->members->nelts; i++)
            if (ms[i].w->max_frame > biggest)
                biggest = ms[i].w->max_frame;
        q->bodybuf = apr_palloc(r->pool, biggest);
        if (!r->expecting_100) {
            apr_status_t rv = read_body(q, frame / 2, &inline_len);
            if (rv != APR_SUCCESS) {
                q->c->keepalive = AP_CONN_CLOSE;
                return ap_map_http_request_error(rv, HTTP_BAD_REQUEST);
            }
        }
    }
    switch (r->method_number) {
    case M_GET: /* and HEAD */
    case M_PUT:
    case M_DELETE:
    case M_OPTIONS:
    case M_TRACE:
        q->idempotent = 1;
        break;
    default:
        break;
    }
    apr_pool_cleanup_register(r->pool, q, release_cleanup, apr_pool_cleanup_null);

    unsigned char *tried = apr_pcalloc(r->pool, (apr_size_t)lb->members->nelts);
    st = HTTP_SERVICE_UNAVAILABLE;
    for (int n = 0; n < lb->retries; n++) {
        jl_choice_t ch;
        const char *why;
        int est = jl_lb_elect(lb, r, tried, &ch, &why);
        if (est != OK) {
            if (n == 0) {
                ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "jl: %s: %s", lb->name, why);
                return est;
            }
            break; /* nobody left to try: the last attempt's answer stands */
        }
        q->m = ch.m;
        q->probe = ch.probe;
        q->w = ch.m->w;
        tried[ch.m - (jl_member_t *)lb->members->elts] = 1;
        apr_atomic_inc32(&q->w->shm->busy);
        q->busy = 1;
        fr.route = q->w->route;

        st = attempt(q, &fr, inline_len);
        int again = judge(q);
        q->probe = 0; /* judged */
        if (q->outcome == OUT_ANSWERED)
            jl_lb_account(q->m, q->sent_bytes + q->body_sent);
        leave_member(q);
        if (!again || q->committed || n + 1 >= lb->retries)
            return st;
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "jl: %s: trying another worker after %s", lb->name,
                      q->w->name);
        apr_atomic_inc64(&q->w->shm->failovers);
        close_worker(q);
        reset_attempt(q);
    }
    return st;
}
