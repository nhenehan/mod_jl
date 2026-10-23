/* Balancing: targets, choosing a member, and what requests learn about
 * workers. The semantics follow mod_jk's lb worker.
 *
 * A mount names a target, which is always a balancer: a JlBalancer, or the
 * balancer of one that each JlWorker gets. Everything a request reads or
 * changes here is in shared memory (jl_shm.c), read and written with atomics
 * and no locks. Two children choosing at the same moment can both pick the
 * same member; that makes the spread a little uneven, never wrong. */
#include "mod_jl.h"

#include "apr_atomic.h"
#include "apr_strings.h"
#include "http_log.h"

#include <string.h>

APLOG_USE_MODULE(jl);

const char *const jl_state_names[] = {"OK", "WARN", "ERROR", "PROBING"};
const char *const jl_method_names[] = {"Requests", "Traffic", "Busyness", "Next"};

/* ===== Targets (post_config) ===== */

static apr_uint64_t gcd(apr_uint64_t a, apr_uint64_t b)
{
    while (b) {
        apr_uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* Each worker's balancer of one, for mounts that name the worker. */
static void make_single(apr_pool_t *p, jl_worker_t *w)
{
    jl_balancer_t *lb = apr_pcalloc(p, sizeof *lb);
    lb->name = w->name;
    lb->handler = w->handler;
    lb->single = 1;
    lb->method = JL_LB_REQUESTS;
    lb->recover_time = apr_time_from_sec(60);
    lb->escalation_time = apr_time_from_sec(30);
    lb->maintain = apr_time_from_sec(60);
    lb->retries = 1;
    lb->members = apr_array_make(p, 1, sizeof(jl_member_t));
    jl_member_t *m = apr_array_push(lb->members);
    memset(m, 0, sizeof *m);
    m->lb = lb;
    m->worker_name = w->name;
    m->w = w;
    m->lbfactor = 1;
    m->lb_mult = 1;
    m->activation = JL_ACT_ACTIVE;
    m->redirect = -1;
    w->self = lb;
    w->memberships = apr_array_make(p, 2, sizeof(void *));
    *(jl_member_t **)apr_array_push(w->memberships) = m;
}

static int resolve_balancer(server_rec *s, jl_balancer_t *lb)
{
    jl_member_t *ms = (jl_member_t *)lb->members->elts;
    int n = lb->members->nelts;
    if (n == 0) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "JlBalancer %s has no JlBalancerMember", lb->name);
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    apr_uint64_t lcm = 1;
    for (int i = 0; i < n; i++) {
        ms[i].w = jl_find_worker(s, ms[i].worker_name);
        if (!ms[i].w) {
            ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "JlBalancerMember %s %s: no JlWorker named %s", lb->name,
                         ms[i].worker_name, ms[i].worker_name);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
        for (int j = 0; j < i; j++)
            if (!strcmp(ms[i].w->route, ms[j].w->route)) {
                ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s,
                             "JlBalancer %s: members %s and %s have the same route %s, so sessions cannot tell them "
                             "apart",
                             lb->name, ms[j].w->name, ms[i].w->name, ms[i].w->route);
                return HTTP_INTERNAL_SERVER_ERROR;
            }
        lcm = lcm / gcd(lcm, (apr_uint64_t)ms[i].lbfactor) * (apr_uint64_t)ms[i].lbfactor;
    }
    for (int i = 0; i < n; i++) {
        *(jl_member_t **)apr_array_push(ms[i].w->memberships) = &ms[i];
        ms[i].lb_mult = lcm / (apr_uint64_t)ms[i].lbfactor;
        if (!ms[i].redirect_name)
            continue;
        for (int j = 0; j < n; j++)
            if (!strcmp(ms[j].worker_name, ms[i].redirect_name))
                ms[i].redirect = j;
        if (ms[i].redirect < 0 || ms[i].redirect == i) {
            ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s,
                         "JlBalancerMember %s %s: redirect=%s must name another member of %s", lb->name,
                         ms[i].worker_name, ms[i].redirect_name, lb->name);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
    }
    for (int i = 0; i < n; i++)
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
                     "JlBalancer %s (%s): member %s route %s, lbfactor %d, activation %c, distance %d%s%s%s%s",
                     lb->name, jl_method_names[lb->method], ms[i].w->name, ms[i].w->route, ms[i].lbfactor,
                     "ADS"[ms[i].activation], ms[i].distance, ms[i].redirect >= 0 ? ", redirect " : "",
                     ms[i].redirect >= 0 ? ms[ms[i].redirect].worker_name : "", ms[i].domain ? ", domain " : "",
                     ms[i].domain ? ms[i].domain : "");
    return OK;
}

