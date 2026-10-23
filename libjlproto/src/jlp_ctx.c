/* The shared context: compiled grammar tables, out-of-band options, and the
 * byte templates for the messages that never go through the codec at run time.
 *
 * A template is learnt from rexi's own output and checked against several
 * values before it is trusted, so a schema or codec change that breaks the
 * "prefix + uvarint" shape makes jlp_ctx_create fail instead of corrupting
 * frames. */

#include "jlp_internal.h"

#include <stdlib.h>
#include <string.h>

const uint8_t *jlp_schema_sha256(void)
{
    return jlp_schema_xsd_sha256;
}

const char *jlp_schema_text(size_t *len)
{
    if (len)
        *len = jlp_schema_xsd_len;
    return jlp_schema_xsd;
}

const char *jlp_rexi_version(void)
{
    return rexi_version();
}

/* Encodes <name/> with at most one attribute (an unsigned or boolean) or one
 * binary value as content. */
typedef struct {
    const char *attr;
    RexiValue attr_value;
    const uint8_t *bin;
    size_t bin_len;
    bool has_bin;
} simple_doc;

static int encode_simple(const jlp_ctx *ctx, const char *name, const simple_doc *d, RexiBuffer *out)
{
    RexiEncoder *enc = NULL;
    RexiStatus st = rexi_encoder_new(ctx->tables, &ctx->opts, &enc);
    if (st != REXI_STATUS_OK)
        return jlp_fail_rexi(st, name);
    if ((st = rexi_start_document(enc)) != REXI_STATUS_OK)
        goto fail;
    if ((st = rexi_start_element(enc, JLP_NS, name)) != REXI_STATUS_OK)
        goto fail;
    if (d && d->attr && (st = rexi_attribute(enc, "", d->attr, &d->attr_value)) != REXI_STATUS_OK)
        goto fail;
    if (d && d->has_bin) {
        RexiValue v = {.kind = REXI_VALUE_BINARY, .bytes = d->bin, .bytes_len = d->bin_len};
        if ((st = rexi_characters(enc, &v)) != REXI_STATUS_OK)
            goto fail;
    }
    if ((st = rexi_end_element(enc)) != REXI_STATUS_OK || (st = rexi_end_document(enc)) != REXI_STATUS_OK)
        goto fail;
    st = rexi_encoder_finish(enc, out);
    return st == REXI_STATUS_OK ? JLP_OK : jlp_fail_rexi(st, name);
fail:
    rexi_encoder_free(enc);
    return jlp_fail_rexi(st, name);
}

static int make_const(const jlp_ctx *ctx, const char *name, const simple_doc *d, jlp_const *c)
{
    RexiBuffer b;
    int rc = encode_simple(ctx, name, d, &b);
    if (rc != JLP_OK)
        return rc;
    c->data = malloc(b.len);
    if (!c->data) {
        rexi_buffer_free(b);
        return JLP_ENOMEM;
    }
    memcpy(c->data, b.data, b.len);
    c->len = b.len;
    rexi_buffer_free(b);
    return JLP_OK;
}

/* doc(v) must be prefix || uvarint(v) [|| v bytes of payload]. */
static int check_template(const jlp_template *t, const RexiBuffer *b, uint64_t v, const uint8_t *payload,
                          size_t payload_len)
{
    uint8_t vi[10];
    size_t vn = jlp_uvarint_put(vi, v);
    if (b->len != t->len + vn + payload_len || memcmp(b->data, t->bytes, t->len) != 0 ||
        memcmp(b->data + t->len, vi, vn) != 0 ||
        (payload_len && memcmp(b->data + t->len + vn, payload, payload_len) != 0))
        return jlp_fail(JLP_ESCHEMA, "template check failed at value %llu", (unsigned long long)v);
    return JLP_OK;
}

static int learn_template(const jlp_ctx *ctx, const char *name, bool binary, jlp_template *t)
{
    static const uint64_t probes[] = {0, 1, 127, 128, 255, 16383, 16384, 65535, 65536, 1u << 20, 0xFFFFFFFFu};
    simple_doc d = {0};
    if (binary) {
        d.has_bin = true;
    } else {
        d.attr = "max";
        d.attr_value.kind = REXI_VALUE_UNSIGNED;
    }

    /* doc(0) ends in uvarint(0), the single byte 0x00. */
    RexiBuffer b;
    int rc = encode_simple(ctx, name, &d, &b);
    if (rc != JLP_OK)
        return rc;
    if (b.len < 2 || b.len - 1 > JLP_PREFIX_MAX || b.data[b.len - 1] != 0) {
        rexi_buffer_free(b);
        return jlp_fail(JLP_ESCHEMA, "%s is not a prefix + uvarint document", name);
    }
    memcpy(t->bytes, b.data, b.len - 1);
    t->len = (uint8_t)(b.len - 1);
    rexi_buffer_free(b);

    uint8_t *payload = NULL;
    if (binary && !(payload = malloc(1u << 20)))
        return JLP_ENOMEM;
    for (size_t i = 0; i < sizeof probes / sizeof probes[0] && rc == JLP_OK; i++) {
        uint64_t v = probes[i];
        if (binary) {
            if (v > (1u << 20))
                break;
            for (size_t k = 0; k < v; k++)
                payload[k] = (uint8_t)(k * 131 + 7);
            d.bin = payload;
            d.bin_len = v;
        } else {
            d.attr_value.uint_value = v;
        }
        if ((rc = encode_simple(ctx, name, &d, &b)) != JLP_OK)
            break;
        rc = check_template(t, &b, v, binary ? payload : NULL, binary ? v : 0);
        rexi_buffer_free(b);
    }
    free(payload);
    return rc;
}

