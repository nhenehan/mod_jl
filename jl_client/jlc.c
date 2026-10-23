/* jlc: a jlp v2 client that plays mod_jl's role over one socket, for testing
 * and benchmarking JlServ without Apache.
 *
 *   jlc [conn] [-X METHOD] [-H 'Name: value']... [--data-file F | --data S]
 *       [--chunked] [-i] [-n COUNT] PATH[?QUERY]
 *   jlc [conn] --ping
 *   jlc [conn] bench [-c CONNS] [-d SECONDS] [-X METHOD] [--data S] PATH
 *
 *   conn: --unix PATH | --tcp HOST:PORT, [--secret S] [--max-frame N]
 *
 * The response body goes to stdout; -i also prints the status line and
 * headers to stderr. -n repeats the request on the same connection. */

#define _GNU_SOURCE
#include "jlp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_HEADERS 64

typedef struct {
    const char *unix_path;
    const char *tcp;
    const char *secret;
    uint32_t max_frame;
} conn_opts;

typedef struct {
    const char *method;
    const char *path;
    jlp_header headers[MAX_HEADERS];
    size_t n_headers;
    const char *data;
    const char *data_file;
    int chunked;
    int show_headers;
    long count;
} req_opts;

typedef struct {
    int fd;
    const jlp_ctx *ctx;
    uint32_t max_frame;
    uint32_t req_id;
    uint8_t *rbuf; /* JLP_FRAME_HDR + max_frame */
} conn;

static jlp_ctx *g_ctx;

static void die(const char *what)
{
    fprintf(stderr, "jlc: %s: %s\n", what, errno ? strerror(errno) : jlp_last_error());
    exit(1);
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q && n) {
        errno = ENOMEM;
        die("realloc");
    }
    return q;
}

static void *xmalloc(size_t n) { return xrealloc(NULL, n); }

static int read_exact(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t k = read(fd, p, n);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
        } else if (k == 0) {
            errno = ECONNRESET;
            return -1;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static int writev_all(int fd, struct iovec *iov, int cnt)
{
    while (cnt) {
        ssize_t k = writev(fd, iov, cnt);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        while (cnt && (size_t)k >= iov->iov_len) {
            k -= (ssize_t)iov->iov_len;
            iov++;
            cnt--;
        }
        if (cnt) {
            iov->iov_base = (uint8_t *)iov->iov_base + k;
            iov->iov_len -= (size_t)k;
        }
    }
    return 0;
}

/* Sends header + up to two payload pieces in one writev. */
static int send_frame(conn *c, uint8_t type, uint8_t flags, const void *a, size_t an, const void *b, size_t bn)
{
    uint8_t hdr[JLP_FRAME_HDR];
    jlp_frame_hdr h = {.type = type, .flags = flags, .req_id = c->req_id, .len = (uint32_t)(an + bn)};
    jlp_hdr_pack(hdr, &h);
    struct iovec iov[3] = {{hdr, sizeof hdr}, {(void *)a, an}, {(void *)b, bn}};
    return writev_all(c->fd, iov, bn ? 3 : an ? 2 : 1);
}

static int recv_frame(conn *c, jlp_frame_hdr *h)
{
    if (read_exact(c->fd, c->rbuf, JLP_FRAME_HDR))
        return -1;
    if (jlp_hdr_unpack(c->rbuf, c->max_frame, h) != JLP_OK)
        return -1;
    return h->len ? read_exact(c->fd, c->rbuf + JLP_FRAME_HDR, h->len) : 0;
}

static int connect_to(const conn_opts *o)
{
    int fd;
    if (o->unix_path) {
        struct sockaddr_un sa = {.sun_family = AF_UNIX};
        snprintf(sa.sun_path, sizeof sa.sun_path, "%s", o->unix_path);
        if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0 ||
            connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0)
            return -1;
        return fd;
    }
    char host[256];
    snprintf(host, sizeof host, "%s", o->tcp);
    char *colon = strrchr(host, ':');
    if (!colon) {
        errno = EINVAL;
        return -1;
    }
    *colon = '\0';
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *ai;
    if (getaddrinfo(host, colon + 1, &hints, &ai) != 0) {
        errno = EHOSTUNREACH;
        return -1;
    }
    fd = socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        freeaddrinfo(ai);
        return -1;
    }
    freeaddrinfo(ai);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static void close_conn(conn *c)
{
    if (c->fd >= 0)
        close(c->fd);
    free(c->rbuf);
    c->fd = -1;
    c->rbuf = NULL;
}