int jl_resolve_targets(apr_pool_t *p, server_rec *s)
{
    jl_server_conf *conf = ap_get_module_config(s->module_config, &jl_module);
    for (apr_hash_index_t *hi = apr_hash_first(p, conf->workers); hi; hi = apr_hash_next(hi))
        make_single(p, apr_hash_this_val(hi));
    for (apr_hash_index_t *hi = apr_hash_first(p, conf->balancers); hi; hi = apr_hash_next(hi)) {
        int st = resolve_balancer(s, apr_hash_this_val(hi));
        if (st != OK)
            return st;
    }
    return OK;
}

/* ===== Live settings (jl-status changes them) ===== */

static apr_uint64_t mult(const jl_member_t *m)
{
    return apr_atomic_read64(&m->shm->lb_mult);
}

static int distance(const jl_member_t *m)
{
    return (int)apr_atomic_read32(&m->shm->distance);
}

void jl_lb_set_lbfactor(jl_member_t *m, int lbfactor)
{
    jl_balancer_t *lb = m->lb;
    jl_member_t *ms = (jl_member_t *)lb->members->elts;
    apr_atomic_set32(&m->shm->lbfactor, (apr_uint32_t)lbfactor);
    apr_uint64_t lcm = 1;
    for (int i = 0; i < lb->members->nelts; i++) {
        apr_uint64_t f = apr_atomic_read32(&ms[i].shm->lbfactor);
        lcm = lcm / gcd(lcm, f) * f;
    }
    for (int i = 0; i < lb->members->nelts; i++)
        apr_atomic_set64(&ms[i].shm->lb_mult, lcm / apr_atomic_read32(&ms[i].shm->lbfactor));
}

/* ===== Worker states ===== */

static apr_uint32_t state_of(const jl_member_t *m)
{
    return apr_atomic_read32(&m->w->shm->state);
}

void jl_lb_hard_error(jl_member_t *m, request_rec *r, const char *why)
{
    jl_shm_worker_t *sh = m->w->shm;
    apr_atomic_set64(&sh->error_time, (apr_uint64_t)apr_time_now());
    apr_uint32_t prev = apr_atomic_xchg32(&sh->state, JL_ST_ERROR);
    apr_atomic_inc64(&sh->errors);
    if (prev != JL_ST_ERROR && !m->lb->single)
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r, "jl: worker %s is now in ERROR (was %s): %s", m->w->name,
                      jl_state_names[prev & 3], why);
}

void jl_lb_soft_error(jl_member_t *m, request_rec *r, const char *why)
{
    jl_shm_worker_t *sh = m->w->shm;
    apr_atomic_inc64(&sh->errors);
    apr_time_t now = apr_time_now();
    switch (state_of(m)) {
    case JL_ST_OK:
        if (apr_atomic_cas32(&sh->state, JL_ST_WARN, JL_ST_OK) == JL_ST_OK) {
            apr_atomic_set64(&sh->warn_time, (apr_uint64_t)now);
            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "jl: worker %s is now in WARN: %s", m->w->name, why);
        }
        break;
    case JL_ST_WARN:
        /* Still failing a while after the first failure: give up on it. */
        if (now - (apr_time_t)apr_atomic_read64(&sh->warn_time) >= m->lb->escalation_time)
            jl_lb_hard_error(m, r, apr_pstrcat(r->pool, why, " (failing since error_escalation_time)", NULL));
        break;
    case JL_ST_PROBING:
        jl_lb_hard_error(m, r, apr_pstrcat(r->pool, why, " (while probing it)", NULL));
        break;
    default:
        break;
    }
}

void jl_lb_reply_timeout(jl_member_t *m, request_rec *r)
{
    apr_uint32_t n = apr_atomic_inc32(&m->w->shm->reply_timeouts) + 1;
    if (m->lb->max_reply_timeouts > 0 && n > (apr_uint32_t)m->lb->max_reply_timeouts)
        jl_lb_hard_error(m, r, apr_psprintf(r->pool, "%u reply timeouts, more than max_reply_timeouts", n));
    else
        jl_lb_soft_error(m, r, "reply_timeout");
}

/* A member coming back has fallen behind in lb_value while it was out: start
 * it level with the busiest, or it would take every request until it caught
 * up. */
