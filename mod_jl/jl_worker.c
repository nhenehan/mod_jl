/* Connections to a worker: a pool per worker per child process, connect and
 * HELLO, the stale check and CPing.
 *
 * The pool is a fixed set of slots and a stack of the idle ones. The most
 * recently released connection is taken first, and slots without one wait
 * at the bottom, so under light load a few connections stay warm and the rest
 * age out. Slots connect lazily, on the
 * first request that takes them: a worker that is down costs nothing until it
 * is used, and child_init never blocks on one. */
#include "mod_jl.h"

#include "apr_strings.h"
#include "ap_mpm.h"
#include "http_log.h"

#include <string.h>

APLOG_USE_MODULE(jl);

#define JL_AGENT "mod_jl/0.3"

struct jl_pool {
    const jl_worker_t *w;
    server_rec *s;
    apr_thread_mutex_t *lock;
    apr_thread_cond_t *freed;
    jl_conn_t *slots;
    jl_conn_t **idle; /* idle[n_idle - 1] was released last */
    int size, min, n_idle;
};

/* ===== Connecting ===== */

static apr_status_t open_socket(jl_conn_t *c, apr_socket_t **out, const char **why)
{
    const jl_worker_t *w = c->w;
    apr_status_t rv = APR_EGENERAL;
    for (apr_sockaddr_t *sa = w->addr; sa; sa = sa->next) {
        apr_socket_t *s;
        /* AF_UNIX takes protocol 0: APR_PROTO_TCP would be refused. */
        int proto = w->path ? 0 : APR_PROTO_TCP;
        if ((rv = apr_socket_create(&s, sa->family, SOCK_STREAM, proto, c->spool)) != APR_SUCCESS) {
            *why = "socket";
            continue;
        }
        if (!w->path) {
            apr_socket_opt_set(s, APR_TCP_NODELAY, 1);
            if (w->keepalive)
                apr_socket_opt_set(s, APR_SO_KEEPALIVE, 1);
        }
        apr_socket_timeout_set(s, w->connect_timeout);
        if ((rv = apr_socket_connect(s, sa)) == APR_SUCCESS) {
            *out = s;
            return APR_SUCCESS;
        }
        *why = "connect";
        apr_socket_close(s);
    }
    return rv;
}

static void close_conn(jl_conn_t *c)
{
    if (!c->sock)
        return;
    apr_socket_close(c->sock);
    apr_pool_clear(c->spool);
    c->sock = NULL;
    c->io.sock = NULL;
}

static apr_status_t hello(jl_conn_t *c, const char **why)
{
    const jl_worker_t *w = c->w;
    jlp_hello hello = {
        .minor = JLP_PROTO_MINOR,
        .features = JLP_FEAT_BYTE_ALIGNED | (w->secret ? JLP_FEAT_SECRET : 0),
        .max_frame = w->max_frame,
        .secret = w->secret,
        .secret_len = w->secret ? (uint16_t)strlen(w->secret) : 0,
        .agent = JL_AGENT,
        .agent_len = (uint16_t)strlen(JL_AGENT),
    };
    memcpy(hello.schema_sha256, jlp_schema_sha256(), sizeof hello.schema_sha256);
    uint8_t buf[512];
    size_t n = jlp_hello_pack(buf, sizeof buf, &hello);
    if (n == 0) {
        *why = "HELLO does not fit (secret too long?)";
        return APR_EINVAL;
    }
    struct iovec part = {.iov_base = buf, .iov_len = n};
    apr_status_t rv = jl_io_send(&c->io, JLP_T_HELLO, 0, 0, &part, 1);
    if (rv != APR_SUCCESS) {
        *why = "sending HELLO";
        return rv;
    }

    /* HELLO_ACK is a round trip already: under ping_mode C it is the CPing. */
    jlp_frame_hdr h;
    const uint8_t *p;
    rv = jl_io_read_frame_within(&c->io, &h, &p, w->ping_mode & JL_PING_CONNECT ? w->ping_timeout : c->io.timeout);
    if (rv != APR_SUCCESS) {
        *why = APR_STATUS_IS_TIMEUP(rv) && (w->ping_mode & JL_PING_CONNECT) ? "no HELLO_ACK within ping_timeout"
                                                                            : "reading HELLO_ACK";
        return rv;
    }
    jlp_hello_ack ack;
    if (h.type != JLP_T_HELLO_ACK || jlp_hello_ack_unpack(p, h.len, &ack) != JLP_OK) {
        *why = "a malformed HELLO_ACK";
        return JL_IO_EPROTO;
    }
    if (ack.status != JLP_HELLO_OK) {
        *why = ack.status == JLP_HELLO_BAD_SECRET        ? "HELLO refused: wrong secret"
               : ack.status == JLP_HELLO_SCHEMA_MISMATCH ? "HELLO refused: the worker was built from another schema"
                                                         : "HELLO refused: unsupported features";
        return APR_EACCES;
    }
    /* The worker only ever lowers the frame size we asked for. */
    if (ack.max_frame < JLP_MAX_FRAME_MIN || ack.max_frame > w->max_frame) {
        *why = "HELLO_ACK with an out-of-range max_frame";
        return JL_IO_EPROTO;
    }
    c->io.max_frame = ack.max_frame;
    c->worker_pid = ack.pid;
    return APR_SUCCESS;
}

