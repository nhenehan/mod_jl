/* Directives: JlWorker, JlBalancer, JlBalancerMember, JlMount,
 * JlWatchdogInterval and JlStatusReadOnly. */
#include "mod_jl.h"

#include "apr_hash.h"
#include "apr_lib.h"
#include "apr_strings.h"
#include "http_log.h"

APLOG_USE_MODULE(jl);

void *jl_create_server_conf(apr_pool_t *p, server_rec *s)
{
    jl_server_conf *conf = apr_pcalloc(p, sizeof *conf);
    conf->workers = apr_hash_make(p);
    conf->balancers = apr_hash_make(p);
    conf->mounts = apr_array_make(p, 4, sizeof(jl_mount_t));
    return conf;
}

/* Workers and balancers are global (defined at the top level, shared by
 * every vhost); mounts belong to the server they are written in, as with
 * mod_jk. */
void *jl_merge_server_conf(apr_pool_t *p, void *basev, void *addv)
{
    jl_server_conf *base = basev, *add = addv;
    jl_server_conf *conf = apr_pcalloc(p, sizeof *conf);
    conf->workers = base->workers;
    conf->balancers = base->balancers;
    conf->mounts = add->mounts;
    conf->watchdog_interval = base->watchdog_interval;
    return conf;
}

void *jl_create_dir_conf(apr_pool_t *p, char *dir)
{
    jl_dir_conf *dc = apr_pcalloc(p, sizeof *dc);
    dc->status_readonly = -1;
    return dc;
}

void *jl_merge_dir_conf(apr_pool_t *p, void *basev, void *addv)
{
    jl_dir_conf *base = basev, *add = addv;
    jl_dir_conf *dc = apr_pcalloc(p, sizeof *dc);
    dc->status_readonly = add->status_readonly != -1 ? add->status_readonly : base->status_readonly;
    return dc;
}

jl_worker_t *jl_find_worker(server_rec *s, const char *name)
{
    jl_server_conf *conf = ap_get_module_config(s->module_config, &jl_module);
    return apr_hash_get(conf->workers, name, APR_HASH_KEY_STRING);
}

jl_balancer_t *jl_find_target(server_rec *s, const char *name)
{
    jl_server_conf *conf = ap_get_module_config(s->module_config, &jl_module);
    jl_balancer_t *lb = apr_hash_get(conf->balancers, name, APR_HASH_KEY_STRING);
    if (lb)
        return lb;
    jl_worker_t *w = apr_hash_get(conf->workers, name, APR_HASH_KEY_STRING);
    return w ? w->self : NULL;
}

/* ===== Values ===== */
/* who names the directive and its object in messages: "JlWorker j1". */

static const char *set_timeout(cmd_parms *cmd, const char *who, const char *key, const char *val,
                               apr_interval_time_t *out)
{
    /* ap_timeout_parameter_parse takes "soon" for 0: insist on a number. */
    if (!apr_isdigit(val[0]) || ap_timeout_parameter_parse(val, out, "s") != APR_SUCCESS || *out < 0)
        return apr_psprintf(cmd->pool, "%s: bad %s=%s (seconds, or a number with ms/s/mi/h)", who, key, val);
    return NULL;
}

static const char *set_count(cmd_parms *cmd, const char *who, const char *key, const char *val, int lo, int hi,
                             int *out)
{
    char *end;
    apr_int64_t n = apr_strtoi64(val, &end, 10);
    if (!apr_isdigit(val[0]) || *end || n < lo || n > hi)
        return apr_psprintf(cmd->pool, "%s: %s must be a number from %d to %d", who, key, lo, hi);
    *out = (int)n;
    return NULL;
}

static const char *set_flag(cmd_parms *cmd, const char *who, const char *key, const char *val, int *out)
{
    if (!strcasecmp(val, "on") || !strcasecmp(val, "true") || !strcmp(val, "1"))
        *out = 1;
    else if (!strcasecmp(val, "off") || !strcasecmp(val, "false") || !strcmp(val, "0"))
        *out = 0;
    else
        return apr_psprintf(cmd->pool, "%s: %s must be on or off", who, key);
    return NULL;
}

