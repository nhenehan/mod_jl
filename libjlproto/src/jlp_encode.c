/* ForwardRequest encoding. Attributes are written in EXI order (sorted by local
 * name), which for jlp2.xsd is the order they are declared in. */

#include "jlp_internal.h"

#include "jlp_vocab.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    RexiEncoder *enc;
    RexiStatus st;
    const char *where;
    char *scratch; /* lossy UTF-8 copies */
    size_t scratch_cap;
} enc_state;

#define CK(call)                                                                                                       \
    do {                                                                                                               \
        if ((s->st = (call)) != REXI_STATUS_OK) {                                                                      \
            s->where = #call;                                                                                          \
            return -1;                                                                                                 \
        }                                                                                                              \
    } while (0)

/* s as valid UTF-8: itself, or a copy in scratch with each bad byte replaced by
 * U+FFFD. NULL on allocation failure. */
static const char *utf8_or_lossy(enc_state *s, const char *str)
{
    size_t n = strlen(str);
    if (jlp_utf8_valid(str, n))
        return str;
    if (s->scratch_cap < 3 * n + 1) {
        char *p = realloc(s->scratch, 3 * n + 1);
        if (!p)
            return NULL;
        s->scratch = p;
        s->scratch_cap = 3 * n + 1;
    }
    char *o = s->scratch;
    for (size_t i = 0; i < n;) {
        /* Copy the longest valid sequence starting here, else one U+FFFD. */
        size_t k;
        for (k = 4; k > 0; k--)
            if (i + k <= n && jlp_utf8_valid(str + i, k))
                break;
        if (k) {
            memcpy(o, str + i, k);
            o += k;
            i += k;
        } else {
            memcpy(o, "\xEF\xBF\xBD", 3);
            o += 3;
            i++;
        }
    }
    *o = '\0';
    return s->scratch;
}

static int at_string(enc_state *s, const char *name, const char *value)
{
    if (!value)
        return 0;
    const char *v = utf8_or_lossy(s, value);
    if (!v) {
        s->st = REXI_STATUS_ERR_INVALID_ARGUMENT;
        s->where = "out of memory";
        return -1;
    }
    RexiValue rv = {.kind = REXI_VALUE_STRING, .lexical = v};
    CK(rexi_attribute(s->enc, "", name, &rv));
    return 0;
}

static int at_uint(enc_state *s, const char *name, uint64_t value)
{
    RexiValue rv = {.kind = REXI_VALUE_UNSIGNED, .uint_value = value};
    CK(rexi_attribute(s->enc, "", name, &rv));
    return 0;
}

static int el_string(enc_state *s, const char *name, const char *value)
{
    RexiValue rv = {.kind = REXI_VALUE_STRING, .lexical = value};
    CK(rexi_start_element(s->enc, JLP_NS, name));
    CK(rexi_characters(s->enc, &rv));
    CK(rexi_end_element(s->enc));
    return 0;
}

static int el_binary(enc_state *s, const char *name, const uint8_t *p, size_t n)
{
    RexiValue rv = {.kind = REXI_VALUE_BINARY, .bytes = p, .bytes_len = n};
    CK(rexi_start_element(s->enc, JLP_NS, name));
    CK(rexi_characters(s->enc, &rv));
    CK(rexi_end_element(s->enc));
    return 0;
}

static int header(enc_state *s, const jlp_header *h)
{
    size_t nlen = strlen(h->name), vlen = strlen(h->value);
    if (!jlp_utf8_valid(h->name, nlen) || !jlp_utf8_valid(h->value, vlen)) {
        RexiValue n = {.kind = REXI_VALUE_BINARY, .bytes = (const uint8_t *)h->name, .bytes_len = nlen};
        RexiValue v = {.kind = REXI_VALUE_BINARY, .bytes = (const uint8_t *)h->value, .bytes_len = vlen};
        CK(rexi_start_element(s->enc, JLP_NS, "R"));
        CK(rexi_attribute(s->enc, "", "n", &n));
        CK(rexi_characters(s->enc, &v));
        CK(rexi_end_element(s->enc));
        return 0;
    }
    int ord = jlp_header_lookup(h->name, nlen);
    RexiValue v = {.kind = REXI_VALUE_STRING, .lexical = h->value};
    if (ord >= 0) {
        RexiValue n = {.kind = REXI_VALUE_ENUM, .uint_value = (uint64_t)ord};
        CK(rexi_start_element(s->enc, JLP_NS, "H"));
        CK(rexi_attribute(s->enc, "", "n", &n));
    } else {
        RexiValue n = {.kind = REXI_VALUE_STRING, .lexical = h->name};
        CK(rexi_start_element(s->enc, JLP_NS, "X"));
        CK(rexi_attribute(s->enc, "", "n", &n));
    }
    CK(rexi_characters(s->enc, &v));
    CK(rexi_end_element(s->enc));
    return 0;
}

