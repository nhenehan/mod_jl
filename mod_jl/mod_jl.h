/* mod_jl: forwards requests to Julia workers (JlServ) over jlp v2. A tribute
 * to mod_jk: the directives, the balancer and its states, sticky routes and
 * CPing all follow it, and where mod_jk speaks AJP, mod_jl speaks AJP's shape
 * in schema-informed EXI.
 *
 * A mount names a target: a JlBalancer, or a single JlWorker (a balancer of
 * one). Each worker has a pool of connections per child process, checked
 * before use (CPing). Worker health and balancing state live in shared
 * memory (jl_shm.c), so every child sees the same. */
#ifndef MOD_JL_H
#define MOD_JL_H

#include "jlp.h"

#include "apr_global_mutex.h"
#include "apr_hash.h"
#include "apr_network_io.h"
#include "apr_tables.h"
#include "apr_thread_cond.h"
#include "apr_thread_mutex.h"
#include "httpd.h"
#include "http_config.h"

/* ping_mode letters. */
#define JL_PING_CONNECT 0x1  /* C: HELLO_ACK must come within ping_timeout */
#define JL_PING_PREPOST 0x2  /* P: CPing before every request */
#define JL_PING_INTERVAL 0x4 /* I: CPing a connection idle longer than connection_ping_interval */
#define JL_PING_ALL (JL_PING_CONNECT | JL_PING_PREPOST | JL_PING_INTERVAL)

typedef struct jl_pool jl_pool_t;

/* Worker states, in shared memory. */
#define JL_ST_OK 0
#define JL_ST_WARN 1    /* failed recently; ERROR if it fails again after error_escalation_time */
#define JL_ST_ERROR 2   /* skipped until recover_time has passed */
#define JL_ST_PROBING 3 /* one request is trying it after recover_time */

/* Member activation. */
#define JL_ACT_ACTIVE 0   /* A */
#define JL_ACT_DISABLED 1 /* D: only requests sticky to it */
#define JL_ACT_STOPPED 2  /* S: nothing */

/* recovery_options: may a request the worker received go to another? */
#define JL_RECOVER_IDEMPOTENT 0 /* only GET, HEAD, PUT, DELETE, OPTIONS, TRACE */
#define JL_RECOVER_ALWAYS 1
#define JL_RECOVER_NEVER 2 /* only requests that never reached a worker */

/* Shared memory: one per worker, one per balancer member, one per balancer.
 * Every field is read and written with apr_atomic_*. */
typedef struct {
    volatile apr_uint32_t state;
    volatile apr_uint32_t busy;
    volatile apr_uint32_t reply_timeouts; /* since the last maintenance */
    volatile apr_uint32_t pad;
    volatile apr_uint64_t error_time; /* apr_time_t of the move to ERROR */
    volatile apr_uint64_t warn_time;  /* ... and to WARN */
    volatile apr_uint64_t requests, errors, failovers, bytes_in, bytes_out;
} jl_shm_worker_t;

/* The member settings jl-status can change live here; the config only seeds
 * them. */
typedef struct {
    volatile apr_uint64_t lb_value;
    volatile apr_uint64_t elected;
    volatile apr_uint64_t lb_mult;    /* lcm of the balancer's lbfactors / lbfactor */
    volatile apr_uint32_t activation; /* JL_ACT_* */
    volatile apr_uint32_t lbfactor;
    volatile apr_uint32_t distance;
    volatile apr_uint32_t pad;
} jl_shm_member_t;

typedef struct {
    volatile apr_uint64_t last_maintain;
    volatile apr_uint32_t next_index; /* method Next */
    volatile apr_uint32_t pad;
} jl_shm_balancer_t;

/* One JlWorker. Parsed in jl_conf.c, its address resolved in post_config. */
typedef struct {
    const char *name;    /* lowercase */
    const char *handler; /* "jl:" name, what r->handler holds for it */
    const char *target;  /* as configured: unix:/path or tcp://host:port */
    const char *path;    /* unix socket path, or NULL */
    const char *host;    /* tcp host, or NULL */
    apr_port_t port;
    apr_sockaddr_t *addr; /* resolved in post_config, in pconf */
    const char *secret;   /* or NULL */
    apr_interval_time_t connect_timeout;
    apr_interval_time_t socket_timeout; /* 0: the server's Timeout */
    uint32_t max_frame;
    int keepalive; /* SO_KEEPALIVE, TCP only */

    int ping_mode; /* JL_PING_* */
    apr_interval_time_t ping_timeout;
    apr_interval_time_t ping_interval;

    int pool_size; /* 0: one per thread of the child */
    int pool_min;  /* connections the idle sweep leaves open */
    apr_interval_time_t pool_timeout;    /* close idle connections after this; 0: never */
    apr_interval_time_t acquire_timeout; /* wait this long for a free one; 0: as long as it takes */

    const char *route; /* the session id suffix that pins a session to it */
    int retries;       /* connect attempts when it refuses connections */
    apr_interval_time_t retry_interval;
    apr_interval_time_t reply_timeout; /* longest wait for a frame once the request is sent; 0: socket_timeout */
    int recovery;                      /* JL_RECOVER_* */
    apr_array_header_t *fail_on_status; /* int: >0 fail over and mark the worker; <0 only fail the request */

    struct jl_balancer *self; /* the balancer of one that a mount naming this worker uses */
    apr_array_header_t *memberships; /* void*: every jl_member_t of this worker, self's first */
    int index;                /* its shared memory slot */
    jl_shm_worker_t *shm;
    jl_pool_t *pool; /* made in child_init: each child has its own */
} jl_worker_t;