/* On failure the connection is closed again and errno says why. */
static int open_conn(conn *c, const conn_opts *o)
{
    memset(c, 0, sizeof *c);
    c->ctx = g_ctx;
    c->max_frame = JLP_MAX_FRAME_MAX; /* until HELLO_ACK says otherwise */
    c->fd = -1;
    c->rbuf = xmalloc(JLP_FRAME_HDR + JLP_MAX_FRAME_MAX);
    if ((c->fd = connect_to(o)) < 0)
        goto fail;
    jlp_hello hello = {.minor = JLP_PROTO_MINOR,
                       .features = JLP_FEAT_BYTE_ALIGNED | (o->secret ? JLP_FEAT_SECRET : 0),
                       .max_frame = o->max_frame,
                       .secret = o->secret,
                       .secret_len = o->secret ? (uint16_t)strlen(o->secret) : 0,
                       .agent = "jlc/0.1",
                       .agent_len = 7};
    memcpy(hello.schema_sha256, jlp_schema_sha256(), 32);
    uint8_t buf[512];
    size_t n = jlp_hello_pack(buf, sizeof buf, &hello);
    jlp_frame_hdr h;
    if (send_frame(c, JLP_T_HELLO, 0, buf, n, NULL, 0) || recv_frame(c, &h))
        goto fail;
    jlp_hello_ack ack;
    if (h.type != JLP_T_HELLO_ACK || jlp_hello_ack_unpack(c->rbuf + JLP_FRAME_HDR, h.len, &ack) != JLP_OK) {
        errno = EPROTO;
        goto fail;
    }
    if (ack.status != JLP_HELLO_OK) {
        fprintf(stderr, "jlc: HELLO refused, status %u\n", ack.status);
        errno = EACCES;
        goto fail;
    }
    c->max_frame = ack.max_frame;
    return 0;
fail: {
    int e = errno;
    close_conn(c);
    errno = e;
    return -1;
}
}


/* ===== One request ===== */

typedef struct {
    int show_headers;
    FILE *out;
    unsigned status;
    uint64_t body_bytes;
} resp_state;

static int on_status(void *ud, unsigned status, const char *reason, size_t len)
{
    resp_state *r = ud;
    r->status = status;
    if (r->show_headers)
        fprintf(stderr, "< %u %.*s\n", status, (int)len, reason ? reason : "");
    return 0;
}

static int on_header(void *ud, int ord, const char *name, size_t nlen, const char *value, size_t vlen, int raw)
{
    resp_state *r = ud;
    if (r->show_headers)
        fprintf(stderr, "< %.*s: %.*s\n", (int)nlen, name, (int)vlen, value);
    return 0;
}

typedef struct {
    int fd;          /* body source, or -1 */
    const char *mem; /* or in-memory body */
    size_t mem_len, mem_off;
    int eof;
} body_src;

static size_t body_read(body_src *b, uint8_t *dst, size_t n)
{
    if (b->eof)
        return 0;
    if (b->mem) {
        size_t k = b->mem_len - b->mem_off < n ? b->mem_len - b->mem_off : n;
        memcpy(dst, b->mem + b->mem_off, k);
        b->mem_off += k;
        if (b->mem_off == b->mem_len)
            b->eof = 1;
        return k;
    }
    size_t got = 0;
    while (got < n) {
        ssize_t k = read(b->fd, dst + got, n - got);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0) {
            b->eof = 1;
            break;
        }
        got += (size_t)k;
    }
    return got;
}

