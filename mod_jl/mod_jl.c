/* mod_jl: the module and its hooks. */
#include "mod_jl.h"

#include "apr_hash.h"
#include "apr_strings.h"
#include "http_core.h"
#include "http_log.h"
#include "http_request.h"
#include "util_mutex.h"

#include <stdlib.h>

/* No APLOG_USE_MODULE here: AP_DECLARE_MODULE below does the same. */

jlp_ctx *jl_ctx;
apr_global_mutex_t *jl_status_mutex;
const char *jl_status_nonce;

static apr_status_t destroy_ctx(void *data)
{
    jlp_ctx_destroy(data);
    if (jl_ctx == data)
        jl_ctx = NULL;
    return APR_SUCCESS;
}

static int resolve_workers(apr_pool_t *p, server_rec *s)
{
    jl_server_conf *conf = ap_get_module_config(s->module_config, &jl_module);
    for (apr_hash_index_t *hi = apr_hash_first(p, conf->workers); hi; hi = apr_hash_next(hi)) {
        jl_worker_t *w = apr_hash_this_val(hi);
        apr_status_t rv = w->path ? apr_sockaddr_info_get(&w->addr, w->path, APR_UNIX, 0, 0, p)
                                  : apr_sockaddr_info_get(&w->addr, w->host, APR_UNSPEC, w->port, 0, p);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_EMERG, rv, s, "JlWorker %s: cannot resolve %s", w->name, w->target);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, "JlWorker %s: %s, max_packet_size %u", w->name, w->target,
                     w->max_frame);
    }
    return OK;
}

static int jl_post_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s)
{
    /* httpd reads the config twice at startup; the first pass is a dry run. */
    if (ap_state_query(AP_SQ_MAIN_STATE) == AP_SQ_MS_CREATE_PRE_CONFIG)
        return OK;
    jlp_ctx *ctx;
    int rc = jlp_ctx_create(&ctx, NULL);
    if (rc != JLP_OK) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "jl: cannot set up the codec: %s", jlp_last_error());
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    jl_ctx = ctx;
    apr_pool_cleanup_register(pconf, ctx, destroy_ctx, apr_pool_cleanup_null);
    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s, "jl: jlp v2, rexi %s", jlp_rexi_version());
    int st = resolve_workers(pconf, s);
    if (st == OK)
        st = jl_resolve_targets(pconf, s);
    if (st == OK)
        st = jl_shm_init(pconf, s);
    if (st == OK)
        st = jl_resolve_mounts(pconf, s);
    if (st == OK)
        st = jl_watchdog_init(pconf, s);
    if (st != OK)
        return st;

    unsigned char rnd[16];
    apr_status_t rv = apr_generate_random_bytes(rnd, sizeof rnd);
    if (rv == APR_SUCCESS)
        rv = ap_global_mutex_create(&jl_status_mutex, NULL, "jl-status", NULL, s, pconf, 0);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, rv, s, "jl: cannot set up jl-status");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    char *hex = apr_palloc(pconf, 2 * sizeof rnd + 1);
    for (size_t i = 0; i < sizeof rnd; i++)
        apr_snprintf(hex + 2 * i, 3, "%02x", rnd[i]);
    jl_status_nonce = hex;
    return OK;
}

/* Each child keeps its own connections to the workers. */
static int jl_pre_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp)
{
    return ap_mutex_register(pconf, "jl-status", NULL, APR_LOCK_DEFAULT, 0) == APR_SUCCESS ? OK
                                                                                       : HTTP_INTERNAL_SERVER_ERROR;
}

static void jl_child_init(apr_pool_t *pchild, server_rec *s)
{
    if (jl_pools_init(pchild, s) != APR_SUCCESS)
        exit(APEXIT_CHILDSICK);
    if (jl_status_mutex &&
        apr_global_mutex_child_init(&jl_status_mutex, apr_global_mutex_lockfile(jl_status_mutex), pchild) !=
            APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s, "jl: cannot reopen the jl-status mutex");
        exit(APEXIT_CHILDSICK);
    }
}

/* JlMount: claim the request for its target. Runs after mod_rewrite, so a
 * rewritten URI is what is matched. */
static int jl_translate_name(request_rec *r)
{
    if (r->proxyreq || !r->uri || r->uri[0] != '/')
        return DECLINED;
    jl_server_conf *conf = ap_get_module_config(r->server->module_config, &jl_module);
    const jl_mount_t *m = jl_match_mount(conf, r->uri);
    if (!m)
        return DECLINED;
    r->handler = m->target->handler;
    ap_set_module_config(r->request_config, &jl_module, m->target);
    return DECLINED; /* let the rest of translation run; the handler is chosen */
}

/* A mounted URI is not a file: skip the directory walk (as mod_proxy does). */
static int jl_map_to_storage(request_rec *r)
{
    return ap_get_module_config(r->request_config, &jl_module) ? OK : DECLINED;
}

/* Keep AddHandler (by the file extension) from taking a mounted request;
 * SetHandler still wins, as it is applied later. */
static int jl_type_checker(request_rec *r)
{
    jl_balancer_t *t = ap_get_module_config(r->request_config, &jl_module);
    if (!t)
        return DECLINED;
    r->handler = t->handler;
    return OK;
}

static void jl_register_hooks(apr_pool_t *p)
{
    static const char *const after_rewrite[] = {"mod_rewrite.c", NULL};
    ap_hook_pre_config(jl_pre_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_config(jl_post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(jl_child_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_translate_name(jl_translate_name, after_rewrite, NULL, APR_HOOK_FIRST);
    ap_hook_map_to_storage(jl_map_to_storage, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_type_checker(jl_type_checker, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_handler(jl_handler, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(jl_status_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

/* Built with -fvisibility=hidden; the module record is the one symbol httpd
 * needs to see. */
#pragma GCC visibility push(default)
AP_DECLARE_MODULE(jl) = {
    STANDARD20_MODULE_STUFF,
    jl_create_dir_conf,    /* per-directory config */
    jl_merge_dir_conf,     /* merge per-directory */
    jl_create_server_conf, /* per-server config */
    jl_merge_server_conf,  /* merge per-server */
    jl_cmds,               /* directives */
    jl_register_hooks,
    AP_MODULE_FLAG_NONE,
};
#pragma GCC visibility pop
