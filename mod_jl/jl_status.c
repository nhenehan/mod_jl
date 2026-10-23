/* The jl-status handler: the balancers' and workers' state, as HTML, JSON or
 * key=value lines, and commands that change it.
 *
 *   ?cmd=list (default) | show&lb=NAME | show&w=NAME
 *   ?mime=html (default) | json | prop
 *   POST cmd=update lb= w= [activation=A|D|S] [lbfactor=N] [distance=N] nonce=
 *   POST cmd=reset [lb=] [w=] nonce=
 *   POST cmd=recover w= nonce=
 *
 * A command that changes something must be POSTed, carry the nonce (which
 * the page shows, so a cross-site form can't know it) and come to a location
 * with JlStatusReadOnly Off. Who may reach the page at all is up to Require. */
#include "mod_jl.h"

#include "apr_atomic.h"
#include "apr_lib.h"
#include "apr_strings.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_script.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

APLOG_USE_MODULE(jl);

#define FMT_HTML 0
#define FMT_JSON 1
#define FMT_PROP 2

typedef struct {
    request_rec *r;
    apr_table_t *args;
    int fmt;
    int readonly;
    apr_array_header_t *balancers; /* jl_balancer_t*, the JlBalancers by name */
    apr_array_header_t *workers;   /* jl_worker_t*, by name */
    const jl_balancer_t *only_lb;  /* show */
    const jl_worker_t *only_w;
} st_t;

/* ===== Gathering ===== */

static int by_lb_name(const void *a, const void *b)
{
    return strcmp((*(jl_balancer_t *const *)a)->name, (*(jl_balancer_t *const *)b)->name);
}

static int by_w_name(const void *a, const void *b)
{
    return strcmp((*(jl_worker_t *const *)a)->name, (*(jl_worker_t *const *)b)->name);
}

static void gather(st_t *st)
{
    request_rec *r = st->r;
    jl_server_conf *conf = ap_get_module_config(r->server->module_config, &jl_module);
    st->balancers = apr_array_make(r->pool, 8, sizeof(void *));
    st->workers = apr_array_make(r->pool, 8, sizeof(void *));
    for (apr_hash_index_t *hi = apr_hash_first(r->pool, conf->balancers); hi; hi = apr_hash_next(hi))
        *(void **)apr_array_push(st->balancers) = apr_hash_this_val(hi);
    for (apr_hash_index_t *hi = apr_hash_first(r->pool, conf->workers); hi; hi = apr_hash_next(hi))
        *(void **)apr_array_push(st->workers) = apr_hash_this_val(hi);
    qsort(st->balancers->elts, st->balancers->nelts, sizeof(void *), by_lb_name);
    qsort(st->workers->elts, st->workers->nelts, sizeof(void *), by_w_name);
}

static jl_balancer_t *find_lb(st_t *st, const char *name)
{
    jl_server_conf *conf = ap_get_module_config(st->r->server->module_config, &jl_module);
    char *n = apr_pstrdup(st->r->pool, name);
    ap_str_tolower(n);
    return apr_hash_get(conf->balancers, n, APR_HASH_KEY_STRING);
}

static jl_worker_t *find_w(st_t *st, const char *name)
{
    char *n = apr_pstrdup(st->r->pool, name);
    ap_str_tolower(n);
    return jl_find_worker(st->r->server, n);
}

static jl_member_t *find_member(jl_balancer_t *lb, const jl_worker_t *w)
{
    jl_member_t *ms = (jl_member_t *)lb->members->elts;
    for (int i = 0; i < lb->members->nelts; i++)
        if (ms[i].w == w)
            return &ms[i];
    return NULL;
}

static apr_uint32_t state_of(const jl_worker_t *w)
{
    return apr_atomic_read32(&w->shm->state) & 3;
}

static char activation_of(const jl_member_t *m)
{
    return "ADS"[apr_atomic_read32(&m->shm->activation) % 3];
}

/* ===== JSON ===== */

