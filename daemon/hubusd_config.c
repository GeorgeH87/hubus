#include "hubusd_config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "hubusd_yaml.h"

#define MODE_MAIN 0 /* the main file */
#define MODE_FILE 1 /* the runtime file: unknown keys are warnings, settings are read */
#define MODE_MQTT 2 /* a change over MQTT: unknown keys are errors, settings belong to the modules and are kept */

#define WARNING_LEN sizeof(((hubusd_config_t *)0)->warnings[0])

/*
 * `cfg`, the whole configuration, only for the main file. The runtime part has `rt` alone: what reads it cannot reach GPIO,
 * timing, the broker, or the modules.
 */
typedef struct {
    hubusd_config_t *cfg;
    hubusd_runtime_cfg_t *rt;
    char (*warnings)[WARNING_LEN]; /* NULL: none are kept (MODE_MQTT turns them into errors) */
    unsigned *n_warnings;
    char *err;
    size_t errsz;
    int failed;
    int mode;
} ctx_t;

static void fail(ctx_t *c, int line, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (c->failed) {
        return;
    }
    c->failed = 1;
    n = snprintf(c->err, c->errsz, "line %d: ", line);
    if (n > 0 && (size_t)n < c->errsz) {
        va_start(ap, fmt);
        vsnprintf(c->err + n, c->errsz - (size_t)n, fmt, ap);
        va_end(ap);
    }
}

static void warn(ctx_t *c, int line, const char *fmt, ...)
{
    va_list ap;
    char *w;
    int n;

    if (!c->warnings || *c->n_warnings >= HUBUSD_MAX_WARNINGS) {
        return;
    }
    w = c->warnings[(*c->n_warnings)++];
    n = snprintf(w, WARNING_LEN, "line %d: ", line);
    if (n > 0 && (size_t)n < WARNING_LEN) {
        va_start(ap, fmt);
        vsnprintf(w + n, WARNING_LEN - (size_t)n, fmt, ap);
        va_end(ap);
    }
}

/* ---- typed readers ------------------------------------------------------- */

static yn_t *section(ctx_t *c, yn_t *map, const char *key)
{
    yn_t *n = yn_get(map, key);

    if (n && n->type != YN_MAP) {
        fail(c, n->line, "'%s' must be a mapping", key);
        return NULL;
    }
    return n;
}

static yn_t *scalar(ctx_t *c, yn_t *map, const char *key)
{
    yn_t *n = yn_get(map, key);

    if (n && n->type != YN_SCALAR) {
        fail(c, n->line, "'%s' must be a single value", key);
        return NULL;
    }
    return n;
}

static void read_str(ctx_t *c, yn_t *map, const char *key, char *dst, size_t size)
{
    yn_t *n = scalar(c, map, key);

    if (!n) {
        return;
    }
    if (strlen(n->text) >= size) {
        fail(c, n->line, "'%s' is too long (maximum %zu characters)", key, size - 1u);
        return;
    }
    strcpy(dst, n->text);
}

