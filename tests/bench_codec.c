/* Codec micro-benchmarks: ForwardRequest encode for a typical browser GET
 * (12 headers), SendHeaders decode, BodyChunk view. Prints median and p99 in
 * nanoseconds. Build Release for meaningful numbers. */

#include "jlp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ITER 200000

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void report(const char *name, uint64_t *t, size_t n, size_t bytes)
{
    qsort(t, n, sizeof *t, cmp_u64);
    printf("%-28s median %6llu ns  p99 %6llu ns  (%zu bytes)\n", name, (unsigned long long)t[n / 2],
           (unsigned long long)t[n * 99 / 100], bytes);
}

static int nop_status(void *ud, unsigned s, const char *r, size_t n)
{
    return 0;
}
static int nop_header(void *ud, int o, const char *n, size_t nl, const char *v, size_t vl, int raw)
{
    (*(int *)ud)++;
    return 0;
}

int main(void)
{
    jlp_ctx *ctx;
    /* JLP_VML overrides valueMaxLength, to compare string-table settings. */
    jlp_config cfg = {.value_max_length = getenv("JLP_VML") ? atoll(getenv("JLP_VML")) : JLP_VALUE_MAX_LENGTH_DEFAULT};
    printf("valueMaxLength %lld\n", (long long)cfg.value_max_length);
    if (jlp_ctx_create(&ctx, &cfg) != JLP_OK) {
        fprintf(stderr, "%s\n", jlp_last_error());
        return 1;
    }
    uint64_t *t = malloc(ITER * sizeof *t);

    jlp_header h[] = {
        {"Host", "www.example.org"},
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64; rv:131.0) Gecko/20100101 Firefox/131.0"},
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
        {"Accept-Language", "en-US,en;q=0.5"},
        {"Accept-Encoding", "gzip, deflate, br, zstd"},
        {"Connection", "keep-alive"},
        {"Cookie", "JSESSIONID=8f1c2b7a9d4e4f0a.j1; theme=dark; _ga=GA1.2.123456789.1700000000"},
        {"Upgrade-Insecure-Requests", "1"},
        {"Sec-Fetch-Dest", "document"},
        {"Sec-Fetch-Mode", "navigate"},
        {"Sec-Fetch-Site", "none"},
        {"Priority", "u=0, i"},
    };
    jlp_forward_request fr = {
        .method = "GET",
        .uri = "/app/orders/12345/items",
        .query = "page=2&sort=desc",
        .headers = h,
        .n_headers = 12,
        .proto = 1001,
        .remote_addr = "192.168.1.20",
        .remote_port = 53122,
        .local_port = -1,
        .server_name = "www.example.org",
        .server_port = 443,
        .is_ssl = 1,
        .route = "j1",
        .body_length = JLP_NO_BODY,
    };
    size_t raw_head = strlen("GET /app/orders/12345/items?page=2&sort=desc HTTP/1.1\r\n") + 2;
    for (size_t i = 0; i < 12; i++)
        raw_head += strlen(h[i].name) + 2 + strlen(h[i].value) + 2;

    jlp_buf b;
    size_t enc_len = 0;
    for (int i = 0; i < ITER; i++) {
        uint64_t t0 = now_ns();
        jlp_encode_forward(ctx, &fr, &b);
        t[i] = now_ns() - t0;
        enc_len = b.len;
        jlp_buf_free(&b);
    }
    report("encode ForwardRequest", t, ITER, enc_len);
    printf("%-28s %zu raw HTTP/1.1 head bytes -> %zu EXI bytes (%.2fx)\n", "", raw_head, enc_len,
           (double)enc_len / (double)raw_head);

    /* A SendHeaders decode, from the reference vector shape. */
    FILE *f = fopen("testdata/vectors/send_headers.exi", "rb");
    if (f) {
        uint8_t buf[512];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        jlp_headers_visitor v = {.status = nop_status, .header = nop_header};
        int count = 0;
        for (int i = 0; i < ITER; i++) {
            uint64_t t0 = now_ns();
            jlp_decode_send_headers(ctx, buf, n, &v, &count);
            t[i] = now_ns() - t0;
        }
        report("decode SendHeaders", t, ITER, n);
    }

    static uint8_t chunk[65536];
    uint8_t pre[JLP_PREFIX_MAX];
    size_t pn = jlp_body_prefix(ctx, sizeof chunk - 8, pre);
    memcpy(chunk, pre, pn);
    const uint8_t *data;
    size_t len;
    for (int i = 0; i < ITER; i++) {
        uint64_t t0 = now_ns();
        jlp_body_view(ctx, chunk, pn + sizeof chunk - 8, &data, &len);
        t[i] = now_ns() - t0;
    }
    report("BodyChunk view (64 KiB)", t, ITER, pn);

    free(t);
    jlp_ctx_destroy(ctx);
    return 0;
}