static apr_status_t connect_conn(jl_conn_t *c, server_rec *srv, const char **why)
{
    const jl_worker_t *w = c->w;
    apr_status_t rv;
    if (!c->pool) {
        if ((rv = apr_pool_create_unmanaged_ex(&c->pool, NULL, NULL)) != APR_SUCCESS ||
            (rv = apr_pool_create(&c->spool, c->pool)) != APR_SUCCESS) {
            *why = "allocating a connection";
            return rv;
        }
        apr_pool_tag(c->pool, "jl_conn");
    }
    apr_socket_t *s;
    if ((rv = open_socket(c, &s, why)) != APR_SUCCESS) {
        apr_pool_clear(c->spool);
        return rv;
    }
    apr_interval_time_t timeout = w->socket_timeout > 0 ? w->socket_timeout : srv->timeout;
    apr_socket_timeout_set(s, timeout);
    c->sock = s;
    jl_io_init(&c->io, s, c->pool, w->max_frame, timeout);
    if ((rv = hello(c, why)) != APR_SUCCESS) {
        close_conn(c);
        return rv;
    }
    c->reused = 0;
    c->last_used = apr_time_now();
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, srv, "jl: %s: connected to %s (worker pid %u)", w->name, w->target,
                  c->worker_pid);
    return APR_SUCCESS;
}

/* ===== Checks before use ===== */

/* An idle connection has nothing to read. If it does, say why it is no good:
 * usually a SHUTDOWN from a draining worker, or EOF from one that went away. */
static const char *stale(jl_conn_t *c)
{
    if (!jl_io_ready(&c->io))
        return NULL;
    jlp_frame_hdr h;
    if (c->io.end - c->io.start >= JLP_FRAME_HDR &&
        jlp_hdr_unpack(c->io.buf + c->io.start, c->io.max_frame, &h) == JLP_OK && h.type == JLP_T_SHUTDOWN)
        return "the worker is shutting down";
    return c->io.end > c->io.start ? "unexpected data from the worker" : "closed by the worker";
}

/* CPING, then CPONG with the same id within ping_timeout. Returns
 * APR_ECONNRESET when the worker answered SHUTDOWN instead: it is draining,
 * and a new connection may reach its successor. */
static apr_status_t cping(jl_conn_t *c, server_rec *srv, apr_pool_t *tmp, const char **why)
{
    const jl_worker_t *w = c->w;
    const uint8_t *doc;
    size_t n;
    if (jlp_const_doc(jl_ctx, JLP_T_CPING, &doc, &n) != JLP_OK) {
        *why = "no CPING document";
        return APR_EGENERAL;
    }
    uint32_t id = jl_conn_next_id(c);
    struct iovec part = {.iov_base = (void *)doc, .iov_len = n};
    apr_time_t t0 = apr_time_now();
    apr_status_t rv = jl_io_send(&c->io, JLP_T_CPING, 0, id, &part, 1);
    if (rv != APR_SUCCESS) {
        *why = "sending CPING";
        return rv;
    }
    jlp_frame_hdr h;
    const uint8_t *p;
    if ((rv = jl_io_read_frame_within(&c->io, &h, &p, w->ping_timeout)) != APR_SUCCESS) {
        *why = APR_STATUS_IS_TIMEUP(rv) ? "no CPONG within ping_timeout" : "reading CPONG";
        return rv;
    }
    if (h.type == JLP_T_SHUTDOWN) {
        *why = "the worker is shutting down";
        return APR_ECONNRESET;
    }
    if (h.type != JLP_T_CPONG || h.req_id != id) {
        *why = apr_psprintf(tmp, "%s %u in answer to CPING %u", jlp_type_name(h.type), h.req_id, id);
        return JL_IO_EPROTO;
    }
    ap_log_error(APLOG_MARK, APLOG_TRACE1, 0, srv, "jl: %s: CPONG in %" APR_TIME_T_FMT " us", w->name,
                  apr_time_now() - t0);
    return APR_SUCCESS;
}

