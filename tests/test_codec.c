/* libjlproto tests: byte equality with the rexi CLI reference vectors
 * (testdata/vectors, from schema/samples), decoding, templates, the handshake,
 * edge cases (non-UTF-8, empty values, unknown names) and a mutation loop that
 * must never crash (run it under -DJL_SANITIZE=ON). */

#include "jlp.h"
#include "rexi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static const char *vectors_dir;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s) failed (%s)\n", __FILE__, __LINE__, #cond, jlp_last_error());            \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static uint8_t *slurp(const char *name, size_t *n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", vectors_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(len ? (size_t)len : 1);
    if (fread(p, 1, (size_t)len, f) != (size_t)len)
        exit(2);
    fclose(f);
    *n = (size_t)len;
    return p;
}

static void hexdiff(const char *what, const uint8_t *a, size_t na, const uint8_t *b, size_t nb)
{
    fprintf(stderr, "%s differs (%zu vs %zu bytes)\n  got:  ", what, na, nb);
    for (size_t i = 0; i < na; i++)
        fprintf(stderr, "%02x", a[i]);
    fprintf(stderr, "\n  want: ");
    for (size_t i = 0; i < nb; i++)
        fprintf(stderr, "%02x", b[i]);
    fprintf(stderr, "\n");
}

static void expect_vector(const char *name, const uint8_t *p, size_t n)
{
    size_t vn;
    uint8_t *v = slurp(name, &vn);
    if (n != vn || memcmp(p, v, n) != 0) {
        hexdiff(name, p, n, v, vn);
        failures++;
    }
    free(v);
}

/* ===== XML-ish dump through rexi, to check what the encoder produced ===== */

typedef struct {
    char buf[8192];
    size_t len;
} dump;

static void put(dump *d, const char *s, size_t n)
{
    if (d->len + n < sizeof d->buf) {
        memcpy(d->buf + d->len, s, n);
        d->len += n;
        d->buf[d->len] = '\0';
    }
}

static int d_se(RexiStr u, RexiStr l, void *ud)
{
    put(ud, "<", 1);
    put(ud, l.ptr, l.len);
    put(ud, ">", 1);
    return 0;
}
static int d_ee(void *ud)
{
    put(ud, "/", 1);
    return 0;
}
static int d_at(RexiStr u, RexiStr l, const RexiValue *v, void *ud)
{
    put(ud, "@", 1);
    put(ud, l.ptr, l.len);
    put(ud, "=", 1);
    put(ud, v->lexical, v->lexical_len);
    put(ud, " ", 1);
    return 0;
}
static int d_ch(const RexiValue *v, void *ud)
{
    put(ud, "'", 1);
    put(ud, v->lexical, v->lexical_len);
    put(ud, "'", 1);
    return 0;
}

static const char *dump_doc(const jlp_buf *b, dump *d)
{
    static const char *xsd;
    static RexiTables *t;
    if (!t) {
        xsd = jlp_schema_text(NULL);
        if (rexi_tables_from_xsd_string(xsd, NULL, &t) != REXI_STATUS_OK)
            exit(2);
    }
    RexiOptions o = rexi_options_strict();
    o.alignment = REXI_ALIGN_BYTE_ALIGNED;
    o.value_max_length = 128;
    RexiContentHandler h = {.start_element = d_se, .end_element = d_ee, .attribute = d_at, .characters = d_ch};
    d->len = 0;
    d->buf[0] = '\0';
    if (rexi_decode(t, b->data, b->len, &o, &h, d) != REXI_STATUS_OK)
        snprintf(d->buf, sizeof d->buf, "DECODE ERROR: %s", rexi_last_error());
    return d->buf;
}

/* ===== Tests ===== */