/* Returns 0 and sets *reuse on a complete response; -1 on failure. */
static int do_request(conn *c, const req_opts *ro, const jlp_forward_request *base, body_src *body, resp_state *rs,
                      int *reuse)
{
    jlp_forward_request fr = *base;
    c->req_id++;

    /* Inline first chunk: half a frame, so the head always fits beside it. */
    uint8_t *inline_buf = NULL;
    size_t inline_len = 0;
    int last = 0;
    if (fr.body_length != JLP_NO_BODY) {
        inline_buf = xmalloc(c->max_frame / 2);
        inline_len = body_read(body, inline_buf, c->max_frame / 2);
        last = body->eof;
        fr.body = inline_buf;
        fr.body_len = inline_len;
    }
    jlp_buf eb;
    if (jlp_encode_forward(c->ctx, &fr, &eb) != JLP_OK) {
        free(inline_buf);
        return -1;
    }
    int rc = eb.len > c->max_frame
                 ? -1
                 : send_frame(c, JLP_T_FORWARD_REQUEST, last ? JLP_F_LAST : 0, eb.data, eb.len, NULL, 0);
    jlp_buf_free(&eb);
    free(inline_buf);
    if (rc)
        return -1;

    jlp_headers_visitor v = {.status = on_status, .header = on_header};
    uint8_t *chunk = NULL;
    for (;;) {
        jlp_frame_hdr h;
        if (recv_frame(c, &h))
            goto fail;
        const uint8_t *p = c->rbuf + JLP_FRAME_HDR;
        if (h.req_id != c->req_id && h.type != JLP_T_SHUTDOWN) {
            fprintf(stderr, "jlc: frame for request %u, expected %u\n", h.req_id, c->req_id);
            goto fail;
        }
        switch (h.type) {
        case JLP_T_GET_BODY_CHUNK: {
            uint32_t max;
            if (jlp_get_body_decode(c->ctx, p, h.len, &max) != JLP_OK)
                goto fail;
            size_t cap = c->max_frame - jlp_body_overhead(c->ctx, c->max_frame);
            if (max > cap)
                max = (uint32_t)cap;
            if (!chunk)
                chunk = xmalloc(cap);
            size_t n = body_read(body, chunk, max);
            uint8_t pre[JLP_PREFIX_MAX];
            size_t pn = jlp_body_prefix(c->ctx, n, pre);
            if (send_frame(c, JLP_T_BODY_CHUNK, body->eof ? JLP_F_LAST : 0, pre, pn, chunk, n))
                goto fail;
            break;
        }
        case JLP_T_SEND_HEADERS:
            if (jlp_decode_send_headers(c->ctx, p, h.len, &v, rs) != JLP_OK)
                goto fail;
            break;
        case JLP_T_BODY_CHUNK: {
            const uint8_t *data;
            size_t len;
            if (jlp_body_view(c->ctx, p, h.len, &data, &len) != JLP_OK)
                goto fail;
            rs->body_bytes += len;
            if (rs->out && len && fwrite(data, 1, len, rs->out) != len)
                goto fail;
            break;
        }
        case JLP_T_END_RESPONSE:
            if (jlp_decode_end_response(c->ctx, p, h.len, reuse, &v, rs) != JLP_OK)
                goto fail;
            free(chunk);
            return 0;
        case JLP_T_ERROR: {
            unsigned code;
            char msg[256];
            if (jlp_decode_error(c->ctx, p, h.len, &code, msg, sizeof msg) == JLP_OK)
                fprintf(stderr, "jlc: worker ERROR %u: %s\n", code, msg);
            goto fail;
        }
        case JLP_T_SHUTDOWN:
            /* Sent only on idle connections, so not here; tolerate it. */
            break;
        default:
            fprintf(stderr, "jlc: unexpected %s\n", jlp_type_name(h.type));
            goto fail;
        }
    }
fail:
    free(chunk);
    return -1;
}

static void split_target(const char *path, char **uri, char **query)
{
    const char *q = strchr(path, '?');
    *uri = q ? strndup(path, (size_t)(q - path)) : strdup(path);
    *query = q ? strdup(q + 1) : NULL;
}

static void base_request(jlp_forward_request *fr, const req_opts *ro, char *uri, char *query, int64_t body_length)
{
    memset(fr, 0, sizeof *fr);
    fr->method = ro->method;
    fr->uri = uri;
    fr->query = query;
    fr->headers = ro->headers;
    fr->n_headers = ro->n_headers;
    fr->proto = 1001;
    fr->remote_addr = "127.0.0.1";
    fr->remote_port = 40000;
    fr->local_port = -1;
    fr->server_name = "localhost";
    fr->server_port = 80;
    fr->body_length = body_length;
}