static const char *js(apr_pool_t *p, const char *s)
{
    size_t n = 2;
    for (const char *c = s; *c; c++)
        n += (unsigned char)*c < 0x20 ? 6 : (*c == '"' || *c == '\\') ? 2 : 1;
    char *out = apr_palloc(p, n + 1), *o = out;
    *o++ = '"';
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c < 0x20) {
            apr_snprintf(o, 7, "\\u%04x", *c);
            o += 6;
        } else {
            if (*c == '"' || *c == '\\')
                *o++ = '\\';
            *o++ = (char)*c;
        }
    }
    *o++ = '"';
    *o = '\0';
    return out;
}

static void json_member(st_t *st, const jl_member_t *m, int last)
{
    request_rec *r = st->r;
    ap_rprintf(r,
               "{\"worker\":%s,\"route\":%s,\"activation\":\"%c\",\"lbfactor\":%u,\"distance\":%u,"
               "\"lb_mult\":%" APR_UINT64_T_FMT ",\"lb_value\":%" APR_UINT64_T_FMT ",\"elected\":%" APR_UINT64_T_FMT
               ",\"state\":\"%s\",\"busy\":%u}%s",
               js(r->pool, m->w->name), js(r->pool, m->w->route), activation_of(m),
               apr_atomic_read32(&m->shm->lbfactor), apr_atomic_read32(&m->shm->distance),
               apr_atomic_read64(&m->shm->lb_mult), apr_atomic_read64(&m->shm->lb_value),
               apr_atomic_read64(&m->shm->elected), jl_state_names[state_of(m->w)], apr_atomic_read32(&m->w->shm->busy),
               last ? "" : ",");
}

static void json_balancer(st_t *st, const jl_balancer_t *lb, int last)
{
    request_rec *r = st->r;
    ap_rprintf(r,
               "{\"name\":%s,\"method\":\"%s\",\"sticky_session\":%s,\"sticky_session_force\":%s,"
               "\"session_cookie\":%s,\"recover_time\":%" APR_TIME_T_FMT ",\"retries\":%d,\"members\":[",
               js(r->pool, lb->name), jl_method_names[lb->method], lb->sticky ? "true" : "false",
               lb->sticky_force ? "true" : "false", js(r->pool, lb->session_cookie), apr_time_sec(lb->recover_time),
               lb->retries);
    const jl_member_t *ms = (const jl_member_t *)lb->members->elts;
    for (int i = 0; i < lb->members->nelts; i++)
        json_member(st, &ms[i], i + 1 == lb->members->nelts);
    ap_rprintf(r, "]}%s", last ? "" : ",");
}

static void json_worker(st_t *st, const jl_worker_t *w, int last)
{
    request_rec *r = st->r;
    int size, idle, open;
    jl_pool_stats(w, &size, &idle, &open);
    ap_rprintf(r,
               "{\"name\":%s,\"target\":%s,\"route\":%s,\"state\":\"%s\",\"busy\":%u,\"requests\":%" APR_UINT64_T_FMT
               ",\"errors\":%" APR_UINT64_T_FMT ",\"failovers\":%" APR_UINT64_T_FMT ",\"bytes_out\":%" APR_UINT64_T_FMT
               ",\"reply_timeouts\":%u,\"error_time\":%" APR_UINT64_T_FMT
               ",\"connections\":{\"size\":%d,\"idle\":%d,\"open\":%d}}%s",
               js(r->pool, w->name), js(r->pool, w->target), js(r->pool, w->route), jl_state_names[state_of(w)],
               apr_atomic_read32(&w->shm->busy), apr_atomic_read64(&w->shm->requests),
               apr_atomic_read64(&w->shm->errors), apr_atomic_read64(&w->shm->failovers),
               apr_atomic_read64(&w->shm->bytes_out), apr_atomic_read32(&w->shm->reply_timeouts),
               (apr_uint64_t)apr_time_sec(apr_atomic_read64(&w->shm->error_time)), size, idle, open, last ? "" : ",");
}

