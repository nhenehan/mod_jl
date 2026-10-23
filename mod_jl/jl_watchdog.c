/* The watchdog: every JlWatchdogInterval, one child checks each worker in
 * ERROR on a connection of its own (connect, HELLO, CPing). One that answers
 * is OK again before any request needs it; one that doesn't starts its
 * recover_time again, so requests don't try it either.
 *
 * It runs on mod_watchdog's singleton: in one child at a time. Requests still
 * probe workers on their own; the watchdog only means an idle balancer finds
 * out too. */
#include "mod_jl.h"

#include "apr_atomic.h"
#include "http_log.h"
#include "mod_watchdog.h"

APLOG_USE_MODULE(jl);

static APR_OPTIONAL_FN_TYPE(ap_watchdog_get_instance) *wd_get_instance;
static APR_OPTIONAL_FN_TYPE(ap_watchdog_register_callback) *wd_register_callback;

static apr_status_t check_workers(int state, void *data, apr_pool_t *pool)
{
    server_rec *s = data;
    if (state != AP_WATCHDOG_STATE_RUNNING)
        return APR_SUCCESS;
    jl_server_conf *conf = ap_get_module_config(s->module_config, &jl_module);
    for (apr_hash_index_t *hi = apr_hash_first(pool, conf->workers); hi; hi = apr_hash_next(hi)) {
        jl_worker_t *w = apr_hash_this_val(hi);
        if (apr_atomic_read32(&w->shm->state) != JL_ST_ERROR) /* PROBING belongs to a request */
            continue;
        const char *why = "";
        apr_status_t rv = jl_worker_check(w, s, pool, &why);
        if (rv == APR_SUCCESS) {
            if (jl_lb_worker_recovered(w))
                ap_log_error(APLOG_MARK, APLOG_INFO, 0, s, "jl: watchdog: worker %s is OK again", w->name);
        } else {
            apr_atomic_set64(&w->shm->error_time, (apr_uint64_t)apr_time_now());
            ap_log_error(APLOG_MARK, APLOG_DEBUG, rv, s, "jl: watchdog: worker %s still failing: %s", w->name, why);
        }
    }
    return APR_SUCCESS;
}

int jl_watchdog_init(apr_pool_t *pconf, server_rec *s)
{
    jl_server_conf *conf = ap_get_module_config(s->module_config, &jl_module);
    if (conf->watchdog_interval <= 0)
        return OK;
    wd_get_instance = APR_RETRIEVE_OPTIONAL_FN(ap_watchdog_get_instance);
    wd_register_callback = APR_RETRIEVE_OPTIONAL_FN(ap_watchdog_register_callback);
    if (!wd_get_instance || !wd_register_callback) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s,
                     "JlWatchdogInterval needs mod_watchdog (LoadModule watchdog_module "
                     "modules/mod_watchdog.so)");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    ap_watchdog_t *wd;
    apr_status_t rv = wd_get_instance(&wd, "_jl_", 0, 1, pconf);
    if (rv == APR_SUCCESS)
        rv = wd_register_callback(wd, conf->watchdog_interval, s, check_workers);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, rv, s, "jl: cannot start the watchdog");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, "jl: watchdog every %" APR_TIME_T_FMT " ms",
                 apr_time_as_msec(conf->watchdog_interval));
    return OK;
}