static int run_single(const conn_opts *co, req_opts *ro)
{
    conn c;
    if (open_conn(&c, co))
        die("connect");
    char *uri, *query;
    split_target(ro->path, &uri, &query);
    int rc = 0;
    for (long i = 0; i < ro->count && rc == 0; i++) {
        body_src body = {.fd = -1};
        int64_t blen = JLP_NO_BODY;
        if (ro->data_file) {
            body.fd = strcmp(ro->data_file, "-") == 0 ? 0 : open(ro->data_file, O_RDONLY | O_CLOEXEC);
            if (body.fd < 0)
                die(ro->data_file);
            struct stat st;
            blen = !ro->chunked && fstat(body.fd, &st) == 0 && S_ISREG(st.st_mode) ? st.st_size : -1;
        } else if (ro->data) {
            body.mem = ro->data;
            body.mem_len = strlen(ro->data);
            body.eof = body.mem_len == 0;
            blen = ro->chunked ? -1 : (int64_t)body.mem_len;
        }
        jlp_forward_request fr;
        base_request(&fr, ro, uri, query, blen);
        resp_state rs = {.show_headers = ro->show_headers, .out = stdout};
        int reuse = 0;
        if (do_request(&c, ro, &fr, &body, &rs, &reuse)) {
            fprintf(stderr, "jlc: request failed: %s\n", errno ? strerror(errno) : jlp_last_error());
            rc = 1;
        } else if (!reuse && i + 1 < ro->count) {
            close_conn(&c);
            if (open_conn(&c, co))
                die("reconnect");
        }
        if (body.fd > 0)
            close(body.fd);
    }
    fflush(stdout);
    close_conn(&c);
    free(uri);
    free(query);
    return rc;
}

static int run_ping(const conn_opts *co)
{
    conn c;
    if (open_conn(&c, co))
        die("connect");
    const uint8_t *p;
    size_t n;
    jlp_const_doc(g_ctx, JLP_T_CPING, &p, &n);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    jlp_frame_hdr h;
    if (send_frame(&c, JLP_T_CPING, 0, p, n, NULL, 0) || recv_frame(&c, &h) || h.type != JLP_T_CPONG)
        die("cping");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("CPONG in %.1f us\n", ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3);
    close_conn(&c);
    return 0;
}

/* ===== bench ===== */

typedef struct {
    const conn_opts *co;
    const req_opts *ro;
    double deadline;
    uint64_t *lat; /* ns */
    size_t n_lat, cap_lat;
    uint64_t errors, bytes;
    uint64_t refused; /* reconnects refused after the worker closed cleanly */
} worker;