static void out_json(st_t *st)
{
    request_rec *r = st->r;
    ap_set_content_type(r, "application/json");
    ap_rprintf(r,
               "{\"server\":\"mod_jl\",\"pid\":%" APR_PID_T_FMT ",\"time\":%" APR_TIME_T_FMT
               ",\"readonly\":%s,\"nonce\":\"%s\",\"balancers\":[",
               getpid(), apr_time_sec(apr_time_now()), st->readonly ? "true" : "false", jl_status_nonce);
    jl_balancer_t **lbs = (jl_balancer_t **)st->balancers->elts;
    int first = 1;
    for (int i = 0; i < st->balancers->nelts; i++) {
        if ((st->only_lb && lbs[i] != st->only_lb) || st->only_w)
            continue;
        if (!first)
            ap_rputs(",", r);
        json_balancer(st, lbs[i], 1);
        first = 0;
    }
    ap_rputs("],\"workers\":[", r);
    jl_worker_t **ws = (jl_worker_t **)st->workers->elts;
    first = 1;
    for (int i = 0; i < st->workers->nelts; i++) {
        if ((st->only_w && ws[i] != st->only_w) || st->only_lb)
            continue;
        if (!first)
            ap_rputs(",", r);
        json_worker(st, ws[i], 1);
        first = 0;
    }
    ap_rputs("]}\n", r);
}

/* ===== key=value ===== */

static void out_prop(st_t *st)
{
    request_rec *r = st->r;
    ap_set_content_type(r, "text/plain; charset=utf-8");
    ap_rprintf(r, "pid=%" APR_PID_T_FMT "\nreadonly=%d\nnonce=%s\n", getpid(), st->readonly, jl_status_nonce);
    jl_balancer_t **lbs = (jl_balancer_t **)st->balancers->elts;
    for (int i = 0; i < st->balancers->nelts && !st->only_w; i++) {
        const jl_balancer_t *lb = lbs[i];
        if (st->only_lb && lb != st->only_lb)
            continue;
        const char *b = lb->name;
        ap_rprintf(r, "balancer.%s.method=%s\nbalancer.%s.sticky_session=%d\nbalancer.%s.sticky_session_force=%d\n", b,
                   jl_method_names[lb->method], b, lb->sticky, b, lb->sticky_force);
        const jl_member_t *ms = (const jl_member_t *)lb->members->elts;
        for (int j = 0; j < lb->members->nelts; j++) {
            const jl_member_t *m = &ms[j];
            const char *p = apr_psprintf(r->pool, "balancer.%s.member.%s", b, m->w->name);
            ap_rprintf(r,
                       "%s.activation=%c\n%s.lbfactor=%u\n%s.distance=%u\n%s.lb_value=%" APR_UINT64_T_FMT
                       "\n%s.elected=%" APR_UINT64_T_FMT "\n%s.state=%s\n",
                       p, activation_of(m), p, apr_atomic_read32(&m->shm->lbfactor), p,
                       apr_atomic_read32(&m->shm->distance), p, apr_atomic_read64(&m->shm->lb_value), p,
                       apr_atomic_read64(&m->shm->elected), p, jl_state_names[state_of(m->w)]);
        }
    }
    jl_worker_t **ws = (jl_worker_t **)st->workers->elts;
    for (int i = 0; i < st->workers->nelts && !st->only_lb; i++) {
        const jl_worker_t *w = ws[i];
        if (st->only_w && w != st->only_w)
            continue;
        int size, idle, open;
        jl_pool_stats(w, &size, &idle, &open);
        const char *p = apr_psprintf(r->pool, "worker.%s", w->name);
        ap_rprintf(r,
                   "%s.target=%s\n%s.route=%s\n%s.state=%s\n%s.busy=%u\n%s.requests=%" APR_UINT64_T_FMT
                   "\n%s.errors=%" APR_UINT64_T_FMT "\n%s.failovers=%" APR_UINT64_T_FMT
                   "\n%s.connections.open=%d\n%s.connections.idle=%d\n",
                   p, w->target, p, w->route, p, jl_state_names[state_of(w)], p, apr_atomic_read32(&w->shm->busy), p,
                   apr_atomic_read64(&w->shm->requests), p, apr_atomic_read64(&w->shm->errors), p,
                   apr_atomic_read64(&w->shm->failovers), p, open, p, idle);
    }
}

/* ===== HTML ===== */

static const char *h(apr_pool_t *p, const char *s)
{
    return ap_escape_html2(p, s, 1);
}

