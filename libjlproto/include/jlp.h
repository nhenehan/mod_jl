/* jlp: the mod_jl wire protocol (version 2). Framing plus HTTP-as-XML messages
 * (schema/jlp2.xsd) as strict, byte-aligned, schema-informed EXI, one document
 * per frame. This library has no I/O of its own: callers own the sockets.
 *
 * The C side plays the web server role: it encodes ForwardRequest and decodes
 * what the worker sends back. Everything is thread-safe once a jlp_ctx exists;
 * the context is shared read-only. */

#ifndef JLP_H
#define JLP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Results ===== */

enum {
    JLP_OK = 0,
    JLP_EINVAL = -1, /* bad argument */
    JLP_ENOMEM = -2,
    JLP_EREXI = -3,   /* the EXI codec failed; see jlp_last_error() */
    JLP_EPROTO = -4,  /* malformed frame or message */
    JLP_ETOOBIG = -5, /* larger than the negotiated max frame */
    JLP_ESCHEMA = -6, /* schema could not be compiled or a template check failed */
    JLP_EABORT = -7   /* a visitor callback returned nonzero */
};

/* A message for the last failed jlp_* call on this thread. Never NULL. */
const char *jlp_last_error(void);
const char *jlp_strerror(int rc);

/* ===== Framing ===== */

#define JLP_MAGIC 0x4A
#define JLP_PROTO_MAJOR 2
#define JLP_PROTO_MINOR 0
#define JLP_FRAME_HDR 12
#define JLP_MAX_FRAME_DEFAULT 65536u
#define JLP_MAX_FRAME_MIN 8192u
#define JLP_MAX_FRAME_MAX (1u << 20)

enum jlp_type {
    JLP_T_HELLO = 0x01,
    JLP_T_HELLO_ACK = 0x02,
    JLP_T_FORWARD_REQUEST = 0x10,
    JLP_T_BODY_CHUNK = 0x11,
    JLP_T_GET_BODY_CHUNK = 0x12,
    JLP_T_SEND_HEADERS = 0x13,
    JLP_T_END_RESPONSE = 0x14,
    JLP_T_CPING = 0x20,
    JLP_T_CPONG = 0x21,
    JLP_T_SHUTDOWN = 0x22,
    JLP_T_ERROR = 0x23
};

enum jlp_flags {
    JLP_F_LAST = 0x01,  /* final request-body chunk */
    JLP_F_FLUSH = 0x02, /* flush the response downstream after this chunk */
    JLP_F_RAW = 0x04    /* reserved; never set */
};

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint32_t req_id;
    uint32_t len; /* payload bytes after the 12-byte header */
} jlp_frame_hdr;

void jlp_hdr_pack(uint8_t out[JLP_FRAME_HDR], const jlp_frame_hdr *h);
/* JLP_EPROTO on bad magic/version, JLP_ETOOBIG if len > max_frame. */
int jlp_hdr_unpack(const uint8_t in[JLP_FRAME_HDR], uint32_t max_frame, jlp_frame_hdr *h);
const char *jlp_type_name(uint8_t type);

/* ===== Handshake (fixed binary, the only non-EXI payloads) ===== */

enum jlp_features {
    JLP_FEAT_BYTE_ALIGNED = 0x0001, /* always set in v2.0 */
    JLP_FEAT_SECRET = 0x0002
};

enum jlp_hello_status {
    JLP_HELLO_OK = 0,
    JLP_HELLO_SCHEMA_MISMATCH = 1,
    JLP_HELLO_BAD_SECRET = 2,
    JLP_HELLO_UNSUPPORTED = 3
};

typedef struct {
    uint16_t minor;
    uint16_t features;
    uint32_t max_frame;
    uint8_t schema_sha256[32];
    const char *secret; /* not NUL-terminated on unpack */
    uint16_t secret_len;
    const char *agent;
    uint16_t agent_len;
} jlp_hello;

typedef struct {
    uint16_t status;
    uint16_t features;
    uint32_t max_frame;
    uint32_t pid;
    const char *name; /* the worker's own name; not NUL-terminated on unpack */
    uint16_t name_len;
} jlp_hello_ack;

/* Pack into out (cap bytes); returns the payload length, or 0 if it does not
 * fit. */
size_t jlp_hello_pack(uint8_t *out, size_t cap, const jlp_hello *h);
size_t jlp_hello_ack_pack(uint8_t *out, size_t cap, const jlp_hello_ack *a);
/* Unpacked string pointers point into p. */
int jlp_hello_unpack(const uint8_t *p, size_t n, jlp_hello *h);
int jlp_hello_ack_unpack(const uint8_t *p, size_t n, jlp_hello_ack *a);

/* ===== Context ===== */

typedef struct jlp_ctx jlp_ctx;

typedef struct {
    /* EXI valueMaxLength: longer strings skip the string table. <0 = unlimited.
     */
    int64_t value_max_length;
} jlp_config;

#define JLP_VALUE_MAX_LENGTH_DEFAULT 128

/* Compiles the embedded schema and derives and checks the byte templates.
 * cfg may be NULL for defaults. One per process; share it between threads. */
int jlp_ctx_create(jlp_ctx **out, const jlp_config *cfg);
void jlp_ctx_destroy(jlp_ctx *ctx);