int jlp_ctx_create(jlp_ctx **out, const jlp_config *cfg)
{
    if (!out)
        return jlp_fail(JLP_EINVAL, "jlp_ctx_create: out is NULL");
    jlp_ctx *ctx = calloc(1, sizeof *ctx);
    if (!ctx)
        return JLP_ENOMEM;

    RexiStatus st = rexi_tables_from_xsd_string(jlp_schema_xsd, NULL, &ctx->tables);
    if (st != REXI_STATUS_OK) {
        free(ctx);
        jlp_fail_rexi(st, "compiling jlp2.xsd");
        return JLP_ESCHEMA;
    }
    ctx->opts = rexi_options_strict();
    ctx->opts.alignment = REXI_ALIGN_BYTE_ALIGNED;
    ctx->opts.value_max_length = cfg ? cfg->value_max_length : JLP_VALUE_MAX_LENGTH_DEFAULT;

    simple_doc reuse = {.attr = "reuse", .attr_value = {.kind = REXI_VALUE_BOOLEAN, .bool_value = true}};
    simple_doc noreuse = {.attr = "reuse", .attr_value = {.kind = REXI_VALUE_BOOLEAN, .bool_value = false}};
    int rc;
    if ((rc = learn_template(ctx, "BodyChunk", true, &ctx->body)) != JLP_OK ||
        (rc = learn_template(ctx, "GetBodyChunk", false, &ctx->get_body)) != JLP_OK ||
        (rc = make_const(ctx, "CPing", NULL, &ctx->cping)) != JLP_OK ||
        (rc = make_const(ctx, "CPong", NULL, &ctx->cpong)) != JLP_OK ||
        (rc = make_const(ctx, "EndResponse", &reuse, &ctx->end_reuse)) != JLP_OK ||
        (rc = make_const(ctx, "EndResponse", &noreuse, &ctx->end_noreuse)) != JLP_OK) {
        jlp_ctx_destroy(ctx);
        return rc == JLP_EREXI ? JLP_ESCHEMA : rc;
    }
    *out = ctx;
    return JLP_OK;
}

void jlp_ctx_destroy(jlp_ctx *ctx)
{
    if (!ctx)
        return;
    rexi_tables_free(ctx->tables);
    free(ctx->cping.data);
    free(ctx->cpong.data);
    free(ctx->end_reuse.data);
    free(ctx->end_noreuse.data);
    free(ctx);
}

int jlp_const_doc(const jlp_ctx *ctx, int which, const uint8_t **p, size_t *n)
{
    const jlp_const *c;
    switch (which) {
    case JLP_T_CPING:
        c = &ctx->cping;
        break;
    case JLP_T_CPONG:
        c = &ctx->cpong;
        break;
    case JLP_T_END_RESPONSE:
        c = &ctx->end_reuse;
        break;
    case JLP_CONST_END_NOREUSE:
        c = &ctx->end_noreuse;
        break;
    default:
        return jlp_fail(JLP_EINVAL, "no constant document %#x", which);
    }
    *p = c->data;
    *n = c->len;
    return JLP_OK;
}

/* ===== Template messages ===== */

size_t jlp_body_prefix(const jlp_ctx *ctx, size_t len, uint8_t out[JLP_PREFIX_MAX])
{
    memcpy(out, ctx->body.bytes, ctx->body.len);
    return ctx->body.len + jlp_uvarint_put(out + ctx->body.len, len);
}

size_t jlp_body_overhead(const jlp_ctx *ctx, size_t len)
{
    uint8_t tmp[10];
    return ctx->body.len + jlp_uvarint_put(tmp, len);
}

int jlp_body_view(const jlp_ctx *ctx, const uint8_t *p, size_t n, const uint8_t **data, size_t *len)
{
    const jlp_template *t = &ctx->body;
    uint64_t v;
    size_t vn;
    if (n < t->len || memcmp(p, t->bytes, t->len) != 0 || !(vn = jlp_uvarint_get(p + t->len, n - t->len, &v)) ||
        v != n - t->len - vn)
        return jlp_fail(JLP_EPROTO, "malformed BodyChunk (%zu bytes)", n);
    *data = p + t->len + vn;
    *len = (size_t)v;
    return JLP_OK;
}

size_t jlp_get_body_encode(const jlp_ctx *ctx, uint32_t max, uint8_t out[JLP_PREFIX_MAX])
{
    memcpy(out, ctx->get_body.bytes, ctx->get_body.len);
    return ctx->get_body.len + jlp_uvarint_put(out + ctx->get_body.len, max);
}

int jlp_get_body_decode(const jlp_ctx *ctx, const uint8_t *p, size_t n, uint32_t *max)
{
    const jlp_template *t = &ctx->get_body;
    uint64_t v;
    size_t vn;
    /* vn == 0 is a missing or unterminated varint, even when the template
       fills the whole payload. */
    if (n < t->len || memcmp(p, t->bytes, t->len) != 0 || !(vn = jlp_uvarint_get(p + t->len, n - t->len, &v)) ||
        vn != n - t->len || v > UINT32_MAX)
        return jlp_fail(JLP_EPROTO, "malformed GetBodyChunk (%zu bytes)", n);
    *max = (uint32_t)v;
    return JLP_OK;
}

void jlp_buf_free(jlp_buf *b)
{
    if (b && b->priv_) {
        RexiBuffer rb = {.data = b->data, .len = b->len};
        rexi_buffer_free(rb);
        b->data = NULL;
        b->len = 0;
        b->priv_ = NULL;
    }
}