static void test_forward_vectors(const jlp_ctx *ctx)
{
    jlp_header h1[] = {
        {"Host", "localhost:18080"}, {"User-Agent", "curl/8.5.0"},    {"Accept", "*/*"},
        {"X-Trace", "abc123"},       {"Cookie", "JSESSIONID=abc.j1"},
    };
    jlp_forward_request get = {
        .method = "GET",
        .uri = "/app/hello/world",
        .query = "a=1&b=2",
        .headers = h1,
        .n_headers = 5,
        .proto = 1001,
        .remote_addr = "127.0.0.1",
        .remote_port = 51234,
        .local_port = -1,
        .server_name = "localhost",
        .server_port = 18080,
        .route = "j1",
        .body_length = JLP_NO_BODY,
    };
    jlp_buf b;
    CHECK(jlp_encode_forward(ctx, &get, &b) == JLP_OK);
    expect_vector("forward_get.exi", b.data, b.len);
    jlp_buf_free(&b);

    jlp_header h2[] = {{"content-type", "application/octet-stream"}, {"Content-Length", "11"}};
    jlp_header env[] = {{"UNIQUE_ID", "Zx1"}};
    jlp_ssl ssl = {.cipher = "TLS_AES_128_GCM_SHA256", .protocol = "TLSv1.3", .key_size = 128};
    jlp_forward_request post = {
        .method = "BREW",
        .uri = "/coffee",
        .headers = h2,
        .n_headers = 2,
        .proto = 2000,
        .is_ssl = 1,
        .remote_addr = "::1",
        .remote_port = 40000,
        .local_addr = "10.0.0.1",
        .local_port = 443,
        .server_name = "example.org",
        .server_port = 443,
        .auth_type = "Basic",
        .remote_user = "alice",
        .body_length = 11,
        .ssl = &ssl,
        .env = env,
        .n_env = 1,
        .body = (const uint8_t *)"Hello World",
        .body_len = 11,
    };
    CHECK(jlp_encode_forward(ctx, &post, &b) == JLP_OK);
    expect_vector("forward_post.exi", b.data, b.len);
    jlp_buf_free(&b);
}

static void test_forward_edges(const jlp_ctx *ctx)
{
    dump d;
    jlp_buf b;
    jlp_header h[] = {
        {"X-Empty", ""}, {"X-Space", "  "}, {"ACCEPT", "text/html"}, {"X-Latin1", "caf\xe9"}, {"x-\xff", "v"},
    };
    jlp_forward_request fr = {
        .method = "M-SEARCH",
        .uri = "/caf\xe9/x",
        .headers = h,
        .n_headers = 5,
        .proto = 1001,
        .remote_addr = "1.2.3.4",
        .remote_port = 1,
        .local_port = -1,
        .server_name = "h\xff",
        .server_port = 80,
        .body_length = -1,
    };
    CHECK(jlp_encode_forward(ctx, &fr, &b) == JLP_OK);
    const char *got = dump_doc(&b, &d);
    const char *want = "<ForwardRequest>@bodyLength=-1 @isSsl=false @proto=1001 @remoteAddr=1.2.3.4 @remotePort=1 "
                       "@serverName=h\xef\xbf\xbd @serverPort=80 <ExtMethod>'M-SEARCH'/<UriRaw>'L2NhZukveA=='/"
                       "<Headers><X>@n=X-Empty ''/<X>@n=X-Space '  '/<H>@n=accept 'text/html'/"
                       "<R>@n=WC1MYXRpbjE= 'Y2Fm6Q=='/<R>@n=eC3/ 'dg=='///";
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "edge encoding:\n  got:  %s\n  want: %s\n", got, want);
        failures++;
    }
    jlp_buf_free(&b);
}

typedef struct {
    unsigned status;
    char reason[32];
    char headers[512];
    int n;
} collected;

static int c_status(void *ud, unsigned status, const char *reason, size_t len)
{
    collected *c = ud;
    c->status = status;
    snprintf(c->reason, sizeof c->reason, "%.*s", (int)len, reason ? reason : "");
    return 0;
}

static int c_header(void *ud, int ord, const char *name, size_t nlen, const char *value, size_t vlen, int raw)
{
    collected *c = ud;
    size_t used = strlen(c->headers);
    snprintf(c->headers + used, sizeof c->headers - used, "%d:%.*s=%.*s%s;", ord, (int)nlen, name, (int)vlen, value,
             raw ? "(raw)" : "");
    c->n++;
    return 0;
}