typedef struct jl_balancer jl_balancer_t;

/* One JlBalancerMember. */
typedef struct {
    jl_balancer_t *lb;
    const char *worker_name;
    jl_worker_t *w;
    /* As configured; the live values are in shm. */
    int lbfactor;
    apr_uint64_t lb_mult; /* lcm of the balancer's lbfactors / lbfactor */
    int activation;
    int distance;
    const char *redirect_name;
    int redirect; /* index into lb->members, or -1 */
    const char *domain;
    int index; /* its shared memory slot */
    jl_shm_member_t *shm;
} jl_member_t;

#define JL_LB_REQUESTS 0
#define JL_LB_TRAFFIC 1
#define JL_LB_BUSYNESS 2
#define JL_LB_NEXT 3

/* A JlBalancer, or a single worker as a balancer of one (single). What
 * mounts and handlers name. */
struct jl_balancer {
    const char *name;    /* lowercase */
    const char *handler; /* "jl:" name */
    int single;
    int method;
    int sticky, sticky_force;
    const char *session_cookie;
    const char *session_path; /* ";jsessionid" */
    apr_interval_time_t recover_time;
    apr_interval_time_t escalation_time;
    apr_interval_time_t maintain;
    int max_reply_timeouts; /* 0: a reply timeout never marks a worker ERROR by itself */
    int retries;            /* members one request may try */
    apr_array_header_t *members; /* jl_member_t */
    int index;
    jl_shm_balancer_t *shm;
};

// One JlMount: "/x" is exact, "/x/*" is "/x" and everything below it.
typedef struct {
    const char *pattern; /* as configured */
    const char *prefix;  // the pattern without its trailing "/*"
    size_t prefix_len;
    int is_prefix;
    const char *target_name; /* resolved in post_config */
    jl_balancer_t *target;
} jl_mount_t;

typedef struct {
    apr_hash_t *workers;        /* name -> jl_worker_t*; the main server's, shared by vhosts */
    apr_hash_t *balancers;      /* name -> jl_balancer_t*, likewise */
    apr_array_header_t *mounts; /* jl_mount_t, per server; longest prefix first after post_config */
    apr_interval_time_t watchdog_interval; /* JlWatchdogInterval; 0: no watchdog */
} jl_server_conf;

typedef struct {
    int status_readonly; /* JlStatusReadOnly; -1: not set (on) */
} jl_dir_conf;

extern module AP_MODULE_DECLARE_DATA jl_module;

/* The codec context, made in post_config and inherited by the children. */
extern jlp_ctx *jl_ctx;

/* jl-status changes take this (made in post_config); the nonce they must
 * carry is the same in every child. */
extern apr_global_mutex_t *jl_status_mutex;
extern const char *jl_status_nonce;

/* jl_conf.c */
void *jl_create_server_conf(apr_pool_t *p, server_rec *s);
void *jl_merge_server_conf(apr_pool_t *p, void *base, void *add);
void *jl_create_dir_conf(apr_pool_t *p, char *dir);
void *jl_merge_dir_conf(apr_pool_t *p, void *base, void *add);
extern const command_rec jl_cmds[];
jl_worker_t *jl_find_worker(server_rec *s, const char *name);
/* A balancer by name, or a worker's balancer of one; NULL if neither. */
jl_balancer_t *jl_find_target(server_rec *s, const char *name);

/* jl_lb.c: targets, election, worker states. */
int jl_resolve_targets(apr_pool_t *p, server_rec *s); /* post_config; OK or an error */

typedef struct {
    jl_member_t *m;
    int probe; /* this request claimed the recovery probe (ERROR -> PROBING) */
} jl_choice_t;

/* Chooses a member not yet in tried[] (one flag per member). Returns OK, or
 * HTTP_SERVICE_UNAVAILABLE when there is none (*why says why). */
