/* Frame headers, the HELLO handshake, error reporting and small helpers. */

#include "jlp_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static _Thread_local char last_error[512];

const char *jlp_last_error(void)
{
    return last_error;
}

int jlp_fail(int rc, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error, sizeof last_error, fmt, ap);
    va_end(ap);
    return rc;
}

int jlp_fail_rexi(RexiStatus st, const char *what)
{
    return jlp_fail(JLP_EREXI, "%s: rexi status %d: %s", what, (int)st, rexi_last_error());
}

const char *jlp_strerror(int rc)
{
    switch (rc) {
    case JLP_OK:
        return "ok";
    case JLP_EINVAL:
        return "invalid argument";
    case JLP_ENOMEM:
        return "out of memory";
    case JLP_EREXI:
        return "EXI codec error";
    case JLP_EPROTO:
        return "protocol error";
    case JLP_ETOOBIG:
        return "frame too large";
    case JLP_ESCHEMA:
        return "schema error";
    case JLP_EABORT:
        return "aborted by callback";
    default:
        return "unknown error";
    }
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void jlp_hdr_pack(uint8_t out[JLP_FRAME_HDR], const jlp_frame_hdr *h)
{
    out[0] = JLP_MAGIC;
    out[1] = JLP_PROTO_MAJOR;
    out[2] = h->type;
    out[3] = h->flags;
    put32(out + 4, h->req_id);
    put32(out + 8, h->len);
}

int jlp_hdr_unpack(const uint8_t in[JLP_FRAME_HDR], uint32_t max_frame, jlp_frame_hdr *h)
{
    if (in[0] != JLP_MAGIC || in[1] != JLP_PROTO_MAJOR)
        return jlp_fail(JLP_EPROTO, "bad frame magic/version %02x %02x", in[0], in[1]);
    h->type = in[2];
    h->flags = in[3];
    h->req_id = get32(in + 4);
    h->len = get32(in + 8);
    if (h->len > max_frame)
        return jlp_fail(JLP_ETOOBIG, "frame of %u bytes exceeds max %u", h->len, max_frame);
    return JLP_OK;
}

const char *jlp_type_name(uint8_t type)
{
    switch (type) {
    case JLP_T_HELLO:
        return "HELLO";
    case JLP_T_HELLO_ACK:
        return "HELLO_ACK";
    case JLP_T_FORWARD_REQUEST:
        return "FORWARD_REQUEST";
    case JLP_T_BODY_CHUNK:
        return "BODY_CHUNK";
    case JLP_T_GET_BODY_CHUNK:
        return "GET_BODY_CHUNK";
    case JLP_T_SEND_HEADERS:
        return "SEND_HEADERS";
    case JLP_T_END_RESPONSE:
        return "END_RESPONSE";
    case JLP_T_CPING:
        return "CPING";
    case JLP_T_CPONG:
        return "CPONG";
    case JLP_T_SHUTDOWN:
        return "SHUTDOWN";
    case JLP_T_ERROR:
        return "ERROR";
    default:
        return "?";
    }
}

/* HELLO: minor u16, features u16, max_frame u32, sha256[32],
 *        secret_len u16, secret, agent_len u16, agent */
size_t jlp_hello_pack(uint8_t *out, size_t cap, const jlp_hello *h)
{
    size_t n = 2 + 2 + 4 + 32 + 2 + h->secret_len + 2 + h->agent_len;
    if (n > cap)
        return 0;
    uint8_t *p = out;
    put16(p, h->minor);
    put16(p + 2, h->features);
    put32(p + 4, h->max_frame);
    memcpy(p + 8, h->schema_sha256, 32);
    p += 40;
    put16(p, h->secret_len);
    if (h->secret_len)
        memcpy(p + 2, h->secret, h->secret_len);
    p += 2 + h->secret_len;
    put16(p, h->agent_len);
    if (h->agent_len)
        memcpy(p + 2, h->agent, h->agent_len);
    return n;
}

int jlp_hello_unpack(const uint8_t *p, size_t n, jlp_hello *h)
{
    if (n < 42)
        return jlp_fail(JLP_EPROTO, "HELLO too short");
    h->minor = get16(p);
    h->features = get16(p + 2);
    h->max_frame = get32(p + 4);
    memcpy(h->schema_sha256, p + 8, 32);
    size_t off = 40;
    h->secret_len = get16(p + off);
    if (off + 2 + h->secret_len + 2 > n)
        return jlp_fail(JLP_EPROTO, "HELLO secret truncated");
    h->secret = (const char *)p + off + 2;
    off += 2 + h->secret_len;
    h->agent_len = get16(p + off);
    if (off + 2 + h->agent_len != n)
        return jlp_fail(JLP_EPROTO, "HELLO agent length mismatch");
    h->agent = (const char *)p + off + 2;
    return JLP_OK;
}

/* HELLO_ACK: status u16, features u16, max_frame u32, pid u32, name_len u16,
 * name */
size_t jlp_hello_ack_pack(uint8_t *out, size_t cap, const jlp_hello_ack *a)
{
    size_t n = 2 + 2 + 4 + 4 + 2 + a->name_len;
    if (n > cap)
        return 0;
    put16(out, a->status);
    put16(out + 2, a->features);
    put32(out + 4, a->max_frame);
    put32(out + 8, a->pid);
    put16(out + 12, a->name_len);
    if (a->name_len)
        memcpy(out + 14, a->name, a->name_len);
    return n;
}

int jlp_hello_ack_unpack(const uint8_t *p, size_t n, jlp_hello_ack *a)
{
    if (n < 14)
        return jlp_fail(JLP_EPROTO, "HELLO_ACK too short");
    a->status = get16(p);
    a->features = get16(p + 2);
    a->max_frame = get32(p + 4);
    a->pid = get32(p + 8);
    a->name_len = get16(p + 12);
    if (14 + (size_t)a->name_len != n)
        return jlp_fail(JLP_EPROTO, "HELLO_ACK name length mismatch");
    a->name = (const char *)p + 14;
    return JLP_OK;
}

size_t jlp_uvarint_put(uint8_t *out, uint64_t v)
{
    size_t i = 0;
    while (v >= 0x80) {
        out[i++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    out[i++] = (uint8_t)v;
    return i;
}

size_t jlp_uvarint_get(const uint8_t *p, size_t n, uint64_t *v)
{
    uint64_t x = 0;
    for (size_t i = 0; i < n && i < 10; i++) {
        x |= (uint64_t)(p[i] & 0x7f) << (7 * i);
        if (!(p[i] & 0x80)) {
            *v = x;
            return i + 1;
        }
    }
    return 0;
}

/* Strict UTF-8 (RFC 3629): no overlongs, no surrogates, nothing above U+10FFFF.
 */
int jlp_utf8_valid(const char *str, size_t n)
{
    const uint8_t *s = (const uint8_t *)str;
    size_t i = 0;
    while (i < n) {
        /* Fast path for ASCII runs, eight bytes at a time. */
        while (i + 8 <= n) {
            uint64_t w;
            memcpy(&w, s + i, 8);
            if (w & 0x8080808080808080ull)
                break;
            i += 8;
        }
        if (i >= n)
            break;
        uint8_t c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t len;
        uint8_t lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            if (c == 0xE0)
                lo = 0xA0;
            else if (c == 0xED)
                hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            if (c == 0xF0)
                lo = 0x90;
            else if (c == 0xF4)
                hi = 0x8F;
        } else {
            return 0;
        }
        if (i + len > n)
            return 0;
        if (s[i + 1] < lo || s[i + 1] > hi)
            return 0;
        for (size_t k = 2; k < len; k++)
            if ((s[i + k] & 0xC0) != 0x80)
                return 0;
        i += len;
    }
    return 1;
}