static void form_open(st_t *st, const char *cmd)
{
    ap_rprintf(st->r,
               "<form method=\"post\" action=\"%s\"><input type=\"hidden\" name=\"cmd\" value=\"%s\">"
               "<input type=\"hidden\" name=\"nonce\" value=\"%s\">",
               h(st->r->pool, st->r->uri), cmd, jl_status_nonce);
}

static void out_html(st_t *st)
{
    request_rec *r = st->r;
    ap_set_content_type(r, "text/html; charset=utf-8");
    const char *refresh = apr_table_get(st->args, "refresh");
    int secs = refresh ? atoi(refresh) : 0;
    ap_rputs("<!doctype html>\n<html><head><meta charset=\"utf-8\"><title>jl-status</title>\n", r);
    if (secs > 0)
        ap_rprintf(r, "<meta http-equiv=\"refresh\" content=\"%d\">\n", secs);
    ap_rputs("<style>body{font-family:sans-serif;margin:1.5em}table{border-collapse:collapse;margin-bottom:1.5em}"
             "th,td{border:1px solid #bbb;padding:.25em .6em;text-align:left}th{background:#eee}"
             "td.n{text-align:right}.OK{color:#070}.WARN{color:#a60}.ERROR,.PROBING{color:#b00}"
             "form{display:inline;margin:0}input[type=number]{width:4em}</style></head><body>\n",
             r);
    ap_rprintf(r,
               "<h1>mod_jl status</h1>\n<p>Process %" APR_PID_T_FMT ", %s. <a href=\"?mime=json\">JSON</a> "
               "&middot; <a href=\"?mime=prop\">key=value</a> &middot; <a href=\"?refresh=5\">refresh every 5 s</a>"
               "%s</p>\n",
               getpid(), ap_ht_time(r->pool, apr_time_now(), "%Y-%m-%d %H:%M:%S %Z", 0),
               st->readonly ? " &middot; read-only" : "");

    jl_balancer_t **lbs = (jl_balancer_t **)st->balancers->elts;
    for (int i = 0; i < st->balancers->nelts; i++) {
        const jl_balancer_t *lb = lbs[i];
        ap_rprintf(
            r,
            "<h2>Balancer %s</h2>\n<p>%s; sticky sessions %s%s (%s); recover_time %" APR_TIME_T_FMT
            " s; tries %d workers.</p>\n<table><tr><th>Member</th><th>Route</th><th>Activation</th>"
            "<th>lbfactor</th><th>Distance</th><th>State</th><th>Busy</th><th>Count</th><th>Elected</th>%s</tr>\n",
            h(r->pool, lb->name), jl_method_names[lb->method], lb->sticky ? "on" : "off",
            lb->sticky_force ? ", forced" : "", h(r->pool, lb->session_cookie), apr_time_sec(lb->recover_time),
            lb->retries, st->readonly ? "" : "<th>Change</th>");
        const jl_member_t *ms = (const jl_member_t *)lb->members->elts;
        for (int j = 0; j < lb->members->nelts; j++) {
            const jl_member_t *m = &ms[j];
            const char *state = jl_state_names[state_of(m->w)];
            char act = activation_of(m);
            ap_rprintf(r,
                       "<tr><td>%s</td><td>%s</td><td>%c</td><td class=\"n\">%u</td><td class=\"n\">%u</td>"
                       "<td class=\"%s\">%s</td><td class=\"n\">%u</td><td class=\"n\">%" APR_UINT64_T_FMT
                       "</td><td class=\"n\">%" APR_UINT64_T_FMT "</td>",
                       h(r->pool, m->w->name), h(r->pool, m->w->route), act, apr_atomic_read32(&m->shm->lbfactor),
                       apr_atomic_read32(&m->shm->distance), state, state, apr_atomic_read32(&m->w->shm->busy),
                       apr_atomic_read64(&m->shm->lb_value), apr_atomic_read64(&m->shm->elected));
            if (!st->readonly) {
                ap_rputs("<td>", r);
                form_open(st, "update");
                ap_rprintf(r,
                           "<input type=\"hidden\" name=\"lb\" value=\"%s\"><input type=\"hidden\" name=\"w\" "
                           "value=\"%s\"><select name=\"activation\">",
                           h(r->pool, lb->name), h(r->pool, m->w->name));
                for (const char *a = "ADS"; *a; a++)
                    ap_rprintf(r, "<option%s>%c</option>", *a == act ? " selected" : "", *a);
                ap_rprintf(r,
                           "</select> <input type=\"number\" name=\"lbfactor\" min=\"1\" max=\"100\" value=\"%u\"> "
                           "<input type=\"number\" name=\"distance\" min=\"0\" max=\"100\" value=\"%u\"> "
                           "<button>Update</button></form></td>",
                           apr_atomic_read32(&m->shm->lbfactor), apr_atomic_read32(&m->shm->distance));
            }
            ap_rputs("</tr>\n", r);
        }
        ap_rputs("</table>\n", r);
        if (!st->readonly) {
            form_open(st, "reset");
            ap_rprintf(r, "<input type=\"hidden\" name=\"lb\" value=\"%s\"><button>Reset the counts</button></form>\n",
                       h(r->pool, lb->name));
        }
    }

    ap_rprintf(r,
               "<h2>Workers</h2>\n<table><tr><th>Worker</th><th>Address</th><th>Route</th><th>State</th>"
               "<th>Busy</th><th>Requests</th><th>Errors</th><th>Failovers</th><th>Bytes out</th>"
               "<th>Connections here (open / idle / pool)</th>%s</tr>\n",
               st->readonly ? "" : "<th></th>");
    jl_worker_t **ws = (jl_worker_t **)st->workers->elts;
    for (int i = 0; i < st->workers->nelts; i++) {
        const jl_worker_t *w = ws[i];
        const char *state = jl_state_names[state_of(w)];
        int size, idle, open;
        jl_pool_stats(w, &size, &idle, &open);
        ap_rprintf(r,
                   "<tr><td>%s</td><td>%s</td><td>%s</td><td class=\"%s\">%s</td><td class=\"n\">%u</td>"
                   "<td class=\"n\">%" APR_UINT64_T_FMT "</td><td class=\"n\">%" APR_UINT64_T_FMT
                   "</td><td class=\"n\">%" APR_UINT64_T_FMT "</td><td class=\"n\">%" APR_UINT64_T_FMT
                   "</td><td class=\"n\">%d / %d / %d</td>",
                   h(r->pool, w->name), h(r->pool, w->target), h(r->pool, w->route), state, state,
                   apr_atomic_read32(&w->shm->busy), apr_atomic_read64(&w->shm->requests),
                   apr_atomic_read64(&w->shm->errors), apr_atomic_read64(&w->shm->failovers),
                   apr_atomic_read64(&w->shm->bytes_out), open, idle, size);
        if (!st->readonly) {
            ap_rputs("<td>", r);
            if (state_of(w) == JL_ST_ERROR) {
                form_open(st, "recover");
                ap_rprintf(r, "<input type=\"hidden\" name=\"w\" value=\"%s\"><button>Try it now</button></form> ",
                           h(r->pool, w->name));
            }
            form_open(st, "reset");
            ap_rprintf(r, "<input type=\"hidden\" name=\"w\" value=\"%s\"><button>Reset</button></form></td>",
                       h(r->pool, w->name));
        }
        ap_rputs("</tr>\n", r);
    }
    ap_rputs("</table>\n</body></html>\n", r);
}