static void catch_up(jl_member_t *m)
{
    jl_member_t *ms = (jl_member_t *)m->lb->members->elts;
    apr_uint64_t top = 0;
    for (int i = 0; i < m->lb->members->nelts; i++) {
        apr_uint64_t v = apr_atomic_read64(&ms[i].shm->lb_value);
        if (v > top)
            top = v;
    }
    apr_atomic_set64(&m->shm->lb_value, top);
}

/* In every balancer the worker is in. */
static void catch_up_worker(jl_worker_t *w)
{
    jl_member_t **ms = (jl_member_t **)w->memberships->elts;
    for (int i = 0; i < w->memberships->nelts; i++)
        catch_up(ms[i]);
}

int jl_lb_worker_recovered(jl_worker_t *w)
{
    if (apr_atomic_cas32(&w->shm->state, JL_ST_OK, JL_ST_ERROR) != JL_ST_ERROR)
        return 0;
    catch_up_worker(w);
    return 1;
}

void jl_lb_ok(jl_member_t *m, request_rec *r)
{
    jl_shm_worker_t *sh = m->w->shm;
    apr_uint32_t prev = state_of(m);
    if (prev == JL_ST_OK)
        return;
    if (apr_atomic_cas32(&sh->state, JL_ST_OK, prev) != prev)
        return;
    if (prev == JL_ST_PROBING || prev == JL_ST_ERROR)
        catch_up_worker(m->w);
    if (!m->lb->single)
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "jl: worker %s is OK again (was %s)", m->w->name,
                      jl_state_names[prev & 3]);
}

void jl_lb_abandon_probe(jl_member_t *m)
{
    jl_shm_worker_t *sh = m->w->shm;
    if (apr_atomic_cas32(&sh->state, JL_ST_ERROR, JL_ST_PROBING) == JL_ST_PROBING)
        apr_atomic_set64(&sh->error_time, (apr_uint64_t)apr_time_now());
}

void jl_lb_account(jl_member_t *m, apr_off_t bytes)
{
    if (bytes > 0) {
        apr_atomic_add64(&m->w->shm->bytes_out, (apr_uint64_t)bytes);
        if (m->lb->method == JL_LB_TRAFFIC)
            apr_atomic_add64(&m->shm->lb_value, (apr_uint64_t)bytes * mult(m));
    }
}

/* ===== Maintenance ===== */

/* Every `maintain` seconds, one request (whoever wins the CAS) halves the
 * lb_values, so old load fades, and resets the reply timeout counts. */
static void maintain(jl_balancer_t *lb, apr_time_t now)
{
    apr_uint64_t last = apr_atomic_read64(&lb->shm->last_maintain);
    if (last && now - (apr_time_t)last < lb->maintain)
        return;
    if (apr_atomic_cas64(&lb->shm->last_maintain, (apr_uint64_t)now, last) != last || !last)
        return;
    jl_member_t *ms = (jl_member_t *)lb->members->elts;
    for (int i = 0; i < lb->members->nelts; i++) {
        apr_uint64_t v = apr_atomic_read64(&ms[i].shm->lb_value);
        apr_atomic_cas64(&ms[i].shm->lb_value, v / 2, v);
        apr_atomic_set32(&ms[i].w->shm->reply_timeouts, 0);
    }
}

/* ===== Election ===== */

/* Whether m can take this request: *recover says it is in ERROR but due a
 * probe. Disabled members take only sticky requests. */
static int usable(const jl_member_t *m, int sticky, apr_time_t now, int *recover)
{
    *recover = 0;
    apr_uint32_t act = apr_atomic_read32(&m->shm->activation);
    if (act == JL_ACT_STOPPED || (act == JL_ACT_DISABLED && !sticky))
        return 0;
    switch (state_of(m)) {
    case JL_ST_OK:
    case JL_ST_WARN:
        return 1;
    case JL_ST_ERROR:
        *recover = now - (apr_time_t)apr_atomic_read64(&m->w->shm->error_time) >= m->lb->recover_time;
        return *recover;
    default:
        return 0; /* PROBING: someone else's */
    }
}

/* Whether a should be chosen over b. */
static int better(const jl_balancer_t *lb, const jl_member_t *a, const jl_member_t *b)
{
    if (lb->method == JL_LB_BUSYNESS) {
        apr_uint64_t ba = (apr_uint64_t)apr_atomic_read32(&a->w->shm->busy) * mult(a);
        apr_uint64_t bb = (apr_uint64_t)apr_atomic_read32(&b->w->shm->busy) * mult(b);
        if (ba != bb)
            return ba < bb;
    }
    return apr_atomic_read64(&a->shm->lb_value) < apr_atomic_read64(&b->shm->lb_value);
}