/* One of words[] (case-insensitive): its index. */
static const char *set_word(cmd_parms *cmd, const char *who, const char *key, const char *val, const char *const *words,
                            int *out)
{
    for (int i = 0; words[i]; i++)
        if (!strcasecmp(val, words[i])) {
            *out = i;
            return NULL;
        }
    const char *list = words[0];
    for (int i = 1; words[i]; i++)
        list = apr_pstrcat(cmd->pool, list, words[i + 1] ? ", " : " or ", words[i], NULL);
    return apr_psprintf(cmd->pool, "%s: %s must be %s, got %s", who, key, list, val);
}

/* ping_mode=C|P|I|A, letters combined in any order (CI, PI...), or none. */
static const char *set_ping_mode(cmd_parms *cmd, const char *who, const char *val, int *out)
{
    *out = 0;
    if (!strcasecmp(val, "none"))
        return NULL;
    for (const char *c = val; *c; c++) {
        switch (apr_toupper(*c)) {
        case 'C':
            *out |= JL_PING_CONNECT;
            break;
        case 'P':
            *out |= JL_PING_PREPOST;
            break;
        case 'I':
            *out |= JL_PING_INTERVAL;
            break;
        case 'A':
            *out |= JL_PING_ALL;
            break;
        default:
            return apr_psprintf(cmd->pool, "%s: ping_mode takes the letters C, P, I and A, or none; got %s", who, val);
        }
    }
    if (!*val)
        return apr_psprintf(cmd->pool, "%s: empty ping_mode (use none)", who);
    return NULL;
}

/* fail_on_status=503,-500,...: HTTP statuses, negative for "fail the request,
 * leave the worker alone". */
static const char *set_statuses(cmd_parms *cmd, const char *who, const char *val, apr_array_header_t **out)
{
    *out = apr_array_make(cmd->pool, 4, sizeof(int));
    char *list = apr_pstrdup(cmd->pool, val), *state, *tok;
    for (tok = apr_strtok(list, ",", &state); tok; tok = apr_strtok(NULL, ",", &state)) {
        char *end;
        apr_int64_t n = apr_strtoi64(tok, &end, 10);
        apr_int64_t a = n < 0 ? -n : n;
        if (*end || a < 100 || a > 599)
            return apr_psprintf(cmd->pool, "%s: fail_on_status takes HTTP statuses (100-599, or negative), got %s", who,
                                tok);
        *(int *)apr_array_push(*out) = (int)n;
    }
    if ((*out)->nelts == 0)
        return apr_psprintf(cmd->pool, "%s: empty fail_on_status", who);
    return NULL;
}

/* A name for a worker or a balancer: lowercase (httpd lowercases handler
 * names), unique across both. */
static const char *new_name(cmd_parms *cmd, const char *directive, const char *arg, const char **out)
{
    jl_server_conf *conf = ap_get_module_config(cmd->server->module_config, &jl_module);
    char *name = apr_pstrdup(cmd->pool, arg);
    ap_str_tolower(name);
    if (strchr(name, ':') || strchr(name, '/'))
        return apr_psprintf(cmd->pool, "%s %s: a name may not contain ':' or '/'", directive, arg);
    if (apr_hash_get(conf->workers, name, APR_HASH_KEY_STRING))
        return apr_psprintf(cmd->pool, "%s %s: there is already a JlWorker by that name", directive, name);
    if (apr_hash_get(conf->balancers, name, APR_HASH_KEY_STRING))
        return apr_psprintf(cmd->pool, "%s %s: there is already a JlBalancer by that name", directive, name);
    *out = name;
    return NULL;
}

/* ===== JlWorker ===== */