static void test_decode(const jlp_ctx *ctx)
{
    size_t n;
    uint8_t *p = slurp("send_headers.exi", &n);
    jlp_headers_visitor v = {.status = c_status, .header = c_header};
    collected c = {0};
    CHECK(jlp_decode_send_headers(ctx, p, n, &v, &c) == JLP_OK);
    CHECK(c.status == 200);
    CHECK(strcmp(c.reason, "OK") == 0);
    CHECK(strcmp(c.headers, "25:content-type=text/plain; charset=utf-8;62:set-cookie=a=1;62:set-cookie=b=2;"
                            "-1:X-Powered-By=JlServ;") == 0);
    if (c.n != 4)
        fprintf(stderr, "headers: %s\n", c.headers);
    /* The wrong message type is a protocol error, not a crash. */
    CHECK(jlp_decode_end_response(ctx, p, n, &(int){0}, NULL, NULL) == JLP_EPROTO);
    free(p);

    int reuse = -1;
    p = slurp("end_response.exi", &n);
    CHECK(jlp_decode_end_response(ctx, p, n, &reuse, NULL, NULL) == JLP_OK && reuse == 1);
    free(p);

    int graceful = -1;
    char reason[64];
    p = slurp("shutdown.exi", &n);
    CHECK(jlp_decode_shutdown(ctx, p, n, &graceful, reason, sizeof reason) == JLP_OK);
    CHECK(graceful == 1 && strcmp(reason, "restart") == 0);
    free(p);

    unsigned code = 0;
    char msg[8]; /* truncation */
    p = slurp("error.exi", &n);
    CHECK(jlp_decode_error(ctx, p, n, &code, msg, sizeof msg) == JLP_OK);
    CHECK(code == 500 && strcmp(msg, "handler") == 0);
    free(p);
}

static void test_templates(const jlp_ctx *ctx)
{
    size_t n;
    const uint8_t *data;
    size_t len;
    uint8_t *p = slurp("body_chunk.exi", &n);
    CHECK(jlp_body_view(ctx, p, n, &data, &len) == JLP_OK);
    CHECK(len == 11 && memcmp(data, "Hello World", 11) == 0);
    uint8_t pre[JLP_PREFIX_MAX];
    size_t pn = jlp_body_prefix(ctx, 11, pre);
    CHECK(pn == n - 11 && memcmp(pre, p, pn) == 0);
    CHECK(jlp_body_overhead(ctx, 11) == pn);
    /* One byte short, one byte long. */
    CHECK(jlp_body_view(ctx, p, n - 1, &data, &len) == JLP_EPROTO);
    free(p);

    p = slurp("get_body_chunk.exi", &n);
    uint32_t max = 0;
    CHECK(jlp_get_body_decode(ctx, p, n, &max) == JLP_OK && max == 65000);
    /* Every truncation fails, including the template with no varint at all
       (it used to pass with max read from an uninitialised value). */
    for (size_t k = 0; k < n; k++)
        CHECK(jlp_get_body_decode(ctx, p, k, &max) == JLP_EPROTO);
    pn = jlp_get_body_encode(ctx, 65000, pre);
    CHECK(pn == n && memcmp(pre, p, n) == 0);
    free(p);

    const uint8_t *cp;
    size_t cn;
    CHECK(jlp_const_doc(ctx, JLP_T_CPING, &cp, &cn) == JLP_OK);
    expect_vector("cping.exi", cp, cn);
    CHECK(jlp_const_doc(ctx, JLP_T_CPONG, &cp, &cn) == JLP_OK);
    expect_vector("cpong.exi", cp, cn);
    CHECK(jlp_const_doc(ctx, JLP_T_END_RESPONSE, &cp, &cn) == JLP_OK);
    expect_vector("end_response.exi", cp, cn);
}

static void test_framing(void)
{
    uint8_t h[JLP_FRAME_HDR];
    jlp_frame_hdr in = {.type = JLP_T_SEND_HEADERS, .flags = JLP_F_FLUSH, .req_id = 0xA1B2C3D4, .len = 70}, out;
    jlp_hdr_pack(h, &in);
    CHECK(jlp_hdr_unpack(h, 65536, &out) == JLP_OK);
    CHECK(out.type == in.type && out.flags == in.flags && out.req_id == in.req_id && out.len == in.len);
    CHECK(jlp_hdr_unpack(h, 69, &out) == JLP_ETOOBIG);
    h[0] = 0;
    CHECK(jlp_hdr_unpack(h, 65536, &out) == JLP_EPROTO);

    uint8_t buf[256];
    jlp_hello hello = {.minor = 0,
                       .features = JLP_FEAT_BYTE_ALIGNED | JLP_FEAT_SECRET,
                       .max_frame = 65536,
                       .secret = "s3cret",
                       .secret_len = 6,
                       .agent = "mod_jl/0.1",
                       .agent_len = 10};
    memcpy(hello.schema_sha256, jlp_schema_sha256(), 32);
    size_t n = jlp_hello_pack(buf, sizeof buf, &hello);
    jlp_hello back;
    CHECK(n == 60 && jlp_hello_unpack(buf, n, &back) == JLP_OK);
    CHECK(back.max_frame == 65536 && back.secret_len == 6 && memcmp(back.secret, "s3cret", 6) == 0);
    CHECK(back.agent_len == 10 && memcmp(back.schema_sha256, jlp_schema_sha256(), 32) == 0);
    CHECK(jlp_hello_unpack(buf, n - 1, &back) == JLP_EPROTO);

    jlp_hello_ack ack = {.status = JLP_HELLO_OK, .max_frame = 32768, .pid = 4242, .name = "j1", .name_len = 2}, ack2;
    n = jlp_hello_ack_pack(buf, sizeof buf, &ack);
    CHECK(jlp_hello_ack_unpack(buf, n, &ack2) == JLP_OK && ack2.pid == 4242 && ack2.max_frame == 32768);
}

