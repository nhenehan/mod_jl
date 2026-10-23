/* Decoding of the messages a worker sends: SendHeaders, EndResponse (with
 * trailers), Shutdown and Error. One content-handler state machine serves all
 * of them. rexi's strings live only for the callback, so a header name (an
 * attribute) is copied until its value (the element's content) arrives. */

#include "jlp_internal.h"

#include "jlp_vocab.h"

#include <stdlib.h>
#include <string.h>

enum hdr_kind { HK_NONE, HK_H, HK_X, HK_R };

typedef struct {
    const char *root; /* expected global element */
    const jlp_headers_visitor *v;
    void *ud;
    int depth;
    int rc; /* first error from a callback or a check */

    enum hdr_kind hk;
    int ord;
    bool emitted;
    char *name;
    size_t name_len, name_cap;

    /* Root attributes. */
    char *reason; /* SendHeaders reason, Shutdown reason, Error message */
    size_t reason_len, reason_cap;
    bool have_reason;
    uint64_t status; /* status / code */
    bool have_status;
    int flag; /* reuse / graceful */
    bool have_flag;
} dec_state;

static bool eq(RexiStr s, const char *lit)
{
    size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

static int keep(char **buf, size_t *cap, size_t *len, const void *p, size_t n)
{
    if (n + 1 > *cap) {
        size_t c = n + 1 < 64 ? 64 : n + 1;
        char *b = realloc(*buf, c);
        if (!b)
            return -1;
        *buf = b;
        *cap = c;
    }
    if (n)
        memcpy(*buf, p, n);
    (*buf)[n] = '\0';
    *len = n;
    return 0;
}

/* Bounded integer types (Status) decode as INTEGER, others as UNSIGNED. */
static uint64_t uint_of(const RexiValue *v)
{
    if (v->kind == REXI_VALUE_INTEGER)
        return v->int_value < 0 ? 0 : (uint64_t)v->int_value;
    return v->uint_value;
}

static int fail(dec_state *s, int rc)
{
    if (s->rc == JLP_OK)
        s->rc = rc;
    return 1;
}

static int emit(dec_state *s, const char *value, size_t value_len)
{
    s->emitted = true;
    if (!s->v || !s->v->header)
        return 0;
    const char *name = s->name;
    size_t nlen = s->name_len;
    if (s->hk == HK_H) {
        if (s->ord < 0 || s->ord >= JLP_HEADER_COUNT)
            return fail(s, jlp_fail(JLP_EPROTO, "header ordinal %d out of range", s->ord));
        name = jlp_header_names[s->ord];
        nlen = jlp_header_name_lens[s->ord];
    }
    if (s->v->header(s->ud, s->hk == HK_H ? s->ord : -1, name ? name : "", nlen, value, value_len, s->hk == HK_R))
        return fail(s, jlp_fail(JLP_EABORT, "header visitor stopped decoding"));
    return 0;
}

static int on_start(RexiStr uri, RexiStr local, void *user)
{
    dec_state *s = user;
    s->depth++;
    if (s->depth == 1) {
        if (!eq(local, s->root))
            return fail(s, jlp_fail(JLP_EPROTO, "expected %s, got %.*s", s->root, (int)local.len, local.ptr));
    } else if (s->depth == 3 && local.len == 1) {
        s->hk = local.ptr[0] == 'H' ? HK_H : local.ptr[0] == 'X' ? HK_X : local.ptr[0] == 'R' ? HK_R : HK_NONE;
        s->emitted = false;
        s->ord = -1;
        s->name_len = 0;
    }
    return 0;
}

static int on_attribute(RexiStr uri, RexiStr local, const RexiValue *v, void *user)
{
    dec_state *s = user;
    if (s->depth == 1) {
        if (eq(local, "status") || eq(local, "code")) {
            s->status = uint_of(v);
            s->have_status = true;
        } else if (eq(local, "reason") || eq(local, "message")) {
            if (keep(&s->reason, &s->reason_cap, &s->reason_len, v->lexical, v->lexical_len))
                return fail(s, JLP_ENOMEM);
            s->have_reason = true;
        } else if (eq(local, "reuse") || eq(local, "graceful")) {
            s->flag = v->bool_value;
            s->have_flag = true;
        }
        /* SendHeaders: status is the last attribute in EXI order. */
        if (s->have_status && s->v && s->v->status && eq(local, "status")) {
            if (s->v->status(s->ud, (unsigned)s->status, s->have_reason ? s->reason : NULL,
                             s->have_reason ? s->reason_len : 0))
                return fail(s, jlp_fail(JLP_EABORT, "status visitor stopped decoding"));
        }
        return 0;
    }
    if (s->hk == HK_NONE || !eq(local, "n"))
        return 0;
    if (s->hk == HK_H) {
        s->ord = (int)v->uint_value;
    } else {
        const void *p = s->hk == HK_R ? (const void *)v->bytes : (const void *)v->lexical;
        size_t n = s->hk == HK_R ? v->bytes_len : v->lexical_len;
        if (keep(&s->name, &s->name_cap, &s->name_len, p, n))
            return fail(s, JLP_ENOMEM);
    }
    return 0;
}

static int on_characters(const RexiValue *v, void *user)
{
    dec_state *s = user;
    if (s->hk == HK_NONE || s->emitted)
        return 0;
    if (s->hk == HK_R)
        return emit(s, (const char *)v->bytes, v->bytes_len);
    return emit(s, v->lexical, v->lexical_len);
}

static int on_end(void *user)
{
    dec_state *s = user;
    if (s->depth == 3 && s->hk != HK_NONE) {
        if (!s->emitted && emit(s, "", 0))
            return 1;
        s->hk = HK_NONE;
    }
    s->depth--;
    return 0;
}

static const RexiContentHandler handler = {
    .start_element = on_start,
    .end_element = on_end,
    .attribute = on_attribute,
    .characters = on_characters,
};

static int decode(const jlp_ctx *ctx, const uint8_t *p, size_t n, dec_state *s)
{
    RexiStatus st = rexi_decode(ctx->tables, p, n, &ctx->opts, &handler, s);
    free(s->name);
    if (s->rc != JLP_OK) {
        free(s->reason);
        s->reason = NULL;
        return s->rc;
    }
    if (st != REXI_STATUS_OK) {
        free(s->reason);
        s->reason = NULL;
        return jlp_fail_rexi(st, s->root);
    }
    return JLP_OK;
}

int jlp_decode_send_headers(const jlp_ctx *ctx, const uint8_t *p, size_t n, const jlp_headers_visitor *v, void *ud)
{
    dec_state s = {.root = "SendHeaders", .v = v, .ud = ud};
    int rc = decode(ctx, p, n, &s);
    free(s.reason);
    return rc;
}

int jlp_decode_end_response(const jlp_ctx *ctx, const uint8_t *p, size_t n, int *reuse, const jlp_headers_visitor *v,
                            void *ud)
{
    /* The two trailer-less forms are constant documents. */
    if (n == ctx->end_reuse.len && memcmp(p, ctx->end_reuse.data, n) == 0) {
        *reuse = 1;
        return JLP_OK;
    }
    if (n == ctx->end_noreuse.len && memcmp(p, ctx->end_noreuse.data, n) == 0) {
        *reuse = 0;
        return JLP_OK;
    }
    dec_state s = {.root = "EndResponse", .v = v, .ud = ud};
    int rc = decode(ctx, p, n, &s);
    free(s.reason);
    if (rc == JLP_OK)
        *reuse = s.flag;
    return rc;
}

static void copy_out(char *dst, size_t cap, const dec_state *s)
{
    if (!dst || !cap)
        return;
    size_t k = s->have_reason ? (s->reason_len < cap - 1 ? s->reason_len : cap - 1) : 0;
    if (k)
        memcpy(dst, s->reason, k);
    dst[k] = '\0';
}

int jlp_decode_shutdown(const jlp_ctx *ctx, const uint8_t *p, size_t n, int *graceful, char *reason, size_t reason_cap)
{
    dec_state s = {.root = "Shutdown"};
    int rc = decode(ctx, p, n, &s);
    if (rc == JLP_OK) {
        *graceful = s.flag;
        copy_out(reason, reason_cap, &s);
    }
    free(s.reason);
    return rc;
}

int jlp_decode_error(const jlp_ctx *ctx, const uint8_t *p, size_t n, unsigned *code, char *message, size_t message_cap)
{
    dec_state s = {.root = "Error"};
    int rc = decode(ctx, p, n, &s);
    if (rc == JLP_OK) {
        *code = (unsigned)s.status;
        copy_out(message, message_cap, &s);
    }
    free(s.reason);
    return rc;
}