/* ===== The pool ===== */

uint32_t jl_conn_next_id(jl_conn_t *c)
{
    if (++c->next_req_id == 0)
        c->next_req_id = 1;
    return c->next_req_id;
}

static apr_status_t take(jl_pool_t *pl, jl_conn_t **out)
{
    apr_interval_time_t limit = pl->w->acquire_timeout;
    apr_time_t deadline = apr_time_now() + limit;
    apr_status_t rv = APR_SUCCESS;
    apr_thread_mutex_lock(pl->lock);
    while (pl->n_idle == 0) {
        if (limit == 0) {
            apr_thread_cond_wait(pl->freed, pl->lock);
            continue;
        }
        apr_interval_time_t left = deadline - apr_time_now();
        if (left <= 0) {
            rv = APR_TIMEUP;
            break;
        }
        apr_thread_cond_timedwait(pl->freed, pl->lock, left);
    }
    if (rv == APR_SUCCESS)
        *out = pl->idle[--pl->n_idle];
    apr_thread_mutex_unlock(pl->lock);
    return rv;
}

/* Called with the lock held: close idle connections unused for longer than
 * connection_pool_timeout, oldest first, while more than connection_pool_min
 * stay open. Slots in use count as open. */
static void sweep(jl_pool_t *pl, apr_time_t now)
{
    apr_interval_time_t ttl = pl->w->pool_timeout;
    if (ttl == 0)
        return;
    int open = pl->size - pl->n_idle;
    for (int i = 0; i < pl->n_idle; i++)
        open += pl->idle[i]->sock != NULL;
    int closed = 0;
    for (int i = 0; i < pl->n_idle && open > pl->min; i++) {
        jl_conn_t *c = pl->idle[i];
        if (c->sock && now - c->last_used > ttl) {
            close_conn(c);
            open--;
            closed++;
        }
    }
    if (closed)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, pl->s, "jl: %s: closed %d idle connection%s, %d open", pl->w->name,
                     closed, closed == 1 ? "" : "s", open);
}

static void put(jl_conn_t *c)
{
    jl_pool_t *pl = c->owner;
    apr_thread_mutex_lock(pl->lock);
    if (c->sock) {
        pl->idle[pl->n_idle++] = c;
    } else { /* at the bottom, so the open connections are taken first */
        memmove(pl->idle + 1, pl->idle, sizeof *pl->idle * pl->n_idle);
        pl->idle[0] = c;
        pl->n_idle++;
    }
    sweep(pl, apr_time_now());
    apr_thread_cond_signal(pl->freed);
    apr_thread_mutex_unlock(pl->lock);
}

apr_status_t jl_conn_acquire(const jl_worker_t *w, request_rec *r, int flags, jl_conn_t **out, const char **why)
{
    jl_conn_t *c;
    apr_status_t rv = take(w->pool, &c);
    if (rv != APR_SUCCESS) {
        *why = "no free connection within connection_acquire_timeout";
        return JL_EPOOL;
    }
    const char *bad = NULL;
    if (c->sock && (flags & JL_ACQUIRE_FRESH))
        bad = "a new connection was asked for";
    else if (c->sock)
        bad = stale(c);
    if (bad) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "jl: %s: dropping a pooled connection: %s", w->name, bad);
        close_conn(c);
    }

    if (c->sock) {
        apr_time_t now = apr_time_now();
        if ((w->ping_mode & JL_PING_PREPOST) ||
            ((w->ping_mode & JL_PING_INTERVAL) && now - c->last_used > w->ping_interval)) {
            rv = cping(c, r->server, r->pool, why);
            if (rv == APR_ECONNRESET) { /* draining: try a new connection */
                ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "jl: %s: dropping a pooled connection: %s", w->name, *why);
                close_conn(c);
            } else if (rv != APR_SUCCESS) {
                /* A late CPONG would be read as the answer to the request. */
                close_conn(c);
                put(c);
                return rv;
            }
        }
    }
    if (c->sock) {
        c->reused = 1;
    } else {
        /* Nobody listening (a worker restarting with a gap): try again a
         * few times. Anything else is not going to change that soon. */
        for (int attempt = 1;; attempt++) {
            rv = connect_conn(c, r->server, why);
            if (rv == APR_SUCCESS || attempt >= w->retries ||
                !(APR_STATUS_IS_ECONNREFUSED(rv) || APR_STATUS_IS_ENOENT(rv)))
                break;
            apr_sleep(w->retry_interval);
        }
        if (rv != APR_SUCCESS) {
            put(c);
            return rv;
        }
    }
    *out = c;
    return APR_SUCCESS;
}