static const char *parse_target(cmd_parms *cmd, jl_worker_t *w, const char *target)
{
    if (strncmp(target, "unix:", 5) == 0) {
        w->path = target + 5;
        if (w->path[0] != '/')
            return apr_psprintf(cmd->pool, "JlWorker %s: the unix socket path must be absolute: %s", w->name, target);
        if (strlen(w->path) >= 108) /* sizeof sun_path on Linux */
            return apr_psprintf(cmd->pool, "JlWorker %s: unix socket path too long: %s", w->name, target);
        return NULL;
    }
    if (strncmp(target, "tcp://", 6) == 0) {
        char *scope;
        if (apr_parse_addr_port((char **)&w->host, &scope, &w->port, target + 6, cmd->pool) != APR_SUCCESS ||
            w->host == NULL || w->port == 0)
            return apr_psprintf(cmd->pool, "JlWorker %s: expected tcp://host:port, got %s", w->name, target);
        return NULL;
    }
    return apr_psprintf(cmd->pool, "JlWorker %s: the address must be unix:/path or tcp://host:port, got %s", w->name,
                        target);
}

static const char *const recovery_words[] = {"idempotent", "always", "never", NULL};

/* JlWorker NAME unix:/path|tcp://host:port [key=value]... */
static const char *cmd_worker(cmd_parms *cmd, void *dummy, int argc, char *const argv[])
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err)
        return err;
    if (argc < 2)
        return "JlWorker NAME unix:/path|tcp://host:port [secret=S] [route=R] [connect_timeout=T] "
               "[socket_timeout=T] [reply_timeout=T] [max_packet_size=N] [ping_mode=CPIA] [ping_timeout=T] "
               "[connection_ping_interval=T] [connection_pool_size=N] [connection_pool_min=N] "
               "[connection_pool_timeout=T] [connection_acquire_timeout=T] [socket_keepalive=on|off] [retries=N] "
               "[retry_interval=T] [recovery_options=idempotent|always|never] [fail_on_status=S,-S...]";

    jl_server_conf *conf = ap_get_module_config(cmd->server->module_config, &jl_module);
    jl_worker_t *w = apr_pcalloc(cmd->pool, sizeof *w);
    if ((err = new_name(cmd, "JlWorker", argv[0], &w->name)))
        return err;
    const char *who = apr_pstrcat(cmd->pool, "JlWorker ", w->name, NULL);
    w->handler = apr_pstrcat(cmd->pool, "jl:", w->name, NULL);
    w->target = argv[1];
    w->route = w->name;
    w->connect_timeout = apr_time_from_sec(5);
    w->max_frame = JLP_MAX_FRAME_DEFAULT;
    w->ping_mode = JL_PING_CONNECT | JL_PING_INTERVAL;
    w->ping_timeout = apr_time_from_sec(10);
    w->ping_interval = apr_time_from_sec(60);
    w->retries = 2;
    w->retry_interval = apr_time_from_msec(100);
    w->recovery = JL_RECOVER_IDEMPOTENT;
    if ((err = parse_target(cmd, w, argv[1])))
        return err;

    for (int i = 2; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (!eq)
            return apr_psprintf(cmd->pool, "%s: expected key=value, got %s", who, argv[i]);
        const char *key = apr_pstrmemdup(cmd->pool, argv[i], eq - argv[i]), *val = eq + 1;
        if (!strcmp(key, "secret")) {
            w->secret = val;
        } else if (!strcmp(key, "route")) {
            if (!*val || strchr(val, '.') || strchr(val, ';') || strchr(val, ','))
                return apr_psprintf(cmd->pool, "%s: a route may not be empty or contain '.', ';' or ','", who);
            w->route = val;
        } else if (!strcmp(key, "connect_timeout")) {
            err = set_timeout(cmd, who, key, val, &w->connect_timeout);
        } else if (!strcmp(key, "socket_timeout")) {
            err = set_timeout(cmd, who, key, val, &w->socket_timeout);
        } else if (!strcmp(key, "reply_timeout")) {
            err = set_timeout(cmd, who, key, val, &w->reply_timeout);
        } else if (!strcmp(key, "max_packet_size")) {
            int n;
            err = set_count(cmd, who, key, val, JLP_MAX_FRAME_MIN, JLP_MAX_FRAME_MAX, &n);
            w->max_frame = (uint32_t)n;
        } else if (!strcmp(key, "ping_mode")) {
            err = set_ping_mode(cmd, who, val, &w->ping_mode);
        } else if (!strcmp(key, "ping_timeout")) {
            if (!(err = set_timeout(cmd, who, key, val, &w->ping_timeout)) && w->ping_timeout == 0)
                err = apr_psprintf(cmd->pool, "%s: ping_timeout must be more than 0", who);
        } else if (!strcmp(key, "connection_ping_interval")) {
            err = set_timeout(cmd, who, key, val, &w->ping_interval);
        } else if (!strcmp(key, "connection_pool_size")) {
            err = set_count(cmd, who, key, val, 1, 65536, &w->pool_size);
        } else if (!strcmp(key, "connection_pool_min")) {
            err = set_count(cmd, who, key, val, 0, 65536, &w->pool_min);
        } else if (!strcmp(key, "connection_pool_timeout")) {
            err = set_timeout(cmd, who, key, val, &w->pool_timeout);
        } else if (!strcmp(key, "connection_acquire_timeout")) {
            err = set_timeout(cmd, who, key, val, &w->acquire_timeout);
        } else if (!strcmp(key, "socket_keepalive")) {
            err = set_flag(cmd, who, key, val, &w->keepalive);
        } else if (!strcmp(key, "retries")) {
            err = set_count(cmd, who, key, val, 1, 100, &w->retries);
        } else if (!strcmp(key, "retry_interval")) {
            err = set_timeout(cmd, who, key, val, &w->retry_interval);
        } else if (!strcmp(key, "recovery_options")) {
            err = set_word(cmd, who, key, val, recovery_words, &w->recovery);
        } else if (!strcmp(key, "fail_on_status")) {
            err = set_statuses(cmd, who, val, &w->fail_on_status);
        } else {
            err = apr_psprintf(cmd->pool, "%s: unknown setting %s", who, key);
        }
        if (err)
            return err;
    }
    if (w->pool_size && w->pool_min > w->pool_size)
        return apr_psprintf(cmd->pool, "%s: connection_pool_min %d is more than connection_pool_size %d", who,
                            w->pool_min, w->pool_size);
    apr_hash_set(conf->workers, w->name, APR_HASH_KEY_STRING, w);
    return NULL;
}