/* SHA-256 of the embedded jlp2.xsd, for HELLO. */
const uint8_t *jlp_schema_sha256(void);
/* The embedded schema text (NUL-terminated). */
const char *jlp_schema_text(size_t *len);
const char *jlp_rexi_version(void);

/* ===== Encoded output ===== */

typedef struct {
    uint8_t *data;
    size_t len;
    void *priv_; /* owner of data */
} jlp_buf;

void jlp_buf_free(jlp_buf *b);

/* ===== ForwardRequest (encode) =====
 * Every string is NUL-terminated. Strings that are not valid UTF-8 are carried
 * as bytes (header R, UriRaw) or, for attributes, with bad bytes replaced by
 * U+FFFD. Header names are matched case-insensitively against the known list;
 * a match is sent as an ordinal, anything else with its name as given. */

typedef struct {
    const char *name;
    const char *value;
} jlp_header;

typedef struct {
    const char *cipher;         /* optional */
    const char *protocol;       /* optional */
    const char *session_id;     /* optional */
    int key_size;               /* <0: absent */
    const uint8_t *client_cert; /* DER, optional */
    size_t client_cert_len;
    const uint8_t *const *chain; /* DER certificates */
    const size_t *chain_lens;
    size_t n_chain;
} jlp_ssl;

typedef struct {
    const char *method; /* known names go as an ordinal, others as ExtMethod */
    const char *uri;    /* path, %-encoded, without the query; mod_jl sends the
                           normalised path it matched mounts on, escaped again */
    const char *query;  /* optional; without the '?' */
    const jlp_header *headers;
    size_t n_headers;

    uint16_t proto; /* 1000 * major + minor */
    int is_ssl;
    const char *remote_addr;
    uint16_t remote_port;
    const char *local_addr; /* optional */
    int local_port;         /* <0: absent */
    const char *server_name;
    uint16_t server_port;
    const char *auth_type;   /* optional */
    const char *remote_user; /* optional */
    const char *route;       /* optional: the elected member's route */
    int64_t body_length;     /* -1: chunked; JLP_NO_BODY: no body */

    const jlp_ssl *ssl;    /* optional */
    const jlp_header *env; /* JlEnvVar values */
    size_t n_env;

    const uint8_t *body; /* optional first chunk, sent inline */
    size_t body_len;
} jlp_forward_request;

#define JLP_NO_BODY INT64_MIN

int jlp_encode_forward(const jlp_ctx *ctx, const jlp_forward_request *fr, jlp_buf *out);

/* ===== Worker messages (decode) =====
 * Strings passed to callbacks are valid only during the call. Header names for
 * known headers are the canonical lowercase name (ordinal >= 0); for others the
 * name as sent (ordinal -1). raw is 1 when the header came as bytes (R). */

typedef struct {
    int (*status)(void *ud, unsigned status, const char *reason, size_t reason_len);
    int (*header)(void *ud, int ordinal, const char *name, size_t name_len, const char *value, size_t value_len,
                  int raw);
} jlp_headers_visitor;

int jlp_decode_send_headers(const jlp_ctx *ctx, const uint8_t *p, size_t n, const jlp_headers_visitor *v, void *ud);
/* Trailers go to v->header (v may be NULL when trailers are not wanted). */
int jlp_decode_end_response(const jlp_ctx *ctx, const uint8_t *p, size_t n, int *reuse, const jlp_headers_visitor *v,
                            void *ud);
int jlp_decode_shutdown(const jlp_ctx *ctx, const uint8_t *p, size_t n, int *graceful, char *reason, size_t reason_cap);
int jlp_decode_error(const jlp_ctx *ctx, const uint8_t *p, size_t n, unsigned *code, char *message, size_t message_cap);

/* ===== Fixed-template messages (no codec call) ===== */

#define JLP_PREFIX_MAX 16

/* BodyChunk: writes the EXI bytes that come before len bytes of data; returns
 * their count. A frame payload is this prefix followed by the data. */
size_t jlp_body_prefix(const jlp_ctx *ctx, size_t len, uint8_t out[JLP_PREFIX_MAX]);
/* The data of a BodyChunk payload, as a pointer into p. */
int jlp_body_view(const jlp_ctx *ctx, const uint8_t *p, size_t n, const uint8_t **data, size_t *len);
/* Payload bytes of a BodyChunk frame carrying len bytes. */
size_t jlp_body_overhead(const jlp_ctx *ctx, size_t len);

size_t jlp_get_body_encode(const jlp_ctx *ctx, uint32_t max, uint8_t out[JLP_PREFIX_MAX]);
int jlp_get_body_decode(const jlp_ctx *ctx, const uint8_t *p, size_t n, uint32_t *max);

/* Constant documents: JLP_T_CPING, JLP_T_CPONG, and END_RESPONSE with reuse
 * true/false (JLP_T_END_RESPONSE / JLP_CONST_END_NOREUSE). */
#define JLP_CONST_END_NOREUSE 0x114
int jlp_const_doc(const jlp_ctx *ctx, int which, const uint8_t **p, size_t *n);

/* ===== Utilities ===== */

int jlp_utf8_valid(const char *s, size_t n);

#ifdef __cplusplus
}
#endif

#endif