/* ===== Commands ===== */

/* A short answer with status; the handler returns OK after it. */
static int answer(request_rec *r, int status, const char *msg)
{
    r->status = status;
    ap_set_content_type(r, "text/plain; charset=utf-8");
    ap_rprintf(r, "%s\n", msg);
    return OK;
}

static int parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    apr_int64_t n = apr_strtoi64(s, &end, 10);
    if (!*s || *end || n < lo || n > hi)
        return 0;
    *out = (int)n;
    return 1;
}

static const char *cmd_update(st_t *st)
{
    apr_table_t *a = st->args;
    const char *lbname = apr_table_get(a, "lb"), *wname = apr_table_get(a, "w");
    jl_balancer_t *lb = lbname ? find_lb(st, lbname) : NULL;
    jl_worker_t *w = wname ? find_w(st, wname) : NULL;
    jl_member_t *m = lb && w ? find_member(lb, w) : NULL;
    if (!m)
        return "update needs lb= (a JlBalancer) and w= (one of its members)";
    const char *act = apr_table_get(a, "activation"), *f = apr_table_get(a, "lbfactor"),
               *d = apr_table_get(a, "distance");
    int activation = -1, lbfactor = -1, distance = -1;
    if (act) {
        const char *p = strchr("ADS", apr_toupper(act[0]));
        if (!act[0] || act[1] || !p)
            return "activation must be A, D or S";
        activation = (int)(p - "ADS");
    }
    if (f && !parse_int(f, 1, 100, &lbfactor))
        return "lbfactor must be 1 to 100";
    if (d && !parse_int(d, 0, 100, &distance))
        return "distance must be 0 to 100";
    if (activation < 0 && lbfactor < 0 && distance < 0)
        return "update needs activation=, lbfactor= or distance=";

    apr_global_mutex_lock(jl_status_mutex);
    if (activation >= 0)
        apr_atomic_set32(&m->shm->activation, (apr_uint32_t)activation);
    if (distance >= 0)
        apr_atomic_set32(&m->shm->distance, (apr_uint32_t)distance);
    if (lbfactor >= 0)
        jl_lb_set_lbfactor(m, lbfactor);
    apr_global_mutex_unlock(jl_status_mutex);
    ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, st->r,
                  "jl-status: %s member %s: activation %c, lbfactor %u, distance %u", lb->name, w->name,
                  activation_of(m), apr_atomic_read32(&m->shm->lbfactor), apr_atomic_read32(&m->shm->distance));
    st->only_lb = lb;
    return NULL;
}