/* ===== JlBalancer, JlBalancerMember ===== */

static const char *const method_words[] = {"Requests", "Traffic", "Busyness", "Next", NULL};

/* JlBalancer NAME [key=value]... */
static const char *cmd_balancer(cmd_parms *cmd, void *dummy, int argc, char *const argv[])
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err)
        return err;
    if (argc < 1)
        return "JlBalancer NAME [method=Requests|Traffic|Busyness|Next] [sticky_session=on|off] "
               "[sticky_session_force=on|off] [session_cookie=NAME] [session_path=;NAME] [recover_time=T] "
               "[error_escalation_time=T] [max_reply_timeouts=N] [retries=N] [maintain=T]";

    jl_server_conf *conf = ap_get_module_config(cmd->server->module_config, &jl_module);
    jl_balancer_t *lb = apr_pcalloc(cmd->pool, sizeof *lb);
    if ((err = new_name(cmd, "JlBalancer", argv[0], &lb->name)))
        return err;
    const char *who = apr_pstrcat(cmd->pool, "JlBalancer ", lb->name, NULL);
    lb->handler = apr_pstrcat(cmd->pool, "jl:", lb->name, NULL);
    lb->method = JL_LB_REQUESTS;
    lb->sticky = 1;
    lb->session_cookie = "JSESSIONID";
    lb->session_path = ";jsessionid";
    lb->recover_time = apr_time_from_sec(60);
    lb->escalation_time = -1; /* recover_time / 2 */
    lb->maintain = apr_time_from_sec(60);
    lb->retries = 2;
    lb->members = apr_array_make(cmd->pool, 4, sizeof(jl_member_t));

    for (int i = 1; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (!eq)
            return apr_psprintf(cmd->pool, "%s: expected key=value, got %s", who, argv[i]);
        const char *key = apr_pstrmemdup(cmd->pool, argv[i], eq - argv[i]), *val = eq + 1;
        if (!strcmp(key, "method")) {
            err = set_word(cmd, who, key, val, method_words, &lb->method);
        } else if (!strcmp(key, "sticky_session")) {
            err = set_flag(cmd, who, key, val, &lb->sticky);
        } else if (!strcmp(key, "sticky_session_force")) {
            err = set_flag(cmd, who, key, val, &lb->sticky_force);
        } else if (!strcmp(key, "session_cookie")) {
            if (!*val || strpbrk(val, "=; ,"))
                return apr_psprintf(cmd->pool, "%s: bad session_cookie %s", who, val);
            lb->session_cookie = val;
        } else if (!strcmp(key, "session_path")) {
            if (val[0] != ';' || !val[1] || strpbrk(val + 1, "=;/?"))
                return apr_psprintf(cmd->pool, "%s: session_path must look like ;jsessionid, got %s", who, val);
            lb->session_path = val;
        } else if (!strcmp(key, "recover_time")) {
            err = set_timeout(cmd, who, key, val, &lb->recover_time);
        } else if (!strcmp(key, "error_escalation_time")) {
            err = set_timeout(cmd, who, key, val, &lb->escalation_time);
        } else if (!strcmp(key, "max_reply_timeouts")) {
            err = set_count(cmd, who, key, val, 0, 1000000, &lb->max_reply_timeouts);
        } else if (!strcmp(key, "retries")) {
            err = set_count(cmd, who, key, val, 1, 100, &lb->retries);
        } else if (!strcmp(key, "maintain")) {
            if (!(err = set_timeout(cmd, who, key, val, &lb->maintain)) && lb->maintain == 0)
                err = apr_psprintf(cmd->pool, "%s: maintain must be more than 0", who);
        } else {
            err = apr_psprintf(cmd->pool, "%s: unknown setting %s", who, key);
        }
        if (err)
            return err;
    }
    if (lb->escalation_time < 0)
        lb->escalation_time = lb->recover_time / 2;
    apr_hash_set(conf->balancers, lb->name, APR_HASH_KEY_STRING, lb);
    return NULL;
}