static atomic_int g_stop;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void *bench_worker(void *arg)
{
    worker *w = arg;
    conn c;
    int connected = open_conn(&c, w->co) == 0;
    if (!connected) {
        w->errors++;
        return NULL;
    }
    char *uri, *query;
    split_target(w->ro->path, &uri, &query);
    while (!atomic_load(&g_stop) && now_s() < w->deadline) {
        body_src body = {.fd = -1, .mem = w->ro->data, .mem_len = w->ro->data ? strlen(w->ro->data) : 0};
        body.eof = w->ro->data && body.mem_len == 0;
        jlp_forward_request fr;
        base_request(&fr, w->ro, uri, query, w->ro->data ? (int64_t)body.mem_len : JLP_NO_BODY);
        resp_state rs = {0};
        int reuse = 0;
        double t0 = now_s();
        errno = 0;
        if (do_request(&c, w->ro, &fr, &body, &rs, &reuse)) {
            if (!w->errors)
                fprintf(stderr, "jlc: request failed: %s\n", errno ? strerror(errno) : jlp_last_error());
            w->errors++;
            reuse = 0;
        } else if (rs.status >= 500) {
            w->errors++;
            reuse = 0;
        } else {
            if (w->n_lat == w->cap_lat) {
                w->cap_lat = w->cap_lat ? 2 * w->cap_lat : 65536;
                w->lat = xrealloc(w->lat, w->cap_lat * sizeof *w->lat);
            }
            w->lat[w->n_lat++] = (uint64_t)((now_s() - t0) * 1e9);
            w->bytes += rs.body_bytes;
        }
        if (!reuse) {
            /* A worker that is draining ends the response with reuse=false
               and stops listening, so a refused reconnect is not an error. */
            close_conn(&c);
            if (atomic_load(&g_stop) || open_conn(&c, w->co)) {
                w->refused++;
                break;
            }
        }
    }
    close_conn(&c);
    free(uri);
    free(query);
    return NULL;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int run_bench(const conn_opts *co, const req_opts *ro, int conns, double secs)
{
    worker *w = xmalloc((size_t)conns * sizeof *w);
    pthread_t *th = xmalloc((size_t)conns * sizeof *th);
    double start = now_s();
    for (int i = 0; i < conns; i++) {
        w[i] = (worker){.co = co, .ro = ro, .deadline = start + secs};
        pthread_create(&th[i], NULL, bench_worker, &w[i]);
    }
    size_t total = 0;
    uint64_t errors = 0, bytes = 0, refused = 0;
    for (int i = 0; i < conns; i++) {
        pthread_join(th[i], NULL);
        total += w[i].n_lat;
        errors += w[i].errors;
        bytes += w[i].bytes;
        refused += w[i].refused;
    }
    double elapsed = now_s() - start;
    uint64_t *all = xmalloc((total ? total : 1) * sizeof *all);
    size_t k = 0;
    for (int i = 0; i < conns; i++) {
        memcpy(all + k, w[i].lat, w[i].n_lat * sizeof *all);
        k += w[i].n_lat;
        free(w[i].lat);
    }
    qsort(all, total, sizeof *all, cmp_u64);
#define PCT(p) (total ? all[(size_t)((double)(total - 1) * (p))] / 1e3 : 0.0)
    printf("%s %s, %d connections, %.1f s\n", ro->method, ro->path, conns, elapsed);
    printf("  requests %zu  errors %llu  %.0f req/s  %.1f MB/s body\n", total, (unsigned long long)errors,
           total / elapsed, bytes / elapsed / 1e6);
    if (refused)
        printf("  reconnects refused after the worker closed: %llu\n", (unsigned long long)refused);
    printf("  latency us: p50 %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f  max %.1f\n", PCT(0.5), PCT(0.9), PCT(0.99),
           PCT(0.999), PCT(1.0));
    free(all);
    free(w);
    free(th);
    return errors ? 1 : 0;
}

/* ===== main ===== */

static void usage(void)
{
    fprintf(stderr, "usage: jlc (--unix PATH | --tcp HOST:PORT) [--secret S] [--max-frame N]\n"
                    "           [-X METHOD] [-H 'Name: value']... [--data S | --data-file F|-] [--chunked]\n"
                    "           [-i] [-n COUNT] PATH[?QUERY]\n"
                    "       jlc CONN --ping\n"
                    "       jlc CONN bench [-c CONNS] [-d SECONDS] [-X METHOD] [-H ...] [--data S] PATH\n");
    exit(2);
}

int main(int argc, char **argv)
{
    conn_opts co = {.max_frame = JLP_MAX_FRAME_DEFAULT, .secret = getenv("JL_SECRET")};
    req_opts ro = {.method = "GET", .count = 1};
    int bench = 0, ping = 0, conns = 16;
    double secs = 5;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (usage(), (char *)NULL))
        if (!strcmp(a, "--unix"))
            co.unix_path = NEXT();
        else if (!strcmp(a, "--tcp"))
            co.tcp = NEXT();
        else if (!strcmp(a, "--secret"))
            co.secret = NEXT();
        else if (!strcmp(a, "--max-frame"))
            co.max_frame = (uint32_t)atol(NEXT());
        else if (!strcmp(a, "-X"))
            ro.method = NEXT();
        else if (!strcmp(a, "-H")) {
            char *h = NEXT(); /* split in place: argv strings are writable */
            char *colon = strchr(h, ':');
            if (!colon || ro.n_headers == MAX_HEADERS)
                usage();
            *colon = '\0';
            char *v = colon + 1;
            while (*v == ' ')
                v++;
            ro.headers[ro.n_headers++] = (jlp_header){h, v};
        } else if (!strcmp(a, "--data"))
            ro.data = NEXT();
        else if (!strcmp(a, "--data-file"))
            ro.data_file = NEXT();
        else if (!strcmp(a, "--chunked"))
            ro.chunked = 1;
        else if (!strcmp(a, "-i"))
            ro.show_headers = 1;
        else if (!strcmp(a, "-n"))
            ro.count = atol(NEXT());
        else if (!strcmp(a, "-c"))
            conns = atoi(NEXT());
        else if (!strcmp(a, "-d"))
            secs = atof(NEXT());
        else if (!strcmp(a, "--ping"))
            ping = 1;
        else if (!strcmp(a, "bench"))
            bench = 1;
        else if (a[0] == '-')
            usage();
        else
            ro.path = a;
    }
    if (!co.unix_path == !co.tcp || (!ping && !ro.path))
        usage();
    if (jlp_ctx_create(&g_ctx, NULL) != JLP_OK)
        die("jlp_ctx_create");
    int rc = ping ? run_ping(&co) : bench ? run_bench(&co, &ro, conns, secs) : run_single(&co, &ro);
    jlp_ctx_destroy(g_ctx);
    return rc;
}