static const char *cmd_reset(st_t *st)
{
    const char *lbname = apr_table_get(st->args, "lb"), *wname = apr_table_get(st->args, "w");
    jl_balancer_t *lb = lbname ? find_lb(st, lbname) : NULL;
    jl_worker_t *w = wname ? find_w(st, wname) : NULL;
    if ((lbname && !lb) || (wname && !w) || (!lb && !w))
        return "reset needs lb= (a JlBalancer) and/or w= (a JlWorker)";
    apr_global_mutex_lock(jl_status_mutex);
    if (lb) {
        jl_member_t *ms = (jl_member_t *)lb->members->elts;
        for (int i = 0; i < lb->members->nelts; i++) {
            apr_atomic_set64(&ms[i].shm->lb_value, 0);
            apr_atomic_set64(&ms[i].shm->elected, 0);
        }
    }
    if (w) {
        jl_shm_worker_t *sh = w->shm;
        apr_atomic_set64(&sh->requests, 0);
        apr_atomic_set64(&sh->errors, 0);
        apr_atomic_set64(&sh->failovers, 0);
        apr_atomic_set64(&sh->bytes_in, 0);
        apr_atomic_set64(&sh->bytes_out, 0);
        apr_atomic_set32(&sh->reply_timeouts, 0);
    }
    apr_global_mutex_unlock(jl_status_mutex);
    ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, st->r, "jl-status: reset %s%s%s", lb ? lb->name : "",
                  lb && w ? " and " : "", w ? w->name : "");
    st->only_lb = lb;
    st->only_w = lb ? NULL : w;
    return NULL;
}

static const char *cmd_recover(st_t *st)
{
    const char *wname = apr_table_get(st->args, "w");
    jl_worker_t *w = wname ? find_w(st, wname) : NULL;
    if (!w)
        return "recover needs w= (a JlWorker)";
    /* Due a probe now: the next request that could use it tries it. */
    if (state_of(w) == JL_ST_ERROR) {
        apr_atomic_set64(&w->shm->error_time, 0);
        ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, st->r, "jl-status: worker %s will be tried by the next request",
                      w->name);
    }
    st->only_w = w;
    return NULL;
}