static const char *const activation_words[] = {"A", "D", "S", NULL};

/* JlBalancerMember BALANCER WORKER [key=value]... The worker may be defined
 * later; it is resolved in post_config. */
static const char *cmd_member(cmd_parms *cmd, void *dummy, int argc, char *const argv[])
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err)
        return err;
    if (argc < 2)
        return "JlBalancerMember BALANCER WORKER [lbfactor=N] [activation=A|D|S] [distance=N] [redirect=WORKER] "
               "[domain=NAME]";

    jl_server_conf *conf = ap_get_module_config(cmd->server->module_config, &jl_module);
    char *lbname = apr_pstrdup(cmd->pool, argv[0]), *wname = apr_pstrdup(cmd->pool, argv[1]);
    ap_str_tolower(lbname);
    ap_str_tolower(wname);
    jl_balancer_t *lb = apr_hash_get(conf->balancers, lbname, APR_HASH_KEY_STRING);
    if (!lb)
        return apr_psprintf(cmd->pool, "JlBalancerMember %s %s: no JlBalancer %s (define it first)", lbname, wname,
                            lbname);
    const char *who = apr_pstrcat(cmd->pool, "JlBalancerMember ", lbname, " ", wname, NULL);
    jl_member_t *ms = (jl_member_t *)lb->members->elts;
    for (int i = 0; i < lb->members->nelts; i++)
        if (!strcmp(ms[i].worker_name, wname))
            return apr_psprintf(cmd->pool, "%s: already a member", who);

    jl_member_t *m = apr_array_push(lb->members);
    memset(m, 0, sizeof *m);
    m->lb = lb;
    m->worker_name = wname;
    m->lbfactor = 1;
    m->activation = JL_ACT_ACTIVE;
    m->redirect = -1;
    for (int i = 2; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (!eq)
            return apr_psprintf(cmd->pool, "%s: expected key=value, got %s", who, argv[i]);
        const char *key = apr_pstrmemdup(cmd->pool, argv[i], eq - argv[i]), *val = eq + 1;
        if (!strcmp(key, "lbfactor")) {
            err = set_count(cmd, who, key, val, 1, 100, &m->lbfactor);
        } else if (!strcmp(key, "activation")) {
            err = set_word(cmd, who, key, val, activation_words, &m->activation);
        } else if (!strcmp(key, "distance")) {
            err = set_count(cmd, who, key, val, 0, 100, &m->distance);
        } else if (!strcmp(key, "redirect")) {
            char *r = apr_pstrdup(cmd->pool, val);
            ap_str_tolower(r);
            m->redirect_name = r;
        } else if (!strcmp(key, "domain")) {
            m->domain = val;
        } else {
            err = apr_psprintf(cmd->pool, "%s: unknown setting %s", who, key);
        }
        if (err)
            return err;
    }
    return NULL;
}

