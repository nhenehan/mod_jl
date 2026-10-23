#ifndef JLP_INTERNAL_H
#define JLP_INTERNAL_H

#include "jlp.h"
#include "rexi.h"

#include <stdbool.h>

#define JLP_NS "urn:jl:proto:2"

/* A document that is a fixed prefix followed by an unsigned varint (and, for
 * BodyChunk, that many raw bytes). Derived from rexi output at context
 * creation, never hard-coded. */
typedef struct {
    uint8_t bytes[JLP_PREFIX_MAX];
    uint8_t len;
} jlp_template;

typedef struct {
    uint8_t *data;
    size_t len;
} jlp_const;

struct jlp_ctx {
    RexiTables *tables;
    RexiOptions opts;
    jlp_template body;     /* BodyChunk: prefix, uvarint(len), bytes */
    jlp_template get_body; /* GetBodyChunk: prefix, uvarint(max) */
    jlp_const cping, cpong, end_reuse, end_noreuse;
};

extern const char jlp_schema_xsd[];
extern const size_t jlp_schema_xsd_len;
extern const uint8_t jlp_schema_xsd_sha256[32];

/* Records a message for jlp_last_error() and returns rc. */
int jlp_fail(int rc, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Records rexi_last_error() for status st and returns JLP_EREXI. */
int jlp_fail_rexi(RexiStatus st, const char *what);

size_t jlp_uvarint_put(uint8_t *out, uint64_t v);
/* Bytes read, or 0 if the varint is truncated or longer than 10 bytes. */
size_t jlp_uvarint_get(const uint8_t *p, size_t n, uint64_t *v);

#endif