int jl_lb_elect(jl_balancer_t *lb, request_rec *r, const unsigned char *tried, jl_choice_t *out, const char **why);
/* After the request: Traffic accounting. */
void jl_lb_account(jl_member_t *m, apr_off_t bytes);
/* What a request found out about a worker. */
void jl_lb_ok(jl_member_t *m, request_rec *r);
void jl_lb_hard_error(jl_member_t *m, request_rec *r, const char *why);
void jl_lb_soft_error(jl_member_t *m, request_rec *r, const char *why);
void jl_lb_reply_timeout(jl_member_t *m, request_rec *r);
/* A probe that ended without a verdict goes back to ERROR. */
void jl_lb_abandon_probe(jl_member_t *m);
/* ERROR -> OK from outside a request (the watchdog); 1 if it changed. */
int jl_lb_worker_recovered(jl_worker_t *w);
/* lbfactor at runtime; recomputes lb_mult over the balancer. Callers hold
 * the jl-status mutex. */
void jl_lb_set_lbfactor(jl_member_t *m, int lbfactor);
extern const char *const jl_state_names[]; /* by JL_ST_* */
extern const char *const jl_method_names[]; /* by JL_LB_* */

/* jl_shm.c: shared memory for workers, members and balancers. */
int jl_shm_init(apr_pool_t *pconf, server_rec *s); /* post_config; OK or an error */

/* jl_map.c */
int jl_resolve_mounts(apr_pool_t *p, server_rec *s); /* sorts mounts and links their targets; OK or an error */
const jl_mount_t *jl_match_mount(const jl_server_conf *conf, const char *uri);

/* jl_io.c: frames over one worker socket. */
typedef struct {
    apr_socket_t *sock;
    uint8_t *buf; /* read-ahead: buf[start, end) is unread */
    size_t cap, start, end;
    uint32_t max_frame;
    apr_interval_time_t timeout;
} jl_io_t;

#define JL_IO_EPROTO (APR_OS_START_USERERR + 1) /* a malformed frame */

void jl_io_init(jl_io_t *io, apr_socket_t *sock, apr_pool_t *p, uint32_t max_frame, apr_interval_time_t timeout);
/* Reads one frame. The payload points into io's buffer and stays valid until
 * the next read. */
apr_status_t jl_io_read_frame(jl_io_t *io, jlp_frame_hdr *h, const uint8_t **payload);
/* Whether a whole frame header is already buffered, or bytes are waiting on
 * the socket, without blocking. */
int jl_io_ready(jl_io_t *io);
/* Sends one frame: the header, then the payload pieces in order. */
apr_status_t jl_io_send(jl_io_t *io, uint8_t type, uint8_t flags, uint32_t req_id, const struct iovec *parts,
                        int n_parts);

/* Reads one frame, waiting at most timeout for it. */
apr_status_t jl_io_read_frame_within(jl_io_t *io, jlp_frame_hdr *h, const uint8_t **payload,
                                     apr_interval_time_t timeout);

/* jl_worker.c: the pool. A slot is taken by one request at a time; its
 * socket is opened when needed and kept between requests. */
typedef struct {
    jl_pool_t *owner; /* NULL for the watchdog's own connections */
    const jl_worker_t *w;
    apr_pool_t *pool;  /* the slot's own (unmanaged), made on first connect */
    apr_pool_t *spool; /* the socket's: cleared when it closes */
    apr_socket_t *sock; /* NULL: not connected */
    jl_io_t io;
    apr_time_t last_used;
    uint32_t next_req_id;
    int reused; /* this connection has served a request before */
    uint32_t worker_pid;
} jl_conn_t;

#define JL_ACQUIRE_FRESH 0x1 /* close a pooled connection and open a new one */
#define JL_EPOOL (APR_OS_START_USERERR + 2) /* every connection of the pool is in use */

/* Takes a slot, drops its connection if it went stale, then connects (up to
 * retries times while nothing listens) or CPings as ping_mode says. On
 * failure *why says what went wrong and the slot is back in the pool;
 * JL_EPOOL means no slot was free, anything else that the worker failed. */
apr_status_t jl_conn_acquire(const jl_worker_t *w, request_rec *r, int flags, jl_conn_t **out, const char **why);
/* Puts the slot back; keep says whether the connection is fit for another
 * request, otherwise it is closed. */
void jl_conn_release(jl_conn_t *c, int keep);
/* A new request id for the next exchange on c (never 0, which SHUTDOWN uses). */
uint32_t jl_conn_next_id(jl_conn_t *c);
/* child_init: a pool for every worker. */
apr_status_t jl_pools_init(apr_pool_t *pchild, server_rec *s);
/* This process's pool: its size, idle slots and open connections. */
void jl_pool_stats(const jl_worker_t *w, int *size, int *idle, int *open);
/* Connect, HELLO and CPing on a connection of its own, then close it: is the
 * worker answering? */
apr_status_t jl_worker_check(const jl_worker_t *w, server_rec *s, apr_pool_t *p, const char **why);

/* jl_request.c */
int jl_handler(request_rec *r);

/* jl_status.c: the jl-status handler. */
int jl_status_handler(request_rec *r);

/* jl_watchdog.c: post_config; OK or an error. */
int jl_watchdog_init(apr_pool_t *pconf, server_rec *s);

#endif