/* ===== JlMount ===== */

/* JlMount /pattern NAME */
static const char *cmd_mount(cmd_parms *cmd, void *dummy, const char *pattern, const char *name)
{
    const char *err = ap_check_cmd_context(cmd, NOT_IN_DIR_CONTEXT);
    if (err)
        return err;
    if (name == NULL)
        return "JlMount /path[/*] WORKER|BALANCER";
    if (pattern[0] != '/')
        return apr_psprintf(cmd->pool, "JlMount %s: the pattern must start with /", pattern);

    jl_server_conf *conf = ap_get_module_config(cmd->server->module_config, &jl_module);
    jl_mount_t *m = apr_array_push(conf->mounts);
    m->pattern = pattern;
    size_t n = strlen(pattern);
    m->is_prefix = n >= 2 && pattern[n - 1] == '*' && pattern[n - 2] == '/';
    m->prefix = m->is_prefix ? apr_pstrmemdup(cmd->pool, pattern, n - 2) : pattern;
    m->prefix_len = strlen(m->prefix);
    if (strchr(m->prefix, '*'))
        return apr_psprintf(cmd->pool, "JlMount %s: '*' is only allowed as a final /*", pattern);
    char *lname = apr_pstrdup(cmd->pool, name);
    ap_str_tolower(lname);
    m->target_name = lname;
    return NULL;
}

/* JlWatchdogInterval SECONDS */
static const char *cmd_watchdog(cmd_parms *cmd, void *dummy, const char *val)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err)
        return err;
    jl_server_conf *conf = ap_get_module_config(cmd->server->module_config, &jl_module);
    return set_timeout(cmd, "JlWatchdogInterval", "interval", val, &conf->watchdog_interval);
}

/* JlStatusReadOnly On|Off */
static const char *cmd_status_readonly(cmd_parms *cmd, void *dcv, int on)
{
    ((jl_dir_conf *)dcv)->status_readonly = on;
    return NULL;
}

const command_rec jl_cmds[] = {
    AP_INIT_TAKE_ARGV("JlWorker", cmd_worker, NULL, RSRC_CONF,
                      "JlWorker NAME unix:/path|tcp://host:port [key=value]...: a Julia worker"),
    AP_INIT_TAKE_ARGV("JlBalancer", cmd_balancer, NULL, RSRC_CONF,
                      "JlBalancer NAME [key=value]...: share requests between workers"),
    AP_INIT_TAKE_ARGV("JlBalancerMember", cmd_member, NULL, RSRC_CONF,
                      "JlBalancerMember BALANCER WORKER [key=value]...: a worker of a balancer"),
    AP_INIT_TAKE12("JlMount", cmd_mount, NULL, RSRC_CONF,
                   "JlMount /path[/*] NAME: send matching requests to a worker or a balancer"),
    AP_INIT_TAKE1("JlWatchdogInterval", cmd_watchdog, NULL, RSRC_CONF,
                  "JlWatchdogInterval SECONDS: check workers in error this often (0: off)"),
    AP_INIT_FLAG("JlStatusReadOnly", cmd_status_readonly, NULL, OR_ALL,
                 "JlStatusReadOnly On|Off: whether this jl-status location may change anything (default On)"),
    {NULL},
};
