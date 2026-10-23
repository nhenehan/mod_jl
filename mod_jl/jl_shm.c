/* Shared memory: worker health, balancer accounting and member activation,
 * the same in every child process.
 *
 * Three slotmem tables, one slot per worker, per balancer member and per
 * balancer, made in post_config and inherited by the children. Their names
 * carry the config generation: after a graceful restart the new children get
 * new tables, while the old ones finish against theirs, so a busy count is
 * never decremented in a table it was not incremented in.
 *
 * Without mod_slotmem_shm the tables are plain memory in each process. That
 * is enough for single workers (each child keeps its own view of them), so
 * only a JlBalancer insists on shared memory. */
#include "mod_jl.h"

#include "ap_mpm.h"
#include "ap_provider.h"
#include "ap_slotmem.h"
#include "apr_atomic.h"
#include "apr_strings.h"
#include "http_log.h"

#include <string.h>

APLOG_USE_MODULE(jl);

/* Makes n slots of size bytes each and returns their addresses. */
static apr_status_t make_table(const ap_slotmem_provider_t *sm, const char *name, apr_size_t size, unsigned n,
                               apr_pool_t *p, void **slots)
{
    if (!sm) {
        char *mem = apr_pcalloc(p, size * (n ? n : 1));
        for (unsigned i = 0; i < n; i++)
            slots[i] = mem + i * size;
        return APR_SUCCESS;
    }
    ap_slotmem_instance_t *inst;
    apr_status_t rv = sm->create(&inst, name, size, n ? n : 1, 0, p);
    for (unsigned i = 0; rv == APR_SUCCESS && i < n; i++) {
        rv = sm->dptr(inst, i, &slots[i]);
        if (rv == APR_SUCCESS)
            memset(slots[i], 0, size);
    }
    return rv;
}

int jl_shm_init(apr_pool_t *pconf, server_rec *s)
{
    jl_server_conf *conf = ap_get_module_config(s->module_config, &jl_module);
    unsigned n_workers = apr_hash_count(conf->workers);
    unsigned n_explicit = apr_hash_count(conf->balancers);
    unsigned n_lbs = n_workers + n_explicit; /* each worker's balancer of one, then the JlBalancers */

    jl_balancer_t **lbs = apr_palloc(pconf, sizeof *lbs * (n_lbs ? n_lbs : 1));
    jl_worker_t **ws = apr_palloc(pconf, sizeof *ws * (n_workers ? n_workers : 1));
    unsigned i = 0, n_members = 0;
    for (apr_hash_index_t *hi = apr_hash_first(pconf, conf->workers); hi; hi = apr_hash_next(hi), i++) {
        ws[i] = apr_hash_this_val(hi);
        ws[i]->index = (int)i;
        lbs[i] = ws[i]->self;
    }
    for (apr_hash_index_t *hi = apr_hash_first(pconf, conf->balancers); hi; hi = apr_hash_next(hi))
        lbs[i++] = apr_hash_this_val(hi);
    for (i = 0; i < n_lbs; i++) {
        lbs[i]->index = (int)i;
        jl_member_t *ms = (jl_member_t *)lbs[i]->members->elts;
        for (int j = 0; j < lbs[i]->members->nelts; j++)
            ms[j].index = (int)n_members++;
    }

    const ap_slotmem_provider_t *sm = ap_lookup_provider(AP_SLOTMEM_PROVIDER_GROUP, "shm", AP_SLOTMEM_PROVIDER_VERSION);
    if (!sm && n_explicit) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s,
                     "JlBalancer needs shared memory between httpd's processes: load mod_slotmem_shm "
                     "(LoadModule slotmem_shm_module modules/mod_slotmem_shm.so)");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    if (!sm)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, "jl: no mod_slotmem_shm: each process keeps its own worker state");

    int gen = 0;
    ap_mpm_query(AP_MPMQ_GENERATION, &gen);
    void **wslots = apr_palloc(pconf, sizeof(void *) * (n_workers ? n_workers : 1));
    void **mslots = apr_palloc(pconf, sizeof(void *) * (n_members ? n_members : 1));
    void **bslots = apr_palloc(pconf, sizeof(void *) * (n_lbs ? n_lbs : 1));
    static const struct {
        const char *what, *name;
        apr_size_t size;
    } tables[] = {{"workers", "jl_w", sizeof(jl_shm_worker_t)},
                  {"members", "jl_m", sizeof(jl_shm_member_t)},
                  {"balancers", "jl_b", sizeof(jl_shm_balancer_t)}};
    void **slots[] = {wslots, mslots, bslots};
    unsigned counts[] = {n_workers, n_members, n_lbs};
    for (int t = 0; t < 3; t++) {
        apr_status_t rv = make_table(sm, apr_psprintf(pconf, "%s_%d", tables[t].name, gen), tables[t].size,
                                     counts[t], pconf, slots[t]);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_EMERG, rv, s, "jl: cannot make the shared memory for %s", tables[t].what);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    for (i = 0; i < n_workers; i++)
        ws[i]->shm = wslots[i];
    for (i = 0; i < n_lbs; i++) {
        lbs[i]->shm = bslots[i];
        jl_member_t *ms = (jl_member_t *)lbs[i]->members->elts;
        for (int j = 0; j < lbs[i]->members->nelts; j++) {
            ms[j].shm = mslots[ms[j].index];
            apr_atomic_set32(&ms[j].shm->activation, (apr_uint32_t)ms[j].activation);
            apr_atomic_set32(&ms[j].shm->lbfactor, (apr_uint32_t)ms[j].lbfactor);
            apr_atomic_set32(&ms[j].shm->distance, (apr_uint32_t)ms[j].distance);
            apr_atomic_set64(&ms[j].shm->lb_mult, ms[j].lb_mult);
        }
    }
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, "jl: state for %u workers, %u balancers, %u members in %s", n_workers,
                 n_explicit, n_members, sm ? "shared memory" : "process memory");
    return OK;
}