static int ssl(enc_state *s, const jlp_ssl *x)
{
    CK(rexi_start_element(s->enc, JLP_NS, "Ssl"));
    if (at_string(s, "cipher", x->cipher) || (x->key_size >= 0 && at_uint(s, "keySize", (uint64_t)x->key_size)) ||
        at_string(s, "protocol", x->protocol) || at_string(s, "sessionId", x->session_id))
        return -1;
    if (x->client_cert && el_binary(s, "ClientCert", x->client_cert, x->client_cert_len))
        return -1;
    for (size_t i = 0; i < x->n_chain; i++)
        if (el_binary(s, "ChainCert", x->chain[i], x->chain_lens[i]))
            return -1;
    CK(rexi_end_element(s->enc));
    return 0;
}

static int forward(enc_state *s, const jlp_forward_request *fr)
{
    CK(rexi_start_document(s->enc));
    CK(rexi_start_element(s->enc, JLP_NS, "ForwardRequest"));

    /* Attributes, in EXI (alphabetical) order. */
    if (at_string(s, "authType", fr->auth_type))
        return -1;
    if (fr->body_length != JLP_NO_BODY) {
        RexiValue rv = {.kind = REXI_VALUE_INTEGER, .int_value = fr->body_length};
        CK(rexi_attribute(s->enc, "", "bodyLength", &rv));
    }
    RexiValue ssl_flag = {.kind = REXI_VALUE_BOOLEAN, .bool_value = fr->is_ssl != 0};
    CK(rexi_attribute(s->enc, "", "isSsl", &ssl_flag));
    if (at_string(s, "localAddr", fr->local_addr) ||
        (fr->local_port >= 0 && at_uint(s, "localPort", (uint64_t)fr->local_port)) || at_uint(s, "proto", fr->proto) ||
        at_string(s, "remoteAddr", fr->remote_addr ? fr->remote_addr : "") ||
        at_uint(s, "remotePort", fr->remote_port) || at_string(s, "remoteUser", fr->remote_user) ||
        at_string(s, "route", fr->route) || at_string(s, "serverName", fr->server_name ? fr->server_name : "") ||
        at_uint(s, "serverPort", fr->server_port))
        return -1;

    /* Method or ExtMethod. */
    size_t mlen = strlen(fr->method);
    int m = jlp_method_lookup(fr->method, mlen);
    if (m >= 0) {
        RexiValue rv = {.kind = REXI_VALUE_ENUM, .uint_value = (uint64_t)m};
        CK(rexi_start_element(s->enc, JLP_NS, "Method"));
        CK(rexi_characters(s->enc, &rv));
        CK(rexi_end_element(s->enc));
    } else {
        const char *v = utf8_or_lossy(s, fr->method);
        if (!v || el_string(s, "ExtMethod", v))
            return -1;
    }

    /* Uri or UriRaw. */
    size_t ulen = strlen(fr->uri);
    if (jlp_utf8_valid(fr->uri, ulen)) {
        if (el_string(s, "Uri", fr->uri))
            return -1;
    } else if (el_binary(s, "UriRaw", (const uint8_t *)fr->uri, ulen)) {
        return -1;
    }
    if (fr->query) {
        const char *q = utf8_or_lossy(s, fr->query);
        if (!q || el_string(s, "Query", q))
            return -1;
    }

    CK(rexi_start_element(s->enc, JLP_NS, "Headers"));
    for (size_t i = 0; i < fr->n_headers; i++)
        if (header(s, &fr->headers[i]))
            return -1;
    CK(rexi_end_element(s->enc));

    if (fr->ssl && ssl(s, fr->ssl))
        return -1;

    for (size_t i = 0; i < fr->n_env; i++) {
        const char *n = utf8_or_lossy(s, fr->env[i].name);
        if (!n)
            return -1;
        RexiValue nv = {.kind = REXI_VALUE_STRING, .lexical = n};
        CK(rexi_start_element(s->enc, JLP_NS, "Env"));
        CK(rexi_attribute(s->enc, "", "n", &nv));
        const char *v = utf8_or_lossy(s, fr->env[i].value);
        if (!v)
            return -1;
        RexiValue vv = {.kind = REXI_VALUE_STRING, .lexical = v};
        CK(rexi_characters(s->enc, &vv));
        CK(rexi_end_element(s->enc));
    }

    if (fr->body && el_binary(s, "Body", fr->body, fr->body_len))
        return -1;

    CK(rexi_end_element(s->enc));
    CK(rexi_end_document(s->enc));
    return 0;
}

int jlp_encode_forward(const jlp_ctx *ctx, const jlp_forward_request *fr, jlp_buf *out)
{
    if (!ctx || !fr || !out || !fr->method || !fr->uri)
        return jlp_fail(JLP_EINVAL, "jlp_encode_forward: missing argument");
    enc_state s = {0};
    RexiStatus st = rexi_encoder_new(ctx->tables, &ctx->opts, &s.enc);
    if (st != REXI_STATUS_OK)
        return jlp_fail_rexi(st, "rexi_encoder_new");
    if (forward(&s, fr)) {
        rexi_encoder_free(s.enc);
        free(s.scratch);
        return jlp_fail_rexi(s.st, s.where ? s.where : "ForwardRequest");
    }
    free(s.scratch);
    RexiBuffer b;
    if ((st = rexi_encoder_finish(s.enc, &b)) != REXI_STATUS_OK)
        return jlp_fail_rexi(st, "rexi_encoder_finish");
    out->data = b.data;
    out->len = b.len;
    out->priv_ = (void *)1;
    return JLP_OK;
}