void jl_conn_release(jl_conn_t *c, int keep)
{
    if (keep)
        c->last_used = apr_time_now();
    else
        close_conn(c);
    put(c);
}

static apr_status_t destroy_pool(void *data)
{
    jl_pool_t *pl = data;
    for (int i = 0; i < pl->size; i++) {
        jl_conn_t *c = &pl->slots[i];
        if (c->pool) {
            close_conn(c);
            apr_pool_destroy(c->pool);
            c->pool = NULL;
        }
    }
    return APR_SUCCESS;
}

static apr_status_t make_pool(jl_worker_t *w, apr_pool_t *pchild, server_rec *s)
{
    int threads = 0;
    if (ap_mpm_query(AP_MPMQ_MAX_THREADS, &threads) != APR_SUCCESS || threads < 1)
        threads = 1;
    jl_pool_t *pl = apr_pcalloc(pchild, sizeof *pl);
    pl->w = w;
    pl->s = s;
    pl->size = w->pool_size > 0 ? w->pool_size : threads;
    pl->min = w->pool_min;
    if (pl->min > pl->size) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, s,
                     "JlWorker %s: connection_pool_min %d is more than the pool's %d connections", w->name, pl->min,
                     pl->size);
        pl->min = pl->size;
    }
    apr_status_t rv;
    if ((rv = apr_thread_mutex_create(&pl->lock, APR_THREAD_MUTEX_DEFAULT, pchild)) != APR_SUCCESS ||
        (rv = apr_thread_cond_create(&pl->freed, pchild)) != APR_SUCCESS)
        return rv;
    pl->slots = apr_pcalloc(pchild, sizeof *pl->slots * pl->size);
    pl->idle = apr_palloc(pchild, sizeof *pl->idle * pl->size);
    for (int i = 0; i < pl->size; i++) {
        pl->slots[i].owner = pl;
        pl->slots[i].w = w;
        pl->idle[i] = &pl->slots[pl->size - 1 - i]; /* slot 0 on top */
    }
    pl->n_idle = pl->size;
    w->pool = pl;
    apr_pool_cleanup_register(pchild, pl, destroy_pool, apr_pool_cleanup_null);
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
                 "JlWorker %s: pool of %d (min %d), ping_mode %s%s%s, ping_timeout %" APR_TIME_T_FMT " ms", w->name,
                 pl->size, pl->min, w->ping_mode & JL_PING_CONNECT ? "C" : "",
                 w->ping_mode & JL_PING_PREPOST ? "P" : "", w->ping_mode & JL_PING_INTERVAL ? "I" : "",
                 apr_time_as_msec(w->ping_timeout));
    return APR_SUCCESS;
}

apr_status_t jl_pools_init(apr_pool_t *pchild, server_rec *s)
{
    jl_server_conf *conf = ap_get_module_config(s->module_config, &jl_module);
    for (apr_hash_index_t *hi = apr_hash_first(pchild, conf->workers); hi; hi = apr_hash_next(hi)) {
        jl_worker_t *w = apr_hash_this_val(hi);
        apr_status_t rv = make_pool(w, pchild, s);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s, "JlWorker %s: cannot make its connection pool", w->name);
            return rv;
        }
    }
    return APR_SUCCESS;
}

void jl_pool_stats(const jl_worker_t *w, int *size, int *idle, int *open)
{
    jl_pool_t *pl = w->pool;
    *size = *idle = *open = 0;
    if (!pl)
        return;
    apr_thread_mutex_lock(pl->lock);
    *size = pl->size;
    *idle = pl->n_idle;
    for (int i = 0; i < pl->size; i++)
        *open += pl->slots[i].sock != NULL; /* a slot in use may be changing: near enough */
    apr_thread_mutex_unlock(pl->lock);
}

apr_status_t jl_worker_check(const jl_worker_t *w, server_rec *s, apr_pool_t *p, const char **why)
{
    jl_conn_t c;
    memset(&c, 0, sizeof c);
    c.w = w;
    apr_status_t rv = connect_conn(&c, s, why);
    if (rv == APR_SUCCESS)
        rv = cping(&c, s, p, why);
    close_conn(&c);
    if (c.pool)
        apr_pool_destroy(c.pool);
    return rv;
}
