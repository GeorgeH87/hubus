#include "hubusd_config.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hubusd_yaml.h"

typedef struct {
    hubusd_config_t *cfg;
    char *err;
    size_t errsz;
    int failed;
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
    hubusd_config_t *cfg = c->cfg;
    char *w;
    int n;

    if (cfg->n_warnings >= HUBUSD_MAX_WARNINGS) {
        return;
    }
    w = cfg->warnings[cfg->n_warnings++];
    n = snprintf(w, sizeof cfg->warnings[0], "line %d: ", line);
    if (n > 0 && (size_t)n < sizeof cfg->warnings[0]) {
        va_start(ap, fmt);
        vsnprintf(w + n, sizeof cfg->warnings[0] - (size_t)n, fmt, ap);
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

/* readback: slave | cache | none (true = slave, false = none) */
static void read_readback(ctx_t *c, yn_t *map, const char *key, int *out)
{
    yn_t *n = scalar(c, map, key);

    if (!n) {
        return;
    }
    if (strcasecmp(n->text, "slave") == 0 || strcasecmp(n->text, "true") == 0 || strcasecmp(n->text, "yes") == 0 ||
        strcasecmp(n->text, "on") == 0) {
        *out = HUBUSD_READBACK_SLAVE;
    } else if (strcasecmp(n->text, "cache") == 0) {
        *out = HUBUSD_READBACK_CACHE;
    } else if (strcasecmp(n->text, "none") == 0 || strcasecmp(n->text, "false") == 0 || strcasecmp(n->text, "no") == 0 ||
               strcasecmp(n->text, "off") == 0) {
        *out = HUBUSD_READBACK_NONE;
    } else {
        fail(c, n->line, "%s must be slave, cache, or none, got %c%s%c", key, 39, n->text, 39);
    }
}

static void read_double(ctx_t *c, yn_t *map, const char *key, double *out)
{
    yn_t *n = scalar(c, map, key);
    char *end;
    double v;

    if (!n) {
        return;
    }
    v = strtod(n->text, &end);
    if (!*n->text || *end != '\0') {
        fail(c, n->line, "'%s' must be a number, got '%s'", key, n->text);
        return;
    }
    *out = v;
}

static void warn_unknown(ctx_t *c, yn_t *map, const char *where)
{
    unsigned i;

    if (!map) {
        return;
    }
    for (i = 0; i < map->n; i++) {
        if (!map->used[i]) {
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
    cfg->mqtt.discovery = 1;
    cfg->mqtt.stats_interval_s = 60;
    cfg->bus.readback = 1;
    cfg->bus.ping_interval_s = 30;
    cfg->bus.ping_failures = 3;
    cfg->bus.scan_interval_s = 300;
    cfg->bus.startup_retry_s = 10;
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

static void read_us(ctx_t *c, yn_t *s, const char *key, uint32_t *ns)
{
    unsigned us = *ns / 1000u;

    read_unsigned(c, s, key, 1, 10000000, &us);
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
    read_bool(c, s, "discovery", &cfg->mqtt.discovery);
    read_bool(c, s, "discovery_cleanup", &cfg->mqtt.discovery_cleanup);
    read_unsigned(c, s, "stats_interval_s", 0, 86400, &cfg->mqtt.stats_interval_s);
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
    hubusd_config_t *cfg = c->cfg;
    yn_t *s = section(c, root, "bus");

    if (!s) {
        return;
    }
    read_readback(c, s, "readback", &cfg->bus.readback);
    read_unsigned(c, s, "ping_interval_s", 0, 86400, &cfg->bus.ping_interval_s);
    read_unsigned(c, s, "ping_failures", 1, 100, &cfg->bus.ping_failures);
    read_unsigned(c, s, "scan_interval_s", 0, 86400, &cfg->bus.scan_interval_s);
    read_unsigned(c, s, "startup_retry_s", 1, 3600, &cfg->bus.startup_retry_s);
    warn_unknown(c, s, "bus");
}

static void load_cmd(ctx_t *c, yn_t *parent, const char *key, hubusd_cmd_t *cmd)
{
    yn_t *s = section(c, parent, key);
    yn_t *args;
    long op = 0;

    if (!s) {
        return;
    }
    if (!yn_get(s, "opcode")) {
        fail(c, s->line, "'%s' needs an 'opcode'", key);
        return;
    }
    read_int(c, s, "opcode", 0x10, 0xFF, &op);
    cmd->opcode = (uint8_t)op;
    args = yn_get(s, "args");
    if (args) {
        unsigned i;

        if (args->type != YN_SEQ) {
            fail(c, args->line, "'args' must be a list of bytes");
        } else if (args->n > HUBUSD_CMD_MAX_ARGS) {
            fail(c, args->line, "'args' has more than %d bytes", HUBUSD_CMD_MAX_ARGS);
        }
        for (i = 0; !c->failed && args && i < args->n; i++) {
            long v;

            if (args->item[i]->type != YN_SCALAR || !parse_long(args->item[i]->text, &v) || v < 0 || v > 255) {
                fail(c, args->item[i]->line, "argument bytes must be numbers between 0 and 255");
            } else {
                cmd->args[i] = (uint8_t)v;
                cmd->n_args = (uint8_t)(i + 1u);
            }
        }
    }
    cmd->valid = 1;
    warn_unknown(c, s, key);
}

static void load_profile(ctx_t *c, const char *name, yn_t *s)
{
    hubusd_config_t *cfg = c->cfg;
    hubusd_profile_t *p;
    yn_t *n;
    long v;
    unsigned i;

    if (cfg->n_profiles >= HUBUSD_MAX_PROFILES) {
        fail(c, s->line, "too many profiles (maximum %d)", HUBUSD_MAX_PROFILES);
        return;
    }
    if (strlen(name) >= HUBUSD_PROFILE_NAME_LEN || !*name) {
        fail(c, s->line, "profile name '%s' is empty or too long", name);
        return;
    }
    if (hubusd_builtin_profile(name)) {
        fail(c, s->line, "profile '%s' is built in and cannot be redefined", name);
        return;
    }
    for (i = 0; i < cfg->n_profiles; i++) {
        if (strcmp(cfg->profiles[i].name, name) == 0) {
            fail(c, s->line, "profile '%s' is defined twice", name);
            return;
        }
    }
    p = &cfg->profiles[cfg->n_profiles];
    hubusd_profile_init(p);
    snprintf(p->name, sizeof p->name, "%s", name);

    n = scalar(c, s, "component");
    if (!n) {
        if (!c->failed) {
            fail(c, s->line, "profile '%s' needs a 'component' (switch, binary_sensor, sensor)", name);
        }
        return;
    }
    if (strcmp(n->text, "switch") == 0) {
        p->component = HUBUSD_COMP_SWITCH;
    } else if (strcmp(n->text, "binary_sensor") == 0) {
        p->component = HUBUSD_COMP_BINARY_SENSOR;
    } else if (strcmp(n->text, "sensor") == 0) {
        p->component = HUBUSD_COMP_SENSOR;
    } else {
        fail(c, n->line, "unknown component '%s' (switch, binary_sensor, sensor)", n->text);
        return;
    }
    v = -1;
    read_int(c, s, "device_type", 0, 255, &v);
    p->device_type = (int)v;
    v = p->byte_offset;
    read_int(c, s, "byte_offset", 0, 30, &v);
    p->byte_offset = (uint8_t)v;
    v = p->byte_len;
    read_int(c, s, "byte_length", 1, 2, &v);
    p->byte_len = (uint8_t)v;
    {
        int b = 0;
        read_bool(c, s, "signed", &b);
        p->is_signed = (uint8_t)b;
    }
    n = scalar(c, s, "byte_order");
    if (n) {
        if (strcmp(n->text, "big") == 0) {
            p->big_endian = 1;
        } else if (strcmp(n->text, "little") != 0) {
            fail(c, n->line, "byte_order must be 'big' or 'little'");
        }
    }
    read_double(c, s, "scale", &p->scale);
    read_double(c, s, "offset", &p->add);
    v = p->decimals;
    read_int(c, s, "decimals", 0, 6, &v);
    p->decimals = (uint8_t)v;
    read_str(c, s, "unit", p->unit, sizeof p->unit);
    read_str(c, s, "device_class", p->device_class, sizeof p->device_class);
    load_cmd(c, s, "on", &p->on);
    load_cmd(c, s, "off", &p->off);
    if (!c->failed && p->component == HUBUSD_COMP_SWITCH && (!p->on.valid || !p->off.valid)) {
        fail(c, s->line, "switch profile '%s' needs 'on' and 'off' commands", name);
    }
    if (!c->failed && (unsigned)p->byte_offset + p->byte_len > HUBUS_MAX_PAYLOAD) {
        fail(c, s->line, "byte_offset + byte_length exceed the payload");
    }
    warn_unknown(c, s, name);
    cfg->n_profiles++;
}

static void load_profiles(ctx_t *c, yn_t *root)
{
    yn_t *s = section(c, root, "profiles");
    unsigned i;

    for (i = 0; s && !c->failed && i < s->n; i++) {
        if (s->item[i]->type != YN_MAP) {
            fail(c, s->item[i]->line, "profile '%s' must be a mapping", s->key[i]);
        } else {
            s->used[i] = 1;
            load_profile(c, s->key[i], s->item[i]);
        }
    }
}

const hubusd_profile_t *hubusd_config_profile(const hubusd_config_t *cfg, const char *name)
{
    unsigned i;

    for (i = 0; i < cfg->n_profiles; i++) {
        if (strcmp(cfg->profiles[i].name, name) == 0) {
            return &cfg->profiles[i];
        }
    }
    return hubusd_builtin_profile(name);
}

static void load_slaves(ctx_t *c, yn_t *root)
{
    hubusd_config_t *cfg = c->cfg;
    yn_t *s = yn_get(root, "slaves");
    unsigned i;
    unsigned j;

    if (!s) {
        return;
    }
    if (s->type != YN_SEQ) {
        fail(c, s->line, "'slaves' must be a list");
        return;
    }
    for (i = 0; !c->failed && i < s->n; i++) {
        yn_t *e = s->item[i];
        hubusd_slave_cfg_t *sl;
        long mcid = 0;
        long rb;
        yn_t *prof;

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
        memset(sl, 0, sizeof *sl);
        sl->mcid = (uint8_t)mcid;
        sl->readback = -1;
        snprintf(sl->name, sizeof sl->name, "HUBUS slave %ld", mcid);
        read_str(c, e, "name", sl->name, sizeof sl->name);
        read_str(c, e, "profile", sl->profile, sizeof sl->profile);
        prof = yn_get(e, "profile");
        if (!c->failed && sl->profile[0] && !hubusd_config_profile(cfg, sl->profile)) {
            fail(c, prof ? prof->line : e->line, "unknown profile '%s'", sl->profile);
        }
        rb = -1;
        if (yn_get(e, "readback")) {
            int b = HUBUSD_READBACK_SLAVE;
            read_readback(c, e, "readback", &b);
            rb = b;
        }
        sl->readback = (int)rb;
        read_unsigned(c, e, "poll_interval_s", 0, 86400, &sl->poll_interval_s);
        warn_unknown(c, e, "slaves");
        cfg->n_slaves++;
    }
}

/* ---- entry points ------------------------------------------------------------ */

int hubusd_config_parse(const char *text, size_t len, hubusd_config_t *cfg, char *err, size_t errsz)
{
    ctx_t c;
    yn_t *root;
    const char *env;

    hubusd_config_defaults(cfg);
    if (err && errsz) {
        err[0] = '\0';
    }
    root = yn_parse(text, len, err, errsz);
    if (!root) {
        return -1;
    }
    if (root->type != YN_MAP) {
        snprintf(err, errsz, "line %d: the configuration must be a mapping", root->line);
        yn_free(root);
        return -1;
    }
    c.cfg = cfg;
    c.err = err;
    c.errsz = errsz;
    c.failed = 0;

    load_gpio(&c, root);
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
        load_profiles(&c, root);
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

int hubusd_config_load(const char *path, hubusd_config_t *cfg, char *err, size_t errsz)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long size;
    int rc;

    if (!f) {
        snprintf(err, errsz, "%s: %s", path, strerror(errno));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 1 << 20) {
        fclose(f);
        snprintf(err, errsz, "%s: file is too large", path);
        return -1;
    }
    buf = malloc((size_t)size + 1u);
    if (!buf) {
        fclose(f);
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(buf);
        snprintf(err, errsz, "%s: read error", path);
        return -1;
    }
    fclose(f);
    rc = hubusd_config_parse(buf, (size_t)size, cfg, err, errsz);
    free(buf);
    if (rc != 0) {
        char tmp[256];

        snprintf(tmp, sizeof tmp, "%s: %s", path, err);
        snprintf(err, errsz, "%s", tmp);
    }
    return rc;
}