static void test_utf8(void)
{
    CHECK(jlp_utf8_valid("plain ascii, long enough for the fast path", 42));
    CHECK(jlp_utf8_valid("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80", 14));
    CHECK(!jlp_utf8_valid("\xc0\xaf", 2));         /* overlong */
    CHECK(!jlp_utf8_valid("\xed\xa0\x80", 3));     /* surrogate */
    CHECK(!jlp_utf8_valid("\xf4\x90\x80\x80", 4)); /* > U+10FFFF */
    CHECK(!jlp_utf8_valid("abc\xe2\x82", 5));      /* truncated */
}

static int noop_status(void *ud, unsigned s, const char *r, size_t n)
{
    return 0;
}
static int noop_header(void *ud, int o, const char *n, size_t nl, const char *v, size_t vl, int raw)
{
    return 0;
}

/* Random byte flips, truncations and extensions of every vector, fed to every
 * decoder. Only memory safety is checked here (use ASan or valgrind). */
static void test_mutations(const jlp_ctx *ctx)
{
    static const char *names[] = {"send_headers.exi", "end_response.exi", "shutdown.exi",
                                  "error.exi",        "body_chunk.exi",   "get_body_chunk.exi"};
    jlp_headers_visitor v = {.status = noop_status, .header = noop_header};
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    /* JLP_MUTATIONS shortens the loop, e.g. under valgrind. */
    const char *env = getenv("JLP_MUTATIONS");
    int iters = env ? atoi(env) : 20000;
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
        size_t n;
        uint8_t *orig = slurp(names[k], &n);
        uint8_t *m = malloc(n + 8);
        for (int iter = 0; iter < iters; iter++) {
            memcpy(m, orig, n);
            rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
            size_t len = n;
            switch (rng % 3) {
            case 0:
                m[(rng >> 8) % n] ^= (uint8_t)(1u << ((rng >> 40) % 8));
                break;
            case 1:
                len = (rng >> 8) % (n + 1);
                break;
            default:
                m[(rng >> 8) % n] = (uint8_t)(rng >> 32);
                len = n + (rng >> 48) % 8;
                memset(m + n, (int)(rng >> 16), len - n);
            }
            int reuse, graceful;
            unsigned code;
            uint32_t max;
            char buf[16];
            const uint8_t *data;
            size_t dl;
            jlp_decode_send_headers(ctx, m, len, &v, NULL);
            jlp_decode_end_response(ctx, m, len, &reuse, &v, NULL);
            jlp_decode_shutdown(ctx, m, len, &graceful, buf, sizeof buf);
            jlp_decode_error(ctx, m, len, &code, buf, sizeof buf);
            jlp_body_view(ctx, m, len, &data, &dl);
            jlp_get_body_decode(ctx, m, len, &max);
        }
        free(m);
        free(orig);
    }
}

int main(int argc, char **argv)
{
    vectors_dir = argc > 1 ? argv[1] : "testdata/vectors";
    jlp_ctx *ctx;
    if (jlp_ctx_create(&ctx, NULL) != JLP_OK) {
        fprintf(stderr, "jlp_ctx_create: %s\n", jlp_last_error());
        return 1;
    }
    test_framing();
    test_utf8();
    test_templates(ctx);
    test_forward_vectors(ctx);
    test_forward_edges(ctx);
    test_decode(ctx);
    test_mutations(ctx);
    jlp_ctx_destroy(ctx);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_codec: all checks passed (rexi %s)\n", jlp_rexi_version());
    return 0;
}