/* Sticky sessions: a session id ends in ".route". */
typedef struct {
    jl_balancer_t *lb;
    int found;
} route_search;

static int member_by_session(jl_balancer_t *lb, const char *id, size_t len)
{
    const char *dot = NULL;
    for (size_t i = 0; i < len; i++)
        if (id[i] == '.')
            dot = id + i;
    if (!dot || dot + 1 == id + len)
        return -1;
    size_t rlen = (size_t)(id + len - dot - 1);
    jl_member_t *ms = (jl_member_t *)lb->members->elts;
    for (int i = 0; i < lb->members->nelts; i++)
        if (strlen(ms[i].w->route) == rlen && !memcmp(ms[i].w->route, dot + 1, rlen))
            return i;
    return -1;
}

/* apr_table_do over the Cookie headers: the first session_cookie whose
 * route names a member. */
static int scan_cookie(void *data, const char *key, const char *val)
{
    route_search *rs = data;
    size_t nlen = strlen(rs->lb->session_cookie);
    for (const char *p = val; *p;) {
        while (*p == ' ' || *p == ';' || *p == ',' || *p == '\t')
            p++;
        const char *end = p + strcspn(p, ";,");
        if (!strncmp(p, rs->lb->session_cookie, nlen) && p[nlen] == '=') {
            const char *v = p + nlen + 1;
            size_t vlen = (size_t)(end - v);
            while (vlen && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t'))
                vlen--;
            if (vlen >= 2 && v[0] == '"' && v[vlen - 1] == '"') {
                v++;
                vlen -= 2;
            }
            rs->found = member_by_session(rs->lb, v, vlen);
            if (rs->found >= 0)
                return 0; /* stop */
        }
        p = end;
    }
    return 1;
}

/* The member the request's session is pinned to, or -1: the session_path
 * parameter of the URI first, then the session_cookie cookies in order. */
static int session_member(jl_balancer_t *lb, request_rec *r)
{
    size_t plen = strlen(lb->session_path);
    for (const char *p = r->uri; p && (p = strstr(p, lb->session_path)); p += plen) {
        if (p[plen] != '=')
            continue;
        const char *v = p + plen + 1;
        int i = member_by_session(lb, v, strcspn(v, ";/?"));
        if (i >= 0)
            return i;
    }
    route_search rs = {lb, -1};
    apr_table_do(scan_cookie, &rs, r->headers_in, "Cookie", NULL);
    return rs.found;
}

/* Takes member i: claims the recovery probe if it needs one, and does the
 * accounting. Returns 0 if another request claimed the probe first. */
static int take(jl_balancer_t *lb, int i, int recover, jl_choice_t *out)
{
    jl_member_t *m = &((jl_member_t *)lb->members->elts)[i];
    if (recover) {
        if (apr_atomic_cas32(&m->w->shm->state, JL_ST_PROBING, JL_ST_ERROR) != JL_ST_ERROR)
            return 0;
        out->probe = 1;
    }
    out->m = m;
    apr_atomic_inc64(&m->shm->elected);
    apr_atomic_inc64(&m->w->shm->requests);
    if (lb->method != JL_LB_TRAFFIC)
        apr_atomic_add64(&m->shm->lb_value, mult(m));
    if (lb->method == JL_LB_NEXT)
        apr_atomic_set32(&lb->shm->next_index, (apr_uint32_t)(i + 1));
    return 1;
}

static void trace(jl_balancer_t *lb, request_rec *r, const jl_choice_t *c, const char *how)
{
    if (!APLOG_R_IS_LEVEL(r, APLOG_TRACE1) || lb->single)
        return;
    jl_member_t *ms = (jl_member_t *)lb->members->elts;
    const char *list = "";
    for (int i = 0; i < lb->members->nelts; i++)
        list = apr_psprintf(r->pool, "%s %s:%s/busy=%u/lb=%" APR_UINT64_T_FMT, list, ms[i].w->name,
                            jl_state_names[state_of(&ms[i]) & 3], apr_atomic_read32(&ms[i].w->shm->busy),
                            apr_atomic_read64(&ms[i].shm->lb_value));
    ap_log_rerror(APLOG_MARK, APLOG_TRACE1, 0, r, "jl: %s: elected %s (%s%s);%s", lb->name, c->m->w->name, how,
                  c->probe ? ", probe" : "", list);
}

/* The best usable member among those for which pick(i) holds, not yet tried:
 * lowest distance first, a member due a recovery probe before the others,
 * then by method. */
static int choose(jl_balancer_t *lb, request_rec *r, const unsigned char *tried, int sticky,
                  int (*pick)(jl_balancer_t *, int, const void *), const void *arg, jl_choice_t *out)
{
    jl_member_t *ms = (jl_member_t *)lb->members->elts;
    int n = lb->members->nelts;
    apr_time_t now = apr_time_now();
    for (;;) {
        int best = -1, best_recover = 0, dist = 1 << 30;
        unsigned start = lb->method == JL_LB_NEXT ? apr_atomic_read32(&lb->shm->next_index) : 0;
        for (int k = 0; k < n; k++) {
            int i = (int)((start + (unsigned)k) % (unsigned)n);
            int recover;
            if (tried[i] || (pick && !pick(lb, i, arg)) || !usable(&ms[i], sticky, now, &recover))
                continue;
            int better_choice;
            int d = distance(&ms[i]);
            if (best < 0 || d < dist)
                better_choice = 1;
            else if (d > dist)
                better_choice = 0;
            else if (recover != best_recover)
                better_choice = recover;
            else
                better_choice = better(lb, &ms[i], &ms[best]);
            if (better_choice) {
                best = i;
                best_recover = recover;
                dist = d;
            }
        }
        if (best < 0)
            return 0;
        if (take(lb, best, best_recover, out))
            return 1;
        /* Lost the probe to another request: choose again without it. */
    }
}

static int same_domain(jl_balancer_t *lb, int i, const void *arg)
{
    const jl_member_t *ms = (const jl_member_t *)lb->members->elts;
    const char *domain = arg;
    return ms[i].domain && !strcmp(ms[i].domain, domain);
}

int jl_lb_elect(jl_balancer_t *lb, request_rec *r, const unsigned char *tried, jl_choice_t *out, const char **why)
{
    jl_member_t *ms = (jl_member_t *)lb->members->elts;
    int n = lb->members->nelts;
    apr_time_t now = apr_time_now();
    out->m = NULL;
    out->probe = 0;
    maintain(lb, now);

    /* The session's member, then its redirect, then its domain. */
    if (lb->sticky && n > 1) {
        int si = session_member(lb, r);
        if (si >= 0) {
            int recover;
            if (!tried[si] && usable(&ms[si], 1, now, &recover) && take(lb, si, recover, out)) {
                trace(lb, r, out, "sticky");
                return OK;
            }
            int ri = ms[si].redirect;
            if (ri >= 0 && !tried[ri] && usable(&ms[ri], 1, now, &recover) && take(lb, ri, recover, out)) {
                trace(lb, r, out, "sticky, redirect");
                return OK;
            }
            if (ms[si].domain && choose(lb, r, tried, 0, same_domain, ms[si].domain, out)) {
                trace(lb, r, out, "sticky, domain");
                return OK;
            }
            if (lb->sticky_force) {
                *why = apr_psprintf(r->pool, "the session's worker %s is not available (sticky_session_force)",
                                    ms[si].w->name);
                return HTTP_SERVICE_UNAVAILABLE;
            }
        }
    }

    if (choose(lb, r, tried, 0, NULL, NULL, out)) {
        trace(lb, r, out, "balanced");
        return OK;
    }

    /* Every active member is in ERROR and none is due a probe yet. Rather than
     * refuse everything until recover_time passes, try the one that failed
     * longest ago: a full outage then ends with the first request after it. */
    int oldest = -1;
    apr_uint64_t oldest_time = 0;
    for (int i = 0; i < n; i++) {
        apr_uint32_t st = state_of(&ms[i]);
        if (tried[i] || apr_atomic_read32(&ms[i].shm->activation) != JL_ACT_ACTIVE ||
            (st != JL_ST_ERROR && st != JL_ST_PROBING))
            continue;
        apr_uint64_t t = apr_atomic_read64(&ms[i].w->shm->error_time);
        if (oldest < 0 || t < oldest_time) {
            oldest = i;
            oldest_time = t;
        }
    }
    if (oldest >= 0) {
        out->m = &ms[oldest];
        apr_atomic_inc64(&ms[oldest].w->shm->requests);
        trace(lb, r, out, "all in error");
        return OK;
    }
    *why = "no worker is available";
    return HTTP_SERVICE_UNAVAILABLE;
}