/* POSTed form fields join the query's (and win). */
static int read_form(st_t *st)
{
    apr_array_header_t *pairs = NULL;
    int rc = ap_parse_form_data(st->r, NULL, &pairs, -1, 16384);
    if (rc != OK)
        return rc;
    while (pairs && !apr_is_empty_array(pairs)) {
        ap_form_pair_t *pair = (ap_form_pair_t *)apr_array_pop(pairs);
        apr_off_t len;
        apr_brigade_length(pair->value, 1, &len);
        apr_size_t size = (apr_size_t)len;
        char *buf = apr_palloc(st->r->pool, size + 1);
        apr_brigade_flatten(pair->value, buf, &size);
        buf[size] = '\0';
        apr_table_set(st->args, pair->name, buf);
    }
    return OK;
}

int jl_status_handler(request_rec *r)
{
    if (!r->handler || strcmp(r->handler, "jl-status") != 0)
        return DECLINED;
    if (r->method_number != M_GET && r->method_number != M_POST) {
        r->allowed = AP_METHOD_BIT << M_GET | AP_METHOD_BIT << M_POST;
        return HTTP_METHOD_NOT_ALLOWED;
    }
    jl_dir_conf *dc = ap_get_module_config(r->per_dir_config, &jl_module);
    st_t st = {.r = r, .readonly = !dc || dc->status_readonly != 0};
    ap_args_to_table(r, &st.args);
    if (r->method_number == M_POST) {
        int rc = read_form(&st);
        if (rc != OK)
            return rc;
    }
    const char *mime = apr_table_get(st.args, "mime");
    st.fmt = !mime || !strcmp(mime, "html") ? FMT_HTML
             : !strcmp(mime, "json")        ? FMT_JSON
             : !strcmp(mime, "prop")        ? FMT_PROP
                                            : -1;
    if (st.fmt < 0)
        return answer(r, HTTP_BAD_REQUEST, "mime must be html, json or prop");
    gather(&st);

    const char *cmd = apr_table_get(st.args, "cmd");
    if (!cmd || !strcmp(cmd, "list")) {
        /* everything */
    } else if (!strcmp(cmd, "show")) {
        const char *lbname = apr_table_get(st.args, "lb"), *wname = apr_table_get(st.args, "w");
        st.only_lb = lbname ? find_lb(&st, lbname) : NULL;
        st.only_w = !lbname && wname ? find_w(&st, wname) : NULL;
        if (!st.only_lb && !st.only_w)
            return answer(r, HTTP_NOT_FOUND, "show needs lb= (a JlBalancer) or w= (a JlWorker)");
    } else if (!strcmp(cmd, "update") || !strcmp(cmd, "reset") || !strcmp(cmd, "recover")) {
        if (r->method_number != M_POST) {
            apr_table_setn(r->err_headers_out, "Allow", "POST");
            return answer(r, HTTP_METHOD_NOT_ALLOWED, "commands that change something must be POSTed");
        }
        if (st.readonly)
            return answer(r, HTTP_FORBIDDEN,
                          "this jl-status location is read-only (JlStatusReadOnly Off allows changes)");
        const char *nonce = apr_table_get(st.args, "nonce");
        if (!nonce || strcmp(nonce, jl_status_nonce) != 0)
            return answer(r, HTTP_FORBIDDEN, "missing or wrong nonce (it is on the page, and in its JSON)");
        const char *err = !strcmp(cmd, "update")  ? cmd_update(&st)
                          : !strcmp(cmd, "reset") ? cmd_reset(&st)
                                                  : cmd_recover(&st);
        if (err)
            return answer(r, HTTP_BAD_REQUEST, err);
        if (st.fmt == FMT_HTML) { /* back to the page */
            apr_table_setn(r->headers_out, "Location", ap_construct_url(r->pool, r->uri, r));
            return HTTP_SEE_OTHER;
        }
    } else {
        return answer(r, HTTP_BAD_REQUEST, "cmd must be list, show, update, reset or recover");
    }

    apr_table_setn(r->headers_out, "Cache-Control", "no-store");
    if (r->header_only)
        return OK;
    if (st.fmt == FMT_JSON)
        out_json(&st);
    else if (st.fmt == FMT_PROP)
        out_prop(&st);
    else
        out_html(&st);
    return OK;
}
