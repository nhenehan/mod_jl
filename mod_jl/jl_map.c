/* Mounts: which requests go to which worker or balancer. Matched on r->uri, the decoded and
 * normalised path, the same path <Location> sections see. */
#include "mod_jl.h"

#include "apr_strings.h"
#include "http_log.h"

#include <stdlib.h>

APLOG_USE_MODULE(jl);

/* Longest prefix first; at equal length an exact mount before a prefix one. */
static int by_specificity(const void *a, const void *b)
{
    const jl_mount_t *x = a, *y = b;
    if (x->prefix_len != y->prefix_len)
        return x->prefix_len > y->prefix_len ? -1 : 1;
    return x->is_prefix - y->is_prefix;
}

int jl_resolve_mounts(apr_pool_t *p, server_rec *s)
{
    for (server_rec *v = s; v; v = v->next) {
        jl_server_conf *conf = ap_get_module_config(v->module_config, &jl_module);
        jl_mount_t *m = (jl_mount_t *)conf->mounts->elts;
        for (int i = 0; i < conf->mounts->nelts; i++) {
            m[i].target = jl_find_target(s, m[i].target_name);
            if (!m[i].target) {
                ap_log_error(APLOG_MARK, APLOG_EMERG, 0, v, "JlMount %s: no JlWorker or JlBalancer named %s",
                             m[i].pattern, m[i].target_name);
                return HTTP_INTERNAL_SERVER_ERROR;
            }
        }
        qsort(m, conf->mounts->nelts, sizeof *m, by_specificity);
        for (int i = 0; i < conf->mounts->nelts; i++)
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, v, "JlMount %s -> %s%s", m[i].pattern, m[i].target->name,
                         m[i].target->single ? "" : " (balancer)");
    }
    return OK;
}

const jl_mount_t *jl_match_mount(const jl_server_conf *conf, const char *uri)
{
    const jl_mount_t *m = (const jl_mount_t *)conf->mounts->elts;
    for (int i = 0; i < conf->mounts->nelts; i++) {
        if (strncmp(uri, m[i].prefix, m[i].prefix_len) != 0)
            continue;
        char next = uri[m[i].prefix_len];
        // "/app" matches "/app" exactly; "/app/*" also matches "/app/...", but
        // never "/application".
        if (next == '\0' || (m[i].is_prefix && next == '/'))
            return &m[i];
    }
    return NULL;
}