static int parse_long(const char *s, long *out)
{
    char *end;
    long v;

    if (!*s) {
        return 0;
    }
    errno = 0;
    v = strtol(s, &end, 0);
    if (errno || *end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

static void read_int(ctx_t *c, yn_t *map, const char *key, long min, long max, long *out)
{
    yn_t *n = scalar(c, map, key);
    long v;

    if (!n) {
        return;
    }
    if (!parse_long(n->text, &v)) {
        fail(c, n->line, "'%s' must be a number, got '%s'", key, n->text);
        return;
    }
    if (v < min || v > max) {
        fail(c, n->line, "'%s' must be between %ld and %ld, got %ld", key, min, max, v);
        return;
    }
    *out = v;
}

static void read_unsigned(ctx_t *c, yn_t *map, const char *key, long min, long max, unsigned *out)
{
    long v = (long)*out;

    read_int(c, map, key, min, max, &v);
    *out = (unsigned)v;
}

static void read_bool(ctx_t *c, yn_t *map, const char *key, int *out)
{
    yn_t *n = scalar(c, map, key);

    if (!n) {
        return;
    }
    if (strcasecmp(n->text, "true") == 0 || strcasecmp(n->text, "yes") == 0 || strcasecmp(n->text, "on") == 0) {
        *out = 1;
    } else if (strcasecmp(n->text, "false") == 0 || strcasecmp(n->text, "no") == 0 || strcasecmp(n->text, "off") == 0) {
        *out = 0;
    } else {
        fail(c, n->line, "'%s' must be true or false, got '%s'", key, n->text);
    }
}

/* readback: slave | cache | none (true = slave, false = none); for a slave also default, the value of the bus section */
static void read_readback(ctx_t *c, yn_t *map, const char *key, int *out, int for_slave)
{
    yn_t *n = scalar(c, map, key);

    if (!n) {
        return;
    }
    if (for_slave && strcasecmp(n->text, "default") == 0) {
        *out = -1;
    } else if (strcasecmp(n->text, "slave") == 0 || strcasecmp(n->text, "true") == 0 || strcasecmp(n->text, "yes") == 0 ||
        strcasecmp(n->text, "on") == 0) {
        *out = HUBUSD_READBACK_SLAVE;
    } else if (strcasecmp(n->text, "cache") == 0) {
        *out = HUBUSD_READBACK_CACHE;
    } else if (strcasecmp(n->text, "none") == 0 || strcasecmp(n->text, "false") == 0 || strcasecmp(n->text, "no") == 0 ||
               strcasecmp(n->text, "off") == 0) {
        *out = HUBUSD_READBACK_NONE;
    } else {
        fail(c, n->line, "%s must be slave, cache, %snone, got %c%s%c", key, for_slave ? "default, or " : "or ", 39,
             n->text, 39);
    }
}

/* Unknown keys: warnings in a file, errors over MQTT, where nobody reads warnings. */
static void warn_unknown(ctx_t *c, yn_t *map, const char *where)
{
    unsigned i;

    if (!map) {
        return;
    }
    for (i = 0; i < map->n; i++) {
        if (!map->used[i] && c->mode == MODE_MQTT) {
            fail(c, map->key_line[i], "unknown key '%s' in '%s'", map->key[i], where);
        } else if (!map->used[i] && (strcmp(map->key[i], "profile") == 0 || strcmp(map->key[i], "profiles") == 0)) {
            warn(c, map->key_line[i], "'%s' in '%s' is no longer supported and ignored: the entities of a slave come from its module",
                 map->key[i], where);
        } else if (!map->used[i]) {
            warn(c, map->key_line[i], "unknown key '%s' in '%s' is ignored", map->key[i], where);
        }
    }
}

/* ---- defaults ------------------------------------------------------------- */

void hubusd_config_defaults(hubusd_config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->gpio.clk_line = 17;
    cfg->gpio.data_line = 27;
    hubus_master_cfg_default(&cfg->master);
    snprintf(cfg->mqtt.host, sizeof cfg->mqtt.host, "localhost");
    cfg->mqtt.port = 1883;
    snprintf(cfg->mqtt.base_topic, sizeof cfg->mqtt.base_topic, "hubus");
    snprintf(cfg->mqtt.discovery_prefix, sizeof cfg->mqtt.discovery_prefix, "homeassistant");
    snprintf(cfg->mqtt.client_id, sizeof cfg->mqtt.client_id, "hubusd");
    cfg->rt.mqtt.discovery = 1;
    cfg->rt.mqtt.stats_interval_s = 60;
    cfg->rt.bus.readback = 1;
    cfg->rt.bus.ping_interval_s = 30;
    cfg->rt.bus.ping_failures = 3;
    cfg->rt.bus.scan_interval_s = 300;
    cfg->rt.bus.startup_retry_s = 10;
    cfg->modules.call_timeout_ms = 1000;
    snprintf(cfg->runtime_file, sizeof cfg->runtime_file, "%s", HUBUSD_RUNTIME_FILE_DEFAULT);
}

/* In the runtime part: a key that only the main file may have is an error. Only its name is known here; its value is not read. */
static int main_only(ctx_t *c, yn_t *map, const char *key, const char *where)
{
    yn_t *n = yn_get(map, key);

    if (!n) {
        return 0;
    }
    fail(c, n->line, "'%s%s%s' can only be set in the main configuration file", where, where[0] ? "." : "", key);
    return 1;
}

/* ---- sections -------------------------------------------------------------- */

static void load_gpio(ctx_t *c, yn_t *root)
{
    hubusd_config_t *cfg = c->cfg;
    yn_t *s = section(c, root, "gpio");
    long rt = cfg->gpio.realtime;

    if (!s) {
        return;
    }
    read_str(c, s, "chip", cfg->gpio.chip, sizeof cfg->gpio.chip);
    read_str(c, s, "path", cfg->gpio.path, sizeof cfg->gpio.path);
    read_unsigned(c, s, "clk", 0, 1000, &cfg->gpio.clk_line);
    read_unsigned(c, s, "data", 0, 1000, &cfg->gpio.data_line);
    read_int(c, s, "realtime", 0, 99, &rt);
    cfg->gpio.realtime = (int)rt;
    read_bool(c, s, "internal_pullup", &cfg->gpio.internal_pullup);
    if (!c->failed && cfg->gpio.clk_line == cfg->gpio.data_line) {
        fail(c, s->line, "gpio.clk and gpio.data must be different lines");
    }
    warn_unknown(c, s, "gpio");
}

/* A time in microseconds, stored in nanoseconds: up to 1 s, so that it fits the uint32_t of the master. */
static void read_us(ctx_t *c, yn_t *s, const char *key, uint32_t *ns)
{
    unsigned us = *ns / 1000u;

    read_unsigned(c, s, key, 1, 1000000, &us);
    *ns = us * 1000u;
}

static void load_timing(ctx_t *c, yn_t *root)
{
    hubus_master_cfg_t *m = &c->cfg->master;
    yn_t *s = section(c, root, "timing");
    unsigned v;
    int b;

    if (!s) {
        return;
    }
    read_us(c, s, "clk_low_us", &m->t_clk_low_ns);
    read_us(c, s, "clk_high_us", &m->t_clk_high_ns);
    read_us(c, s, "sample_us", &m->t_sample_ns);
    read_us(c, s, "cond_us", &m->t_cond_ns);
    read_us(c, s, "turnaround_us", &m->t_turnaround_ns);
    v = m->t_class_margin_ns / 1000u;
    read_unsigned(c, s, "class_margin_us", 0, 1000000, &v);
    m->t_class_margin_ns = v * 1000u;
    v = m->line_fault_ticks;
    read_unsigned(c, s, "line_fault_ticks", 280, 65000, &v);
    m->line_fault_ticks = (uint16_t)v;
    v = m->retry_budget;
    read_unsigned(c, s, "retry_budget", 0, 20, &v);
    m->retry_budget = (uint8_t)v;
    b = m->follow_more;
    read_bool(c, s, "follow_fragments", &b);
    m->follow_more = (uint8_t)b;
    v = m->max_fragments;
    read_unsigned(c, s, "max_fragments", 1, 255, &v);
    m->max_fragments = (uint8_t)v;
    v = m->dup_fail_threshold;
    read_unsigned(c, s, "duplicate_threshold", 1, 100, &v);
    m->dup_fail_threshold = (uint8_t)v;
    v = m->line_problem_count;
    read_unsigned(c, s, "line_problem_count", 1, 100, &v);
    m->line_problem_count = (uint8_t)v;
    if (!c->failed && m->t_sample_ns > m->t_clk_high_ns) {
        fail(c, s->line, "timing.sample_us must not exceed timing.clk_high_us");
    }
    warn_unknown(c, s, "timing");
}

static int valid_topic_part(const char *t)
{
    size_t n = strlen(t);

    return n > 0 && t[0] != '/' && t[n - 1] != '/' && !strpbrk(t, "+#");
}

static void load_password_file(ctx_t *c, const char *path, int line)
{
    FILE *f = fopen(path, "r");
    char *nl;

    if (!f) {
        fail(c, line, "cannot read password_file '%s': %s", path, strerror(errno));
        return;
    }
    if (!fgets(c->cfg->mqtt.password, sizeof c->cfg->mqtt.password, f)) {
        c->cfg->mqtt.password[0] = '\0';
    }
    fclose(f);
    nl = strpbrk(c->cfg->mqtt.password, "\r\n");
    if (nl) {
        *nl = '\0';
    }
}

/* The keys of the mqtt section in the runtime part. */
static void load_mqtt_runtime(ctx_t *c, yn_t *s)
{
    read_bool(c, s, "discovery", &c->rt->mqtt.discovery);
    read_bool(c, s, "discovery_cleanup", &c->rt->mqtt.discovery_cleanup);
    read_unsigned(c, s, "stats_interval_s", 0, 86400, &c->rt->mqtt.stats_interval_s);
}

/* The mqtt section of the main file: the broker, and the start values of the runtime part. */
static void load_mqtt(ctx_t *c, yn_t *root)
{
    hubusd_config_t *cfg = c->cfg;
    yn_t *s = section(c, root, "mqtt");
    char pwfile[200] = "";
    long port = cfg->mqtt.port;
    yn_t *pf;

    if (!s) {
        return;
    }
    read_str(c, s, "host", cfg->mqtt.host, sizeof cfg->mqtt.host);
    read_int(c, s, "port", 1, 65535, &port);
    cfg->mqtt.port = (int)port;
    read_str(c, s, "username", cfg->mqtt.username, sizeof cfg->mqtt.username);
    read_str(c, s, "password", cfg->mqtt.password, sizeof cfg->mqtt.password);
    read_str(c, s, "password_file", pwfile, sizeof pwfile);
    read_bool(c, s, "tls", &cfg->mqtt.tls);
    read_str(c, s, "cafile", cfg->mqtt.cafile, sizeof cfg->mqtt.cafile);
    read_str(c, s, "base_topic", cfg->mqtt.base_topic, sizeof cfg->mqtt.base_topic);
    read_str(c, s, "discovery_prefix", cfg->mqtt.discovery_prefix, sizeof cfg->mqtt.discovery_prefix);
    read_str(c, s, "client_id", cfg->mqtt.client_id, sizeof cfg->mqtt.client_id);
    load_mqtt_runtime(c, s);
    pf = yn_get(s, "password_file");
    if (!c->failed && pwfile[0]) {
        load_password_file(c, pwfile, pf ? pf->line : s->line);
    }
    if (!c->failed && !valid_topic_part(cfg->mqtt.base_topic)) {
        fail(c, s->line, "mqtt.base_topic must not be empty, start or end with '/', or contain '+' or '#'");
    }
    if (!c->failed && !valid_topic_part(cfg->mqtt.discovery_prefix)) {
        fail(c, s->line, "mqtt.discovery_prefix must not be empty, start or end with '/', or contain '+' or '#'");
    }
    if (!c->failed && cfg->mqtt.tls && cfg->mqtt.port == 1883) {
        warn(c, s->line, "TLS is enabled but the port is 1883 (usually 8883)");
    }
    warn_unknown(c, s, "mqtt");
}

static void load_bus(ctx_t *c, yn_t *root)
{
    hubusd_runtime_cfg_t *rt = c->rt;
    yn_t *s = section(c, root, "bus");

    if (!s) {
        return;
    }
    read_readback(c, s, "readback", &rt->bus.readback, 0);
    read_unsigned(c, s, "ping_interval_s", 0, 86400, &rt->bus.ping_interval_s);
    read_unsigned(c, s, "ping_failures", 1, 100, &rt->bus.ping_failures);
    read_unsigned(c, s, "scan_interval_s", 0, 86400, &rt->bus.scan_interval_s);
    read_unsigned(c, s, "startup_retry_s", 1, 3600, &rt->bus.startup_retry_s);
    warn_unknown(c, s, "bus");
}

static void load_modules(ctx_t *c, yn_t *root)
{
    hubusd_config_t *cfg = c->cfg;
    yn_t *s = section(c, root, "modules");

    if (!s) {
        return;
    }
    read_str(c, s, "dir", cfg->modules.dir, sizeof cfg->modules.dir);
    read_unsigned(c, s, "call_timeout_ms", 10, 60000, &cfg->modules.call_timeout_ms);
    warn_unknown(c, s, "modules");
}

/* The settings of the module of a slave: `module`, the name of the module, and the keys with their values. */
static void load_settings(ctx_t *c, yn_t *e, hubusd_settings_t *st)
{
    yn_t *s = section(c, e, "settings");
    unsigned i;

    if (!s) {
        return;
    }
    memset(st, 0, sizeof *st);
    read_str(c, s, "module", st->module, sizeof st->module);
    for (i = 0; !c->failed && i < s->n; i++) {
        const char *key = s->key[i];
        yn_t *v = s->item[i];

        if (strcmp(key, "module") == 0) {
            continue;
        }
        s->used[i] = 1;
        if (v->type != YN_SCALAR || !hubus_module_setting_valid(key, v->text)) {
            fail(c, s->key_line[i], "setting '%s' must be a key of letters, digits, '_' and '-' (at most %u) with a value "
                 "of at most %u printable characters", key, HUBUS_MODULE_SETTING_KEY_MAX, HUBUS_MODULE_SETTING_MAX);
            return;
        }
        if (st->n >= HUBUS_MODULE_SETTINGS_MAX) {
            fail(c, s->key_line[i], "more than %u settings", HUBUS_MODULE_SETTINGS_MAX);
            return;
        }
        snprintf(st->item[st->n].key, sizeof st->item[0].key, "%s", key);
        snprintf(st->item[st->n].value, sizeof st->item[0].value, "%s", v->text);
        st->n++;
    }
    if (!c->failed && st->n && !st->module[0]) {
        fail(c, s->line, "settings without 'module'");
    }
}

/* The options of a slave that is already set up (mcid, name), from a mapping. */
static void load_slave_options(ctx_t *c, yn_t *e, hubusd_slave_cfg_t *sl)
{
    read_str(c, e, "name", sl->name, sizeof sl->name);
    read_readback(c, e, "readback", &sl->readback, 1);
    read_unsigned(c, e, "poll_interval_s", 0, 86400, &sl->poll_interval_s);
    if (c->mode == MODE_MQTT) {
        (void)yn_get(e, "settings"); /* they belong to the module: what a read of the configuration shows may come back */
    } else {
        load_settings(c, e, &sl->settings);
    }
    warn_unknown(c, e, "slaves");
}

static void slave_defaults(hubusd_slave_cfg_t *sl, unsigned mcid)
{
    memset(sl, 0, sizeof *sl);
    sl->mcid = (uint8_t)mcid;
    sl->readback = -1;
    snprintf(sl->name, sizeof sl->name, "HUBUS slave %u", mcid);
}

/* A list of slaves replaces the list. Over MQTT the settings of a slave that stays are kept. */
static void load_slaves(ctx_t *c, yn_t *root)
{
    hubusd_runtime_cfg_t *cfg = c->rt;
    yn_t *s = yn_get(root, "slaves");
    hubusd_settings_t *kept = NULL;
    unsigned i;
    unsigned j;

    if (!s) {
        return;
    }
    if (s->type != YN_SEQ) {
        fail(c, s->line, "'slaves' must be a list");
        return;
    }
    if (c->mode == MODE_MQTT) {
        kept = calloc(HUBUS_MCID_MAX + 1u, sizeof *kept);
        if (!kept) {
            fail(c, s->line, "out of memory");
            return;
        }
        for (i = 0; i < cfg->n_slaves; i++) {
            kept[cfg->slaves[i].mcid] = cfg->slaves[i].settings;
        }
    }
    cfg->n_slaves = 0;
    for (i = 0; !c->failed && i < s->n; i++) {
        yn_t *e = s->item[i];
        hubusd_slave_cfg_t *sl;
        long mcid = 0;

        if (e->type != YN_MAP) {
            fail(c, e->line, "each slave must be a mapping with at least 'mcid'");
            break;
        }
        if (cfg->n_slaves >= HUBUSD_MAX_SLAVES) {
            fail(c, e->line, "too many slaves");
            break;
        }
        if (!yn_get(e, "mcid")) {
            fail(c, e->line, "slave without 'mcid'");
            break;
        }
        read_int(c, e, "mcid", HUBUS_MCID_MIN, HUBUS_MCID_MAX, &mcid);
        if (c->failed) {
            break;
        }
        for (j = 0; j < cfg->n_slaves; j++) {
            if (cfg->slaves[j].mcid == mcid) {
                fail(c, e->line, "MCID %ld is configured twice", mcid);
            }
        }
        sl = &cfg->slaves[cfg->n_slaves];
        slave_defaults(sl, (unsigned)mcid);
        if (kept) {
            sl->settings = kept[mcid];
        }
        load_slave_options(c, e, sl);
        cfg->n_slaves++;
    }
    free(kept);
}

/* ---- entry points ------------------------------------------------------------ */

/* `cfg` for the main file; for the runtime part only `rt`, and where warnings go (NULL over MQTT). */
static void ctx_init(ctx_t *c, int mode, hubusd_config_t *cfg, hubusd_runtime_cfg_t *rt, char (*warnings)[WARNING_LEN],
                     unsigned *n_warnings, char *err, size_t errsz)
{
    c->cfg = cfg;
    c->rt = rt;
    c->warnings = warnings;
    c->n_warnings = n_warnings;
    c->err = err;
    c->errsz = errsz;
    c->failed = 0;
    c->mode = mode;
    if (err && errsz) {
        err[0] = '\0';
    }
}

/* The document as a mapping, NULL after an error. */
static yn_t *parse_map(ctx_t *c, const char *text, size_t len)
{
    yn_t *root = yn_parse(text, len, c->err, c->errsz);

    if (!root) {
        c->failed = 1;
        if (!c->err[0]) {
            snprintf(c->err, c->errsz, "line 1: empty document");
        }
        return NULL;
    }
    if (root->type != YN_MAP) {
        fail(c, root->line, "the configuration must be a mapping");
        yn_free(root);
        return NULL;
    }
    return root;
}

int hubusd_config_parse(const char *text, size_t len, hubusd_config_t *cfg, char *err, size_t errsz)
{
    ctx_t c;
    yn_t *root;
    const char *env;

    hubusd_config_defaults(cfg);
    ctx_init(&c, MODE_MAIN, cfg, &cfg->rt, cfg->warnings, &cfg->n_warnings, err, errsz);
    root = parse_map(&c, text, len);
    if (!root) {
        return -1;
    }
    read_str(&c, root, "runtime_file", cfg->runtime_file, sizeof cfg->runtime_file);
    if (!c.failed) {
        load_gpio(&c, root);
    }
    if (!c.failed) {
        load_timing(&c, root);
    }
    if (!c.failed) {
        load_mqtt(&c, root);
    }
    if (!c.failed) {
        load_bus(&c, root);
    }
    if (!c.failed) {
        load_modules(&c, root);
    }
    if (!c.failed) {
        load_slaves(&c, root);
    }
    if (!c.failed) {
        warn_unknown(&c, root, "top level");
    }
    yn_free(root);
    if (c.failed) {
        return -1;
    }
    env = getenv("HUBUSD_MQTT_PASSWORD");
    if (env && strlen(env) < sizeof cfg->mqtt.password) {
        strcpy(cfg->mqtt.password, env);
    }
    return 0;
}

/* The runtime part, from the runtime file or over MQTT: only `c->rt`, which has nothing of the fixed part. */
static int load_runtime(ctx_t *c, const char *text, size_t len)
{
    static const char *const fixed[] = {"gpio", "timing", "modules", "runtime_file"};
    static const char *const fixed_mqtt[] = {"host",       "port",           "username",  "password", "password_file", "tls",
                                             "cafile",     "base_topic",     "discovery_prefix", "client_id"};
    yn_t *root;
    yn_t *s;
    unsigned i;

    root = parse_map(c, text, len);
    if (!root) {
        return -1;
    }
    for (i = 0; i < sizeof fixed / sizeof fixed[0] && !main_only(c, root, fixed[i], ""); i++) {
    }
    s = c->failed ? NULL : section(c, root, "mqtt");
    if (s) {
        for (i = 0; i < sizeof fixed_mqtt / sizeof fixed_mqtt[0] && !main_only(c, s, fixed_mqtt[i], "mqtt"); i++) {
        }
        if (!c->failed) {
            load_mqtt_runtime(c, s);
            warn_unknown(c, s, "mqtt");
        }
    }
    if (!c->failed) {
        load_bus(c, root);
    }
    if (!c->failed) {
        load_slaves(c, root);
    }
    if (!c->failed) {
        warn_unknown(c, root, "top level");
    }
    yn_free(root);
    return c->failed ? -1 : 0;
}

int hubusd_config_load_runtime(const char *text, size_t len, hubusd_config_t *cfg, char *err, size_t errsz)
{
    ctx_t c;

    ctx_init(&c, MODE_FILE, NULL, &cfg->rt, cfg->warnings, &cfg->n_warnings, err, errsz);
    return load_runtime(&c, text, len);
}

int hubusd_config_merge(const char *text, size_t len, hubusd_runtime_cfg_t *rt, char *err, size_t errsz)
{
    ctx_t c;

    ctx_init(&c, MODE_MQTT, NULL, rt, NULL, NULL, err, errsz);
    return load_runtime(&c, text, len);
}

/* ---- one slave over MQTT ----------------------------------------------------- */

hubusd_slave_cfg_t *hubusd_config_slave(hubusd_runtime_cfg_t *cfg, unsigned mcid)
{
    unsigned i;

    for (i = 0; i < cfg->n_slaves; i++) {
        if (cfg->slaves[i].mcid == mcid) {
            return &cfg->slaves[i];
        }
    }
    return NULL;
}

int hubusd_config_merge_slave(const char *text, size_t len, hubusd_runtime_cfg_t *cfg, unsigned mcid, char *err, size_t errsz)
{
    hubusd_slave_cfg_t *sl = hubusd_config_slave(cfg, mcid);
    hubusd_slave_cfg_t fresh;
    ctx_t c;
    yn_t *root;
    long m = (long)mcid;

    ctx_init(&c, MODE_MQTT, NULL, cfg, NULL, NULL, err, errsz);
    if (mcid < HUBUS_MCID_MIN || mcid > HUBUS_MCID_MAX) {
        snprintf(err, errsz, "MCID %u out of range 1..63", mcid);
        return -1;
    }
    root = parse_map(&c, text, len);
    if (!root) {
        return -1;
    }
    if (!sl) {
        slave_defaults(&fresh, mcid);
    }
    read_int(&c, root, "mcid", HUBUS_MCID_MIN, HUBUS_MCID_MAX, &m);
    if (!c.failed && m != (long)mcid) {
        fail(&c, root->line, "'mcid' %ld is not the slave of the topic (%u); hubusd changes an MCID through the topic mcid", m,
             mcid);
    }
    if (!c.failed) {
        load_slave_options(&c, root, sl ? sl : &fresh);
    }
    yn_free(root);
    if (c.failed) {
        return -1;
    }
    if (!sl) {
        cfg->slaves[cfg->n_slaves++] = fresh; /* there is room: at most one slave per MCID */
    }
    return 0;
}

int hubusd_config_remove_slave(hubusd_runtime_cfg_t *cfg, unsigned mcid)
{
    hubusd_slave_cfg_t *sl = hubusd_config_slave(cfg, mcid);
    size_t i;

    if (!sl) {
        return -1;
    }
    i = (size_t)(sl - cfg->slaves);
    memmove(sl, sl + 1, (cfg->n_slaves - i - 1u) * sizeof *sl);
    cfg->n_slaves--;
    return 0;
}

static char *read_file(const char *path, size_t *len, char *err, size_t errsz)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long size;

    if (!f) {
        snprintf(err, errsz, "%s: %s", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 1 << 20) {
        fclose(f);
        snprintf(err, errsz, "%s: file is too large", path);
        return NULL;
    }
    buf = malloc((size_t)size + 1u);
    if (!buf) {
        fclose(f);
        snprintf(err, errsz, "out of memory");
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(buf);
        snprintf(err, errsz, "%s: read error", path);
        return NULL;
    }
    fclose(f);
    buf[size] = '\0';
    *len = (size_t)size;
    return buf;
}

static void prefix_err(const char *path, char *err, size_t errsz)
{
    char tmp[256];

    snprintf(tmp, sizeof tmp, "%s: %s", path, err);
    snprintf(err, errsz, "%s", tmp);
}

/* A text that may be cut off: a warning, where the end matters least. */
static void set_text(char *dst, size_t size, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(dst, size, fmt, ap);
    va_end(ap);
}

/* Warnings from `first` on are about the file `path`. */
static void prefix_warnings(hubusd_config_t *cfg, unsigned first, const char *path)
{
    unsigned i;

    for (i = first; i < cfg->n_warnings; i++) {
        char tmp[sizeof cfg->warnings[0]];

        set_text(tmp, sizeof tmp, "%s: %s", path, cfg->warnings[i]);
        memcpy(cfg->warnings[i], tmp, sizeof tmp);
    }
}

/* Whether the main file has a part of the runtime part, which the runtime file then overrides. */
static int has_runtime_part(const char *text, size_t len)
{
    static const char *const keys[] = {"discovery", "discovery_cleanup", "stats_interval_s"};
    yn_t *root = yn_parse(text, len, NULL, 0);
    yn_t *mqtt;
    int found = 0;
    unsigned i;

    if (!root || root->type != YN_MAP) {
        yn_free(root);
        return 0;
    }
    found = yn_get(root, "bus") != NULL || yn_get(root, "slaves") != NULL;
    mqtt = yn_get(root, "mqtt");
    for (i = 0; mqtt && mqtt->type == YN_MAP && i < sizeof keys / sizeof keys[0]; i++) {
        found |= yn_get(mqtt, keys[i]) != NULL;
    }
    yn_free(root);
    return found;
}

int hubusd_config_load(const char *path, hubusd_config_t *cfg, char *err, size_t errsz)
{
    size_t len;
    char *buf = read_file(path, &len, err, errsz);
    int rc, overlap;
    unsigned first;

    if (!buf) {
        return -1;
    }
    rc = hubusd_config_parse(buf, len, cfg, err, errsz);
    overlap = rc == 0 && has_runtime_part(buf, len);
    free(buf);
    if (rc != 0) {
        prefix_err(path, err, errsz);
        return rc;
    }
    if (!cfg->runtime_file[0] || access(cfg->runtime_file, F_OK) != 0) {
        return 0; /* no runtime file yet: the main file has the start values */
    }
    buf = read_file(cfg->runtime_file, &len, err, errsz);
    if (!buf) {
        return -1;
    }
    first = cfg->n_warnings;
    rc = hubusd_config_load_runtime(buf, len, cfg, err, errsz);
    free(buf);
    if (rc != 0) {
        prefix_err(cfg->runtime_file, err, errsz);
        return rc;
    }
    prefix_warnings(cfg, first, cfg->runtime_file);
    if (overlap && cfg->n_warnings < HUBUSD_MAX_WARNINGS) {
        set_text(cfg->warnings[cfg->n_warnings++], sizeof cfg->warnings[0],
                 "%s: the runtime file %s overrides bus, slaves, and mqtt.discovery, discovery_cleanup, stats_interval_s here",
                 path, cfg->runtime_file);
    }
    return 0;
}

/* ---- the runtime file -------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t size, len;
    int overflow;
} out_t;

static void put(out_t *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->overflow) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(o->buf + o->len, o->size - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->size - o->len) {
        o->overflow = 1;
        return;
    }
    o->len += (size_t)n;
}

/* A string in double quotes, so that YAML takes it as it is. */
static void put_quoted(out_t *o, const char *s)
{
    put(o, "\"");
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;

        if (ch == '"' || ch == '\\') {
            put(o, "\\%c", ch);
        } else if (ch < 0x20u || ch == 0x7Fu) {
            put(o, "\\x%02X", ch);
        } else {
            put(o, "%c", ch);
        }
    }
    put(o, "\"");
}

static const char *readback_text(int readback)
{
    return readback == HUBUSD_READBACK_NONE ? "none" : readback == HUBUSD_READBACK_CACHE ? "cache" : "slave";
}

size_t hubusd_config_format_runtime(const hubusd_runtime_cfg_t *cfg, char *buf, size_t size)
{
    out_t o = {buf, size, 0, 0};
    unsigned i, k;

    if (size == 0u) {
        return 0;
    }
    buf[0] = '\0';
    put(&o, "# hubusd runtime configuration: what can change over MQTT (docs/daemon.md, Configuration).\n"
            "# hubusd writes this file after every change. Edit it only while hubusd is stopped, or send SIGHUP\n"
            "# right after saving it: the next change over MQTT would write over the edit.\n");
    put(&o, "bus:\n  readback: %s\n  ping_interval_s: %u\n  ping_failures: %u\n  scan_interval_s: %u\n"
            "  startup_retry_s: %u\n",
        readback_text(cfg->bus.readback), cfg->bus.ping_interval_s, cfg->bus.ping_failures, cfg->bus.scan_interval_s,
        cfg->bus.startup_retry_s);
    put(&o, "mqtt:\n  stats_interval_s: %u\n  discovery: %s\n  discovery_cleanup: %s\n", cfg->mqtt.stats_interval_s,
        cfg->mqtt.discovery ? "true" : "false", cfg->mqtt.discovery_cleanup ? "true" : "false");
    put(&o, cfg->n_slaves ? "slaves:\n" : "slaves: []\n");
    for (i = 0; i < cfg->n_slaves; i++) {
        const hubusd_slave_cfg_t *sl = &cfg->slaves[i];

        put(&o, "  - mcid: %u\n    name: ", sl->mcid);
        put_quoted(&o, sl->name);
        put(&o, "\n");
        if (sl->readback >= 0) {
            put(&o, "    readback: %s\n", readback_text(sl->readback));
        }
        if (sl->poll_interval_s) {
            put(&o, "    poll_interval_s: %u\n", sl->poll_interval_s);
        }
        if (sl->settings.n) {
            put(&o, "    settings:\n      module: ");
            put_quoted(&o, sl->settings.module);
            put(&o, "\n");
            for (k = 0; k < sl->settings.n; k++) {
                put(&o, "      %s: ", sl->settings.item[k].key);
                put_quoted(&o, sl->settings.item[k].value);
                put(&o, "\n");
            }
        }
    }
    if (o.overflow) {
        buf[0] = '\0';
        return 0;
    }
    return o.len;
}

int hubusd_config_write_runtime(const hubusd_runtime_cfg_t *cfg, const char *path, char *err, size_t errsz)
{
    size_t size = 4096u + (size_t)HUBUSD_MAX_SLAVES * (256u + HUBUS_MODULE_SETTINGS_MAX * (2u * HUBUS_MODULE_SETTING_MAX + 48u));
    char *buf = malloc(size);
    char tmp[PATH_MAX];
    size_t len;
    int fd, ok;

    if (!buf) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    len = hubusd_config_format_runtime(cfg, buf, size);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        snprintf(err, errsz, "%s: %s", tmp, strerror(errno));
        free(buf);
        return -1;
    }
    ok = len > 0u && write(fd, buf, len) == (ssize_t)len && fsync(fd) == 0;
    if (!ok) {
        snprintf(err, errsz, "%s: %s", tmp, len ? strerror(errno) : "too large");
    }
    free(buf);
    if (close(fd) != 0 && ok) {
        snprintf(err, errsz, "%s: %s", tmp, strerror(errno));
        ok = 0;
    }
    if (ok && rename(tmp, path) != 0) {
        snprintf(err, errsz, "%s: %s", path, strerror(errno));
        ok = 0;
    }
    if (!ok) {
        (void)unlink(tmp);
    }
    return ok ? 0 : -1;
}
