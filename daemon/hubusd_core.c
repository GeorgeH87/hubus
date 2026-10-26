#include "hubusd_core.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hubusd_json.h"

#define PAYLOAD_MAX 1024

/* ---- helpers ------------------------------------------------------------------ */

static uint64_t now_ms(const hubusd_t *d)
{
    return d->clock.mono_ms(d->clock.ctx);
}

static uint64_t wall_s(const hubusd_t *d)
{
    return d->clock.wall_s ? d->clock.wall_s(d->clock.ctx) : 0;
}

static void vtopic(const hubusd_t *d, char *out, size_t size, const char *fmt, va_list ap)
{
    int n = snprintf(out, size, "%s/", d->cfg.mqtt.base_topic);

    if (n > 0 && (size_t)n < size) {
        vsnprintf(out + n, size - (size_t)n, fmt, ap);
    }
}

static void topic(const hubusd_t *d, char *out, size_t size, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vtopic(d, out, size, fmt, ap);
    va_end(ap);
}

static void pub(hubusd_t *d, const char *t, const char *payload, int retain)
{
    if (d->mqtt.publish(d->mqtt.ctx, t, payload, strlen(payload), retain) != 0) {
        d->stats.publish_errors++;
    }
}

static void pub_topic(hubusd_t *d, int retain, const char *payload, const char *fmt, ...)
{
    char t[160];
    va_list ap;

    va_start(ap, fmt);
    vtopic(d, t, sizeof t, fmt, ap);
    va_end(ap);
    pub(d, t, payload, retain);
}

static void pub_json(hubusd_t *d, jw_t *w, int retain, const char *t)
{
    if (!jw_ok(w)) {
        d->stats.publish_errors++;
        return;
    }
    pub(d, t, w->buf, retain);
}

static void log_event(hubusd_t *d, int level, const char *fmt, ...)
{
    char msg[200];
    char buf[320];
    char t[160];
    jw_t w;
    va_list ap;
    static const char *const names[] = {"info", "warning", "error"};

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (d->log_fn) {
        d->log_fn(d->log_ctx, level, msg);
    }
    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_str(&w, "level", names[level < 0 || level > 2 ? 0 : level]);
    jw_str(&w, "msg", msg);
    jw_uint(&w, "ts", wall_s(d));
    jw_obj_end(&w);
    topic(d, t, sizeof t, "master/log");
    pub_json(d, &w, 0, t);
}

static void hex(const uint8_t *data, unsigned n, char *out)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        snprintf(out + 2u * i, 3, "%02x", data[i]);
    }
    out[2u * n] = '\0';
}

static const char *slave_name(const hubusd_t *d, unsigned mcid)
{
    return d->slave[mcid].configured ? d->cfg.slaves[d->slave[mcid].cfg_index].name : "unconfigured";
}

/* Application bytes of SLAVE_INFO: [0] device type, [1] firmware version, [2..] serial number. */
static int info_device_type(const hubusd_slave_t *s)
{
    return (s->have_info && s->info.len > HUBUS_INFO_APP) ? s->info.payload[HUBUS_INFO_APP] : -1;
}

static int info_can_capture(const hubusd_slave_t *s)
{
    return !s->have_info || (s->info.payload[HUBUS_INFO_CAPS] & HUBUS_CAP_STATUS_CAPTURE);
}

/* The slave exposes a live state: REQUEST_STATE reads it in one transaction. */
static int info_has_state(const hubusd_slave_t *s)
{
    return s->have_info && (s->info.payload[HUBUS_INFO_CAPS] & HUBUS_CAP_STATE);
}

static int slave_readback(const hubusd_t *d, unsigned mcid)
{
    const hubusd_slave_t *s = &d->slave[mcid];
    int rb = s->configured ? d->cfg.slaves[s->cfg_index].readback : -1;

    if (rb < 0) {
        rb = d->cfg.bus.readback;
    }
    /* A slave with neither a state nor STATUS_CAPTURE cannot be read back: its state exists only in the cache. */
    if (rb == HUBUSD_READBACK_SLAVE && !info_has_state(s) && !info_can_capture(s)) {
        rb = HUBUSD_READBACK_CACHE;
    }
    return rb;
}

static void sanitize_id(const char *in, char *out, size_t size)
{
    size_t i;

    for (i = 0; in[i] && i + 1u < size; i++) {
        out[i] = isalnum((unsigned char)in[i]) ? in[i] : '_';
    }
    out[i] = '\0';
}

/* ---- queue (shared with the MQTT thread) ---------------------------------------------- */

static int enqueue(hubusd_t *d, const hubusd_request_t *r)
{
    int ok = 0;

    pthread_mutex_lock(&d->lock);
    if (d->q_count < HUBUSD_QUEUE_LEN) {
        d->queue[(d->q_head + d->q_count) % HUBUSD_QUEUE_LEN] = *r;
        d->q_count++;
        ok = 1;
    } else {
        d->stats.queue_full++;
    }
    pthread_mutex_unlock(&d->lock);
    return ok;
}

static int dequeue(hubusd_t *d, hubusd_request_t *r)
{
    int ok = 0;

    pthread_mutex_lock(&d->lock);
    if (d->q_count > 0) {
        *r = d->queue[d->q_head];
        d->q_head = (d->q_head + 1u) % HUBUSD_QUEUE_LEN;
        d->q_count--;
        ok = 1;
    }
    pthread_mutex_unlock(&d->lock);
    return ok;
}

static unsigned queue_depth(hubusd_t *d)
{
    unsigned n;

    pthread_mutex_lock(&d->lock);
    n = d->q_count;
    pthread_mutex_unlock(&d->lock);
    return n;
}

static void enqueue_reject(hubusd_t *d, const char *fmt, ...)
{
    hubusd_request_t r;
    va_list ap;

    memset(&r, 0, sizeof r);
    r.kind = HUBUSD_REQ_REJECT;
    va_start(ap, fmt);
    vsnprintf(r.text, sizeof r.text, fmt, ap);
    va_end(ap);
    enqueue(d, &r);
}

/* ---- publishing ------------------------------------------------------------------------ */

static void publish_master_status(hubusd_t *d, int online)
{
    pub_topic(d, 1, online ? "online" : "offline", "master/status");
}

static void publish_availability(hubusd_t *d, unsigned mcid)
{
    int on = d->slave[mcid].online == 1;

    pub_topic(d, 1, on ? "online" : "offline", "slave/%u/availability", mcid);
}

static const char *bus_text(const hubusd_t *d)
{
    switch (d->bus) {
    case HUBUSD_BUS_OK: return "ok";
    case HUBUSD_BUS_LINE_FAULT: return "line_fault";
    default: return "starting";
    }
}

static void publish_state(hubusd_t *d, int force)
{
    char buf[768];
    char serial[64];
    char t[160];
    jw_t w;
    unsigned mcid, configured = 0, online = 0;

    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        configured += d->slave[mcid].configured;
        online += (d->slave[mcid].configured && d->slave[mcid].online == 1);
    }
    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_str(&w, "bus", bus_text(d));
    jw_str(&w, "error", d->last_error);
    jw_str(&w, "version", HUBUSD_VERSION);
    jw_uint(&w, "configured_slaves", configured);
    jw_uint(&w, "online_slaves", online);
    jw_arr_begin(&w, "unknown_slaves");
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        const hubusd_slave_t *s = &d->slave[mcid];

        if (!s->unknown_seen) {
            continue;
        }
        jw_obj_begin(&w, NULL);
        jw_uint(&w, "mcid", mcid);
        if (info_device_type(s) >= 0) {
            jw_uint(&w, "device_type", (uint64_t)info_device_type(s));
        }
        if (s->info.len > HUBUS_INFO_APP + 2u) {
            hex(&s->info.payload[HUBUS_INFO_APP + 2u], (unsigned)s->info.len - HUBUS_INFO_APP - 2u, serial);
            jw_str(&w, "serial", serial);
        }
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    if (!jw_ok(&w)) {
        d->stats.publish_errors++;
        return;
    }
    if (force || strcmp(buf, d->last_state_json) != 0) {
        snprintf(d->last_state_json, sizeof d->last_state_json, "%s", buf);
        topic(d, t, sizeof t, "master/state");
        pub(d, t, buf, 1);
    }
}

static void publish_diagnosis(hubusd_t *d, int force)
{
    char buf[256];
    char t[160];
    jw_t w;
    uint64_t suspects;
    uint8_t diag = hubus_master_diagnose(&d->master, &suspects);
    unsigned mcid;

    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_str(&w, "status", diag == HUBUS_DIAG_OK ? "ok" : diag == HUBUS_DIAG_DUPLICATE ? "suspected_duplicate_mcid" : "line_problem");
    jw_arr_begin(&w, "suspects");
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        if (suspects & ((uint64_t)1 << mcid)) {
            jw_uint(&w, NULL, mcid);
        }
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    if (!jw_ok(&w)) {
        return;
    }
    if (force || strcmp(buf, d->last_diag_json) != 0) {
        if (diag != HUBUS_DIAG_OK && strcmp(buf, d->last_diag_json) != 0) {
            log_event(d, 1, "diagnosis: %s", diag == HUBUS_DIAG_DUPLICATE ? "suspected duplicate MCID" : "suspected line problem");
        }
        snprintf(d->last_diag_json, sizeof d->last_diag_json, "%s", buf);
        topic(d, t, sizeof t, "master/diagnosis");
        pub(d, t, buf, 1);
    }
}

static void publish_stats(hubusd_t *d)
{
    char buf[1024];
    char t[160];
    jw_t w;
    const hubus_master_counters_t *c = &d->master.counters;
    uint64_t now = now_ms(d);

    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_uint(&w, "uptime_s", (now - d->start_ms) / 1000u);
    jw_uint(&w, "bursts", d->stats.bursts);
    jw_uint(&w, "polls", c->polls);
    jw_uint(&w, "info_requests", c->info_requests);
    jw_uint(&w, "data_requests", c->data_requests);
    jw_uint(&w, "state_requests", c->state_requests);
    jw_uint(&w, "frames", d->stats.frames);
    jw_uint(&w, "duplicates", d->stats.duplicates);
    jw_uint(&w, "gaps", d->stats.gaps);
    jw_uint(&w, "retries", d->stats.retries);
    jw_uint(&w, "skipped", d->stats.skipped);
    jw_uint(&w, "commands_sent", c->commands);
    jw_uint(&w, "commands_executed", d->stats.commands_executed);
    jw_uint(&w, "commands_rejected", d->stats.commands_rejected);
    jw_uint(&w, "no_response", c->no_response);
    jw_uint(&w, "crc_errors", c->crc_errors);
    jw_uint(&w, "mismatches", c->mismatches);
    jw_uint(&w, "line_errors", c->line_errors);
    jw_uint(&w, "line_faults", c->line_faults);
    jw_uint(&w, "recoveries", c->recoveries);
    jw_uint(&w, "errors_total", (uint64_t)c->crc_errors + c->mismatches + c->line_errors + c->line_faults);
    jw_uint(&w, "startups", d->stats.startups);
    jw_uint(&w, "scans", d->stats.scans);
    jw_uint(&w, "queue_depth", queue_depth(d));
    pthread_mutex_lock(&d->lock);
    jw_uint(&w, "queue_full", d->stats.queue_full);
    pthread_mutex_unlock(&d->lock);
    jw_uint(&w, "publish_errors", d->stats.publish_errors);
    jw_str(&w, "bus", bus_text(d));
    jw_obj_end(&w);
    topic(d, t, sizeof t, "master/stats");
    pub_json(d, &w, 0, t);
}

static void publish_slave_stats(hubusd_t *d, unsigned mcid)
{
    char buf[400];
    char t[160];
    jw_t w;
    const hubusd_slave_t *s = &d->slave[mcid];
    const hubus_mcid_counters_t *c = &d->master.mcid_counters[mcid];

    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_uint(&w, "frames", c->frames);
    jw_uint(&w, "duplicates", c->duplicates);
    jw_uint(&w, "gaps", c->gaps);
    jw_uint(&w, "lost_contents", c->lost);
    jw_uint(&w, "crc_errors", c->crc_errors);
    jw_uint(&w, "no_response", c->no_response);
    jw_uint(&w, "consecutive_failures", d->master.fail[mcid]);
    jw_str(&w, "availability", s->online == 1 ? "online" : s->online == 0 ? "offline" : "unknown");
    if (s->last_seen_ms) {
        jw_uint(&w, "last_seen_s_ago", (now_ms(d) - s->last_seen_ms) / 1000u);
    } else {
        jw_null(&w, "last_seen_s_ago");
    }
    jw_obj_end(&w);
    topic(d, t, sizeof t, "slave/%u/stats", mcid);
    pub_json(d, &w, 0, t);
}

static void publish_slave_info(hubusd_t *d, unsigned mcid)
{
    char buf[512];
    char t[160];
    char app[2 * HUBUS_MAX_PAYLOAD + 1];
    char serial[2 * HUBUS_MAX_PAYLOAD + 1];
    jw_t w;
    const hubusd_slave_t *s = &d->slave[mcid];

    if (!s->have_info) {
        return;
    }
    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_uint(&w, "mcid", mcid);
    jw_str(&w, "name", slave_name(d, mcid));
    jw_uint(&w, "protocol", s->info.payload[HUBUS_INFO_VERSION]);
    jw_uint(&w, "capabilities", s->info.payload[HUBUS_INFO_CAPS] & (uint8_t)~HUBUS_CAP_SPEED_MASK);
    jw_uint(&w, "speed_class", HUBUS_CAP_SPEED_CLASS(s->info.payload[HUBUS_INFO_CAPS]));
    jw_bool(&w, "events", (s->info.payload[HUBUS_INFO_CAPS] & HUBUS_CAP_EVENTS) != 0);
    jw_bool(&w, "status_capture", (s->info.payload[HUBUS_INFO_CAPS] & HUBUS_CAP_STATUS_CAPTURE) != 0);
    jw_bool(&w, "state", (s->info.payload[HUBUS_INFO_CAPS] & HUBUS_CAP_STATE) != 0);
    jw_bool(&w, "update", (s->info.payload[HUBUS_INFO_CAPS] & HUBUS_CAP_UPDATE) != 0);
    jw_bool(&w, "updater", (s->info.payload[HUBUS_INFO_CAPS] & HUBUS_CAP_UPDATER) != 0);
    jw_uint(&w, "max_command", s->info.payload[HUBUS_INFO_MAX_CMD]);
    hex(&s->info.payload[HUBUS_INFO_APP], s->info.len > HUBUS_INFO_APP ? (unsigned)s->info.len - HUBUS_INFO_APP : 0u, app);
    jw_str(&w, "app_data", app);
    if (info_device_type(s) >= 0) {
        jw_uint(&w, "device_type", (uint64_t)info_device_type(s));
    }
    if (s->info.len > HUBUS_INFO_APP + 1u) {
        jw_uint(&w, "firmware", s->info.payload[HUBUS_INFO_APP + 1u]);
    }
    if (s->info.len > HUBUS_INFO_APP + 2u) {
        hex(&s->info.payload[HUBUS_INFO_APP + 2u], (unsigned)s->info.len - HUBUS_INFO_APP - 2u, serial);
        jw_str(&w, "serial", serial);
    }
    jw_str(&w, "profile", s->profile ? s->profile->name : "");
    jw_obj_end(&w);
    topic(d, t, sizeof t, "slave/%u/info", mcid);
    pub_json(d, &w, 1, t);
}

/* ---- Home Assistant discovery -------------------------------------------------------------- */

static const char *component_name(hubusd_component_t c)
{
    switch (c) {
    case HUBUSD_COMP_SWITCH: return "switch";
    case HUBUSD_COMP_BINARY_SENSOR: return "binary_sensor";
    default: return "sensor";
    }
}

static void discovery_topic(const hubusd_t *d, char *out, size_t size, const char *component, const char *object)
{
    snprintf(out, size, "%s/%s/%s/config", d->cfg.mqtt.discovery_prefix, component, object);
}

static void master_identifier(const hubusd_t *d, char *out, size_t size)
{
    char id[64];

    sanitize_id(d->cfg.mqtt.client_id, id, sizeof id);
    snprintf(out, size, "hubusd_%s", id);
}

static void publish_slave_discovery(hubusd_t *d, unsigned mcid)
{
    char buf[1400];
    char t[160], tt[160], object[48], master[96], ident[48], serial[2 * HUBUS_MAX_PAYLOAD + 1], fw[16];
    jw_t w;
    const hubusd_slave_t *s = &d->slave[mcid];
    const hubusd_profile_t *p = s->profile;

    if (!d->cfg.mqtt.discovery || !s->configured || !p) {
        return;
    }
    snprintf(object, sizeof object, "hubus_%u", mcid);
    snprintf(ident, sizeof ident, "hubus_%u", mcid);
    master_identifier(d, master, sizeof master);

    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_null(&w, "name");
    jw_bool(&w, "has_entity_name", 1);
    jw_str(&w, "unique_id", ident);
    topic(d, t, sizeof t, "slave/%u/state", mcid);
    jw_str(&w, "state_topic", t);
    if (p->component == HUBUSD_COMP_SWITCH) {
        topic(d, t, sizeof t, "slave/%u/set", mcid);
        jw_str(&w, "command_topic", t);
    }
    if (p->component == HUBUSD_COMP_SENSOR) {
        if (p->unit[0]) {
            jw_str(&w, "unit_of_measurement", p->unit);
        }
        if (p->device_class[0]) {
            jw_str(&w, "device_class", p->device_class);
        }
        jw_str(&w, "state_class", "measurement");
    } else {
        jw_str(&w, "payload_on", "ON");
        jw_str(&w, "payload_off", "OFF");
        if (p->component == HUBUSD_COMP_SWITCH) {
            jw_str(&w, "state_on", "ON");
            jw_str(&w, "state_off", "OFF");
        }
    }
    jw_arr_begin(&w, "availability");
    jw_obj_begin(&w, NULL);
    topic(d, t, sizeof t, "master/status");
    jw_str(&w, "topic", t);
    jw_obj_end(&w);
    jw_obj_begin(&w, NULL);
    topic(d, t, sizeof t, "slave/%u/availability", mcid);
    jw_str(&w, "topic", t);
    jw_obj_end(&w);
    jw_arr_end(&w);
    jw_str(&w, "availability_mode", "all");
    jw_obj_begin(&w, "device");
    jw_arr_begin(&w, "identifiers");
    jw_str(&w, NULL, ident);
    jw_arr_end(&w);
    jw_str(&w, "name", slave_name(d, mcid));
    jw_str(&w, "manufacturer", "HUBUS");
    jw_str(&w, "model", p->name);
    if (s->have_info && s->info.len > HUBUS_INFO_APP + 1u) {
        snprintf(fw, sizeof fw, "%u", s->info.payload[HUBUS_INFO_APP + 1u]);
        jw_str(&w, "sw_version", fw);
    }
    if (s->have_info && s->info.len > HUBUS_INFO_APP + 2u) {
        hex(&s->info.payload[HUBUS_INFO_APP + 2u], (unsigned)s->info.len - HUBUS_INFO_APP - 2u, serial);
        jw_str(&w, "serial_number", serial);
    }
    jw_str(&w, "via_device", master);
    jw_obj_end(&w);
    jw_obj_end(&w);
    discovery_topic(d, tt, sizeof tt, component_name(p->component), object);
    pub_json(d, &w, 1, tt);
}

static void remove_slave_discovery(hubusd_t *d, unsigned mcid, const hubusd_profile_t *p)
{
    char tt[160], object[48];

    if (!d->cfg.mqtt.discovery) {
        return;
    }
    snprintf(object, sizeof object, "hubus_%u", mcid);
    if (p) {
        discovery_topic(d, tt, sizeof tt, component_name(p->component), object);
        pub(d, tt, "", 1);
    } else {
        discovery_topic(d, tt, sizeof tt, "switch", object);
        pub(d, tt, "", 1);
        discovery_topic(d, tt, sizeof tt, "binary_sensor", object);
        pub(d, tt, "", 1);
        discovery_topic(d, tt, sizeof tt, "sensor", object);
        pub(d, tt, "", 1);
    }
}

typedef struct {
    const char *key;
    const char *name;
    const char *unit;
    const char *device_class;
    const char *state_class;
    const char *category;
} master_sensor_t;

static const master_sensor_t master_sensors[] = {
    {"uptime_s", "Uptime", "s", "duration", "total_increasing", "diagnostic"},
    {"errors_total", "Bus errors", NULL, NULL, "total_increasing", "diagnostic"},
    {"retries", "Retries", NULL, NULL, "total_increasing", "diagnostic"},
    {"duplicates", "Duplicate frames", NULL, NULL, "total_increasing", "diagnostic"},
    {"gaps", "Lost buffer contents", NULL, NULL, "total_increasing", "diagnostic"},
    {"commands_rejected", "Rejected commands", NULL, NULL, "total_increasing", "diagnostic"},
    {"bus", "Bus state", NULL, NULL, NULL, "diagnostic"},
};

static void publish_master_discovery(hubusd_t *d)
{
    char buf[900];
    char t[160], tt[200], master[96], object[160];
    unsigned i;

    if (!d->cfg.mqtt.discovery) {
        return;
    }
    master_identifier(d, master, sizeof master);
    for (i = 0; i < sizeof master_sensors / sizeof master_sensors[0]; i++) {
        const master_sensor_t *m = &master_sensors[i];
        jw_t w;

        jw_init(&w, buf, sizeof buf);
        jw_obj_begin(&w, NULL);
        jw_str(&w, "name", m->name);
        jw_bool(&w, "has_entity_name", 1);
        snprintf(object, sizeof object, "%s_%s", master, m->key);
        jw_str(&w, "unique_id", object);
        topic(d, t, sizeof t, "master/stats");
        jw_str(&w, "state_topic", t);
        snprintf(tt, sizeof tt, "{{ value_json.%s }}", m->key);
        jw_str(&w, "value_template", tt);
        if (m->unit) {
            jw_str(&w, "unit_of_measurement", m->unit);
        }
        if (m->device_class) {
            jw_str(&w, "device_class", m->device_class);
        }
        if (m->state_class) {
            jw_str(&w, "state_class", m->state_class);
        }
        jw_str(&w, "entity_category", m->category);
        topic(d, t, sizeof t, "master/status");
        jw_str(&w, "availability_topic", t);
        jw_obj_begin(&w, "device");
        jw_arr_begin(&w, "identifiers");
        jw_str(&w, NULL, master);
        jw_arr_end(&w);
        jw_str(&w, "name", "HUBUS master");
        jw_str(&w, "manufacturer", "HUBUS");
        jw_str(&w, "model", "hubusd");
        jw_str(&w, "sw_version", HUBUSD_VERSION);
        jw_obj_end(&w);
        jw_obj_end(&w);
        discovery_topic(d, tt, sizeof tt, "sensor", object);
        pub_json(d, &w, 1, tt);
    }
}

static void publish_all_retained(hubusd_t *d)
{
    unsigned mcid;

    publish_master_status(d, 1);
    publish_master_discovery(d);
    publish_state(d, 1);
    publish_diagnosis(d, 1);
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        const hubusd_slave_t *s = &d->slave[mcid];

        if (!s->configured) {
            continue;
        }
        publish_availability(d, mcid);
        publish_slave_info(d, mcid);
        publish_slave_discovery(d, mcid);
        if (s->last_state[0]) {
            pub_topic(d, 1, s->last_state, "slave/%u/state", mcid);
        }
    }
}

/* ---- configuration binding ---------------------------------------------------------------------- */

static void resolve_profile(hubusd_t *d, unsigned mcid)
{
    hubusd_slave_t *s = &d->slave[mcid];
    const hubusd_slave_cfg_t *sc;
    int type;
    unsigned i;

    if (!s->configured) {
        return;
    }
    sc = &d->cfg.slaves[s->cfg_index];
    if (sc->profile[0]) {
        s->profile = hubusd_config_profile(&d->cfg, sc->profile);
        return;
    }
    s->profile = NULL;
    type = info_device_type(s);
    if (type < 0) {
        return;
    }
    for (i = 0; i < d->cfg.n_profiles; i++) {
        if (d->cfg.profiles[i].device_type == type) {
            s->profile = &d->cfg.profiles[i];
            return;
        }
    }
    s->profile = hubusd_builtin_profile_by_type(type);
}

static void bind_config(hubusd_t *d)
{
    unsigned i;
    uint64_t now = now_ms(d);

    for (i = 0; i <= HUBUS_MCID_MAX; i++) {
        d->slave[i].configured = 0;
        d->slave[i].profile = NULL;
    }
    for (i = 0; i < d->cfg.n_slaves; i++) {
        hubusd_slave_t *s = &d->slave[d->cfg.slaves[i].mcid];
        unsigned interval = d->cfg.bus.ping_interval_s ? d->cfg.bus.ping_interval_s : 1u;

        s->configured = 1;
        s->cfg_index = (uint8_t)i;
        s->next_ping_ms = now + (uint64_t)i * interval * 1000u / (d->cfg.n_slaves ? d->cfg.n_slaves : 1u);
        s->next_poll_ms = now + (uint64_t)d->cfg.slaves[i].poll_interval_s * 1000u;
        s->unknown_seen = 0;
        resolve_profile(d, d->cfg.slaves[i].mcid);
    }
}

void hubusd_init(hubusd_t *d, const hubusd_config_t *cfg, const hubus_master_hal_t *hal, hubusd_mqtt_t mqtt,
                 hubusd_clock_t clock)
{
    unsigned i;

    memset(d, 0, sizeof *d);
    d->cfg = *cfg;
    d->mqtt = mqtt;
    d->clock = clock;
    pthread_mutex_init(&d->lock, NULL);
    hubus_master_init(&d->master, hal, &d->cfg.master);
    d->start_ms = now_ms(d);
    d->bus = HUBUSD_BUS_STARTING;
    d->next_stats_ms = d->start_ms + (uint64_t)d->cfg.mqtt.stats_interval_s * 1000u;
    for (i = 0; i <= HUBUS_MCID_MAX; i++) {
        d->slave[i].online = -1;
    }
    bind_config(d);
}

void hubusd_destroy(hubusd_t *d)
{
    free(d->reload_cfg);
    d->reload_cfg = NULL;
    pthread_mutex_destroy(&d->lock);
}

void hubusd_set_logger(hubusd_t *d, hubusd_log_fn fn, void *ctx)
{
    d->log_fn = fn;
    d->log_ctx = ctx;
}

/* ---- frames from slaves --------------------------------------------------------------------------- */

static void set_online(hubusd_t *d, unsigned mcid, int online)
{
    hubusd_slave_t *s = &d->slave[mcid];

    if (s->online == online) {
        return;
    }
    if (s->online >= 0) {
        log_event(d, online ? 0 : 1, "slave %u (%s) is %s", mcid, slave_name(d, mcid), online ? "online" : "offline");
    }
    s->online = online;
    publish_availability(d, mcid);
}

/* Publishes a frame of a configured slave (event data or a state read) and its interpretation by the profile. */
static void deliver(hubusd_t *d, const hubus_slave_frame_t *f, const char *result, uint8_t lost)
{
    hubusd_slave_t *s = &d->slave[f->mcid];
    char buf[400], data[2 * HUBUS_MAX_PAYLOAD + 1], text[32], t[160];
    jw_t w;

    d->stats.frames++;
    if (!s->configured) {
        if (!s->unknown_seen) {
            s->unknown_seen = 1;
            log_event(d, 1, "data from unconfigured slave %u ignored", f->mcid);
            publish_state(d, 0);
        }
        return;
    }
    s->last_seen_ms = now_ms(d);
    s->ping_failures = 0;
    set_online(d, f->mcid, 1);

    hex(f->payload, f->len, data);
    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_uint(&w, "mcid", f->mcid);
    jw_str(&w, "data", data);
    jw_uint(&w, "len", f->len);
    jw_uint(&w, "gen", f->gen);
    jw_bool(&w, "more", f->more);
    jw_str(&w, "result", result);
    jw_uint(&w, "lost", lost);
    jw_uint(&w, "ts", wall_s(d));
    jw_obj_end(&w);
    topic(d, t, sizeof t, "slave/%u/data", f->mcid);
    pub_json(d, &w, 0, t);

    if (s->profile && hubusd_profile_decode(s->profile, f->payload, f->len, text, sizeof text)) {
        snprintf(s->last_state, sizeof s->last_state, "%s", text);
        pub_topic(d, 1, text, "slave/%u/state", f->mcid);
    }
}

static void on_data(void *ctx, const hubus_slave_frame_t *f, hubus_gen_result_t gen, uint8_t lost)
{
    static const char *const results[] = {"baseline", "normal", "duplicate", "wrapped", "gap"};
    hubusd_t *d = ctx;

    if (gen == HUBUS_GEN_GAP && d->slave[f->mcid].configured) {
        d->stats.gaps++;
    }
    deliver(d, f, results[gen <= HUBUS_GEN_GAP ? gen : 0], lost);
}

static void account_burst(hubusd_t *d, const hubus_burst_stats_t *st)
{
    d->stats.bursts++;
    d->stats.duplicates += st->duplicates;
    d->stats.retries += st->retries;
    d->stats.skipped += st->skipped;
}

static void bus_fault(hubusd_t *d, hubus_status_t r)
{
    if (d->bus != HUBUSD_BUS_LINE_FAULT) {
        log_event(d, 2, "bus fault: %s", hubus_status_str(r));
    }
    d->bus = HUBUSD_BUS_LINE_FAULT;
    snprintf(d->last_error, sizeof d->last_error, "%s", hubus_status_str(r));
    d->next_startup_ms = now_ms(d) + (uint64_t)d->cfg.bus.startup_retry_s * 1000u;
    publish_state(d, 0);
}

/* Runs a burst; returns 0 on success. */
static int run_burst(hubusd_t *d)
{
    hubus_burst_stats_t st;
    hubus_status_t r = hubus_master_burst(&d->master, on_data, d, &st);

    account_burst(d, &st);
    if (r != HUBUS_OK) {
        bus_fault(d, r);
        return -1;
    }
    publish_diagnosis(d, 0);
    return 0;
}

/* ---- probing and scanning ------------------------------------------------------------------------------ */

static void store_info(hubusd_t *d, unsigned mcid, const hubus_slave_frame_t *info)
{
    hubusd_slave_t *s = &d->slave[mcid];
    int changed = !s->have_info || s->info.len != info->len || memcmp(s->info.payload, info->payload, info->len) != 0;

    s->info = *info;
    s->have_info = 1;
    if (changed && s->configured) {
        resolve_profile(d, mcid);
        publish_slave_info(d, mcid);
        publish_slave_discovery(d, mcid);
    }
}

/* REQUEST_INFO to a configured slave. `threshold` consecutive misses mark it offline. */
static void ping(hubusd_t *d, unsigned mcid, unsigned threshold)
{
    hubusd_slave_t *s = &d->slave[mcid];
    hubus_slave_frame_t info;
    hubus_status_t r = hubus_master_request_info(&d->master, (uint8_t)mcid, &info);
    uint64_t now = now_ms(d);

    s->next_ping_ms = now + (uint64_t)(d->cfg.bus.ping_interval_s ? d->cfg.bus.ping_interval_s : 30u) * 1000u;
    if (r == HUBUS_OK) {
        int was_online = s->online == 1;

        s->ping_failures = 0;
        s->last_seen_ms = now;
        store_info(d, mcid, &info);
        set_online(d, mcid, 1);
        if (!was_online) {
            s->needs_refresh = 1;
        }
        return;
    }
    if (r == HUBUS_ERR_LINE_FAULT) {
        bus_fault(d, r);
        return;
    }
    if (++s->ping_failures >= threshold) {
        set_online(d, mcid, 0);
    }
}

/* Reads the live state of `mcid` with REQUEST_STATE (repeated on CRC failures) and publishes it. */
static hubus_status_t read_state(hubusd_t *d, unsigned mcid)
{
    hubus_slave_frame_t f;
    hubus_status_t r;
    unsigned attempts;

    for (attempts = 0;; attempts++) {
        r = hubus_master_request_state(&d->master, (uint8_t)mcid, &f);
        if ((r != HUBUS_ERR_CRC && r != HUBUS_ERR_MISMATCH) || attempts >= d->cfg.master.retry_budget) {
            break;
        }
    }
    if (r == HUBUS_OK) {
        deliver(d, &f, "state", 0);
    } else if (r == HUBUS_ERR_LINE_FAULT) {
        bus_fault(d, r);
    } else {
        log_event(d, 2, "state read of slave %u failed: %s", mcid, hubus_status_str(r));
    }
    return r;
}

/* Brings the published state of the slaves that came online up to date: REQUEST_STATE where the slave has a state
 * (one transaction each), otherwise STATUS_CAPTURE and one burst for all of them. */
static void refresh_states(hubusd_t *d)
{
    unsigned mcid;
    int any = 0;

    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX && d->bus == HUBUSD_BUS_OK; mcid++) {
        hubusd_slave_t *s = &d->slave[mcid];

        if (!s->configured || !s->needs_refresh) {
            continue;
        }
        s->needs_refresh = 0;
        if (s->online != 1) {
            continue;
        }
        if (info_has_state(s)) {
            (void)read_state(d, mcid);
        } else if (info_can_capture(s)) {
            hubus_status_t r = hubus_master_status_capture(&d->master, 6, (uint8_t)mcid);

            if (r == HUBUS_ERR_LINE_FAULT) {
                bus_fault(d, r);
            } else if (r == HUBUS_OK) {
                any = 1;
            }
        }
    }
    if (any && d->bus == HUBUSD_BUS_OK) {
        run_burst(d);
    }
}

/* Individual REQUEST_INFO to one MCID on request (MQTT), configured or not. Publishes the outcome on master/probe. */
static void do_probe(hubusd_t *d, unsigned mcid)
{
    hubusd_slave_t *s = &d->slave[mcid];
    hubus_slave_frame_t info;
    hubus_status_t r;
    unsigned attempts;
    char buf[200], t[160];
    const char *state;
    jw_t w;

    for (attempts = 0;; attempts++) {
        r = hubus_master_request_info(&d->master, (uint8_t)mcid, &info);
        if ((r != HUBUS_ERR_CRC && r != HUBUS_ERR_MISMATCH) || attempts >= d->cfg.master.retry_budget) {
            break;
        }
    }
    if (r == HUBUS_ERR_LINE_FAULT) {
        bus_fault(d, r);
        return;
    }
    d->stats.commands_executed++;
    if (r == HUBUS_OK) {
        state = "present";
        s->last_seen_ms = now_ms(d);
        s->ping_failures = 0;
        if (s->configured) {
            int was_online = s->online == 1;

            store_info(d, mcid, &info);
            set_online(d, mcid, 1);
            if (!was_online) {
                s->needs_refresh = 1;
            }
        } else {
            s->info = info;
            s->have_info = 1;
            if (!s->unknown_seen) {
                s->unknown_seen = 1;
                log_event(d, 1, "unconfigured slave found at MCID %u (device type %d)", mcid, info_device_type(s));
                publish_state(d, 0);
            }
        }
    } else if (r == HUBUS_ERR_NO_RESPONSE) {
        state = "absent";
        if (!s->configured && s->unknown_seen) {
            s->unknown_seen = 0;
            publish_state(d, 0);
        }
    } else {
        state = "suspect"; /* repeated CRC failures: duplicate MCID or line problem */
    }
    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_uint(&w, "mcid", mcid);
    jw_str(&w, "state", state);
    jw_bool(&w, "configured", s->configured);
    if (r == HUBUS_OK && info.len > HUBUS_INFO_APP) {
        jw_uint(&w, "device_type", info.payload[HUBUS_INFO_APP]);
        jw_bool(&w, "state_capable", (info.payload[HUBUS_INFO_CAPS] & HUBUS_CAP_STATE) != 0);
    }
    jw_uint(&w, "ts", wall_s(d));
    jw_obj_end(&w);
    topic(d, t, sizeof t, "master/probe");
    pub_json(d, &w, 0, t);
    publish_diagnosis(d, 0);
    if (s->needs_refresh) {
        refresh_states(d);
    }
}

static void do_scan(hubusd_t *d, int publish)
{
    static hubus_scan_t scan;
    hubus_status_t r = hubus_master_scan(&d->master, &scan);
    unsigned mcid;
    char buf[2400], t[160];
    jw_t w;

    d->stats.scans++;
    if (r != HUBUS_OK) {
        bus_fault(d, r);
        return;
    }
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        hubusd_slave_t *s = &d->slave[mcid];

        if (!s->configured) {
            s->unknown_seen = scan.state[mcid] == HUBUS_SCAN_PRESENT;
            if (s->unknown_seen) {
                s->info = scan.info[mcid];
                s->have_info = 1;
                if (!s->unknown_logged) {
                    s->unknown_logged = 1;
                    log_event(d, 1, "unconfigured slave found at MCID %u (device type %d)", mcid, info_device_type(s));
                }
            }
        } else if (scan.state[mcid] == HUBUS_SCAN_PRESENT) {
            if (s->online != 1) {
                s->needs_refresh = 1;
            }
            s->last_seen_ms = now_ms(d);
            s->ping_failures = 0;
            store_info(d, mcid, &scan.info[mcid]);
            set_online(d, mcid, 1);
        }
    }
    publish_state(d, 0);
    publish_diagnosis(d, 0);
    if (!publish) {
        return;
    }
    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_uint(&w, "ts", wall_s(d));
    jw_arr_begin(&w, "slaves");
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        if (scan.state[mcid] == HUBUS_SCAN_ABSENT) {
            continue;
        }
        jw_obj_begin(&w, NULL);
        jw_uint(&w, "mcid", mcid);
        jw_str(&w, "state", scan.state[mcid] == HUBUS_SCAN_PRESENT ? "present" : "suspect");
        jw_bool(&w, "configured", d->slave[mcid].configured);
        if (scan.state[mcid] == HUBUS_SCAN_PRESENT && scan.info[mcid].len > HUBUS_INFO_APP) {
            jw_uint(&w, "device_type", scan.info[mcid].payload[HUBUS_INFO_APP]);
        }
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    topic(d, t, sizeof t, "master/scan");
    pub_json(d, &w, 0, t);
}

/* ---- bus startup ---------------------------------------------------------------------------------------------- */

static void bus_start(hubusd_t *d)
{
    hubus_status_t r;
    unsigned i;

    d->stats.startups++;
    r = hubus_master_startup(&d->master);
    if (r != HUBUS_OK) {
        bus_fault(d, r);
        return;
    }
    d->bus = HUBUSD_BUS_OK;
    d->last_error[0] = '\0';
    log_event(d, 0, "bus started");
    for (i = 0; i < d->cfg.n_slaves && d->bus == HUBUSD_BUS_OK; i++) {
        ping(d, d->cfg.slaves[i].mcid, 1);
    }
    if (d->bus == HUBUSD_BUS_OK) {
        do_scan(d, 0);
    }
    if (d->bus == HUBUSD_BUS_OK) {
        run_burst(d); /* data that was pending before the reset */
    }
    if (d->bus == HUBUSD_BUS_OK) {
        refresh_states(d);
    }
    d->next_scan_ms = now_ms(d) + (uint64_t)d->cfg.bus.scan_interval_s * 1000u;
    d->started = 1;
    publish_state(d, 0);
}

/* ---- commands --------------------------------------------------------------------------------------------------------- */

static void reject(hubusd_t *d, const char *fmt, ...)
{
    char msg[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    d->stats.commands_rejected++;
    log_event(d, 1, "command rejected: %s", msg);
}

/* `read` topic: REQUEST_STATE for one slave. */
static void read_exact(hubusd_t *d, unsigned mcid)
{
    const hubusd_slave_t *s = &d->slave[mcid];

    if (!s->configured) {
        reject(d, "slave %u is not configured", mcid);
    } else if (!info_has_state(s)) {
        reject(d, "slave %u has no state (use the capture topic)", mcid);
    } else if (read_state(d, mcid) == HUBUS_OK) {
        d->stats.commands_executed++;
    }
}

static void capture_exact(hubusd_t *d, unsigned mcid)
{
    hubus_status_t r;

    if (!info_can_capture(&d->slave[mcid])) {
        reject(d, "slave %u does not support STATUS_CAPTURE", mcid);
        return;
    }
    r = hubus_master_status_capture(&d->master, 6, (uint8_t)mcid);
    if (r != HUBUS_OK) {
        log_event(d, 2, "status capture of slave %u failed: %s", mcid, hubus_status_str(r));
        if (r == HUBUS_ERR_LINE_FAULT) {
            bus_fault(d, r);
        }
        return;
    }
    d->stats.commands_executed++;
    run_burst(d);
}

/* `expected` is the state the command establishes (for readback: cache), NULL if it is not known. */
static void send_exact(hubusd_t *d, unsigned mcid, uint8_t opcode, const uint8_t *args, uint8_t n, const char *expected)
{
    const hubusd_slave_t *s = &d->slave[mcid];
    hubus_status_t r;

    if (s->have_info && 1u + n > s->info.payload[HUBUS_INFO_MAX_CMD]) {
        reject(d, "command of %u bytes exceeds the %u bytes slave %u accepts", 1u + n, s->info.payload[HUBUS_INFO_MAX_CMD], mcid);
        return;
    }
    r = hubus_master_send_command(&d->master, 6, (uint8_t)mcid, opcode, args, n);
    if (r != HUBUS_OK) {
        log_event(d, 2, "command to slave %u failed: %s", mcid, hubus_status_str(r));
        if (r == HUBUS_ERR_LINE_FAULT) {
            bus_fault(d, r);
        }
        return;
    }
    d->stats.commands_executed++;
    if (slave_readback(d, mcid) == HUBUSD_READBACK_CACHE) {
        /* no contact with the slave: report what the command establishes */
        if (expected) {
            hubusd_slave_t *w = &d->slave[mcid];

            snprintf(w->last_state, sizeof w->last_state, "%s", expected);
            pub_topic(d, 1, expected, "slave/%u/state", mcid);
        }
    } else if (slave_readback(d, mcid) == HUBUSD_READBACK_SLAVE && info_has_state(s)) {
        /* commands are never acknowledged: read the state back, in one transaction */
        (void)read_state(d, mcid);
    } else if (slave_readback(d, mcid) == HUBUSD_READBACK_SLAVE && info_can_capture(s)) {
        /* a slave without a state captures it into the buffer, which the burst collects */
        r = hubus_master_status_capture(&d->master, 6, (uint8_t)mcid);
        if (r == HUBUS_OK) {
            run_burst(d);
        } else if (r == HUBUS_ERR_LINE_FAULT) {
            bus_fault(d, r);
        }
    }
}

static void exec_request(hubusd_t *d, const hubusd_request_t *q)
{
    hubus_status_t r;

    switch (q->kind) {
    case HUBUSD_REQ_REJECT:
        reject(d, "%s", q->text);
        break;
    case HUBUSD_REQ_STATS:
        publish_stats(d);
        {
            unsigned mcid;
            for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
                if (d->slave[mcid].configured) {
                    publish_slave_stats(d, mcid);
                }
            }
        }
        break;
    case HUBUSD_REQ_SCAN:
        do_scan(d, 1);
        if (d->bus == HUBUSD_BUS_OK) {
            refresh_states(d);
        }
        break;
    case HUBUSD_REQ_CAPTURE:
        capture_exact(d, q->prefix);
        break;
    case HUBUSD_REQ_READ:
        read_exact(d, q->prefix);
        break;
    case HUBUSD_REQ_PROBE:
        do_probe(d, q->prefix);
        break;
    case HUBUSD_REQ_SET: {
        const hubusd_slave_t *s = &d->slave[q->prefix];
        hubusd_cmd_t c;

        if (!s->configured) {
            reject(d, "slave %u is not configured", q->prefix);
        } else if (!s->profile) {
            reject(d, "slave %u has no profile; use the raw command topic", q->prefix);
        } else if (!hubusd_profile_command(s->profile, q->text, &c)) {
            reject(d, "'%s' is not a valid command for profile '%s'", q->text, s->profile->name);
        } else {
            send_exact(d, q->prefix, c.opcode, c.args, c.n_args, strcasecmp(q->text, "ON") == 0 ? "ON" : "OFF");
        }
        break;
    }
    case HUBUSD_REQ_RAW:
        if (q->target_len == HUBUS_TARGET_LEN_EXACT) {
            if (q->opcode == HUBUS_OP_STATUS_CAPTURE) {
                capture_exact(d, q->prefix);
            } else {
                send_exact(d, q->prefix, q->opcode, q->args, q->n_args, NULL);
            }
        } else {
            r = hubus_master_send_command(&d->master, q->target_len, q->prefix, q->opcode, q->args, q->n_args);
            if (r != HUBUS_OK) {
                log_event(d, 2, "group command failed: %s", hubus_status_str(r));
                if (r == HUBUS_ERR_LINE_FAULT) {
                    bus_fault(d, r);
                }
            } else {
                d->stats.commands_executed++;
                if (q->opcode == HUBUS_OP_STATUS_CAPTURE) {
                    run_burst(d); /* the captured states are collected as usual */
                }
            }
        }
        break;
    default:
        break;
    }
}

/* ---- incoming MQTT messages (MQTT thread) --------------------------------------------------------------------------------- */

static int parse_uint(const char *s, unsigned max, unsigned *out, const char **end)
{
    char *e;
    unsigned long v;

    if (!isdigit((unsigned char)*s)) {
        return 0;
    }
    v = strtoul(s, &e, 10);
    if (v > max) {
        return 0;
    }
    *out = (unsigned)v;
    *end = e;
    return 1;
}

/* "0x10 1 2" or "16,1,2": opcode first. Returns 0 on success. */
static int parse_raw(const char *text, hubusd_request_t *r)
{
    const char *p = text;
    unsigned n = 0;

    while (*p) {
        char *end;
        long v;

        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        }
        if (!*p) {
            break;
        }
        v = strtol(p, &end, 0);
        if (end == p || v < 0 || v > 255 || (*end && !strchr(" ,\t\r\n", *end))) {
            return -1;
        }
        if (n >= HUBUS_MAX_PAYLOAD) {
            return -1;
        }
        if (n == 0) {
            r->opcode = (uint8_t)v;
        } else {
            r->args[n - 1u] = (uint8_t)v;
        }
        n++;
        p = end;
    }
    if (n == 0) {
        return -1;
    }
    r->n_args = (uint8_t)(n - 1u);
    return 0;
}

void hubusd_handle_message(hubusd_t *d, const char *topic_in, const void *payload, size_t len)
{
    const char *base = d->cfg.mqtt.base_topic;
    size_t bl = strlen(base);
    const char *p;
    char text[PAYLOAD_MAX];
    hubusd_request_t r;
    unsigned a, b;

    if (strncmp(topic_in, base, bl) != 0 || topic_in[bl] != '/') {
        return;
    }
    p = topic_in + bl + 1u;
    if (len >= sizeof text) {
        enqueue_reject(d, "payload of %zu bytes on '%s' is too long", len, topic_in);
        return;
    }
    if (len) { /* an empty MQTT payload arrives as NULL */
        memcpy(text, payload, len);
    }
    text[len] = '\0';
    memset(&r, 0, sizeof r);

    if (strcmp(p, "master/cmd/scan") == 0) {
        r.kind = HUBUSD_REQ_SCAN;
        enqueue(d, &r);
    } else if (strcmp(p, "master/cmd/probe") == 0) {
        const char *end;

        if (!parse_uint(text, 63, &a, &end) || *end || a < HUBUS_MCID_MIN) {
            enqueue_reject(d, "invalid MCID '%s' for probe (1..63)", text);
            return;
        }
        r.kind = HUBUSD_REQ_PROBE;
        r.target_len = HUBUS_TARGET_LEN_EXACT;
        r.prefix = (uint8_t)a;
        enqueue(d, &r);
    } else if (strcmp(p, "master/cmd/stats") == 0) {
        r.kind = HUBUSD_REQ_STATS;
        enqueue(d, &r);
    } else if (strncmp(p, "slave/", 6) == 0) {
        const char *end;

        if (!parse_uint(p + 6, 999, &a, &end) || *end != '/') {
            enqueue_reject(d, "invalid topic '%s'", topic_in);
            return;
        }
        if (a < HUBUS_MCID_MIN || a > HUBUS_MCID_MAX) {
            enqueue_reject(d, "MCID %u out of range 1..63 in '%s'", a, topic_in);
            return;
        }
        r.target_len = HUBUS_TARGET_LEN_EXACT;
        r.prefix = (uint8_t)a;
        end++;
        if (strcmp(end, "set") == 0) {
            r.kind = HUBUSD_REQ_SET;
            if (strlen(text) >= sizeof r.text) {
                enqueue_reject(d, "set payload for slave %u is too long", a);
                return;
            }
            memcpy(r.text, text, strlen(text) + 1u);
        } else if (strcmp(end, "capture") == 0) {
            r.kind = HUBUSD_REQ_CAPTURE;
        } else if (strcmp(end, "read") == 0) {
            r.kind = HUBUSD_REQ_READ;
        } else if (strcmp(end, "cmd") == 0) {
            r.kind = HUBUSD_REQ_RAW;
            if (parse_raw(text, &r) != 0 || (r.opcode != HUBUS_OP_STATUS_CAPTURE && r.opcode < HUBUS_OP_APP_MIN) ||
                (r.opcode == HUBUS_OP_STATUS_CAPTURE && r.n_args != 0)) {
                enqueue_reject(d, "invalid raw command '%s' for slave %u (opcode 0x00 or >= 0x10, bytes 0..255)", text, a);
                return;
            }
        } else {
            return; /* one of our own state topics */
        }
        enqueue(d, &r);
    } else if (strncmp(p, "group/", 6) == 0) {
        const char *end;

        if (!parse_uint(p + 6, 63, &a, &end) || *end != '/' || !parse_uint(end + 1, 5, &b, &end) ||
            strcmp(end, "/cmd") != 0) {
            enqueue_reject(d, "invalid group topic '%s' (group/<prefix 0..63>/<length 0..5>/cmd)", topic_in);
            return;
        }
        r.kind = HUBUSD_REQ_RAW;
        r.target_len = (uint8_t)b;
        r.prefix = (uint8_t)a;
        if (parse_raw(text, &r) != 0 || (r.opcode != HUBUS_OP_STATUS_CAPTURE && r.opcode < HUBUS_OP_APP_MIN) ||
            (r.opcode == HUBUS_OP_STATUS_CAPTURE && r.n_args != 0)) {
            enqueue_reject(d, "invalid raw command '%s' for group %u/%u", text, a, b);
            return;
        }
        enqueue(d, &r);
    }
}

void hubusd_on_connect(hubusd_t *d)
{
    pthread_mutex_lock(&d->lock);
    d->republish = 1;
    pthread_mutex_unlock(&d->lock);
}

void hubusd_request_reload(hubusd_t *d, hubusd_config_t *new_cfg)
{
    pthread_mutex_lock(&d->lock);
    free(d->reload_cfg);
    d->reload_cfg = new_cfg;
    d->reload_pending = 1;
    pthread_mutex_unlock(&d->lock);
}

void hubusd_subscriptions(const hubusd_t *d, void (*fn)(void *ctx, const char *filter), void *ctx)
{
    static const char *const filters[] = {"master/cmd/+", "slave/+/set", "slave/+/cmd", "slave/+/capture", "slave/+/read",
                                          "group/+/+/cmd"};
    char t[160];
    unsigned i;

    for (i = 0; i < sizeof filters / sizeof filters[0]; i++) {
        topic(d, t, sizeof t, "%s", filters[i]);
        fn(ctx, t);
    }
}

size_t hubusd_status_topic(const hubusd_t *d, char *out, size_t out_size)
{
    topic(d, out, out_size, "master/status");
    return strlen(out);
}

/* ---- reload ------------------------------------------------------------------------------------------------------------------ */

static void apply_reload(hubusd_t *d, hubusd_config_t *nc)
{
    unsigned mcid;
    hubusd_slave_t old[HUBUS_MCID_MAX + 1u];

    memcpy(old, d->slave, sizeof old);
    /* Remove discovery entries of slaves that are gone or change their profile. */
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        if (old[mcid].configured && old[mcid].profile) {
            hubusd_profile_t keep = *old[mcid].profile;
            int still = 0;
            unsigned i;

            for (i = 0; i < nc->n_slaves; i++) {
                still |= nc->slaves[i].mcid == mcid;
            }
            if (!still) {
                remove_slave_discovery(d, mcid, &keep);
                pub_topic(d, 1, "", "slave/%u/availability", mcid);
                pub_topic(d, 1, "", "slave/%u/state", mcid);
                pub_topic(d, 1, "", "slave/%u/info", mcid);
            } else {
                remove_slave_discovery(d, mcid, &keep); /* republished below, possibly with another component */
            }
        }
    }
    d->cfg.n_slaves = nc->n_slaves;
    memcpy(d->cfg.slaves, nc->slaves, sizeof d->cfg.slaves);
    d->cfg.n_profiles = nc->n_profiles;
    memcpy(d->cfg.profiles, nc->profiles, sizeof d->cfg.profiles);
    d->cfg.bus = nc->bus;
    d->cfg.mqtt.stats_interval_s = nc->mqtt.stats_interval_s;
    d->cfg.mqtt.discovery = nc->mqtt.discovery;
    bind_config(d);
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        d->slave[mcid].have_info = old[mcid].have_info;
        d->slave[mcid].info = old[mcid].info;
        d->slave[mcid].online = old[mcid].online;
        d->slave[mcid].last_seen_ms = old[mcid].last_seen_ms;
        memcpy(d->slave[mcid].last_state, old[mcid].last_state, sizeof old[mcid].last_state);
        d->slave[mcid].unknown_seen = d->slave[mcid].configured ? 0 : old[mcid].unknown_seen;
        d->slave[mcid].unknown_logged = old[mcid].unknown_logged;
        if (d->slave[mcid].configured) {
            resolve_profile(d, mcid);
            if (!old[mcid].configured) {
                d->slave[mcid].needs_refresh = 1; /* new slave: probe and read its state */
                d->slave[mcid].online = -1;
                d->slave[mcid].next_ping_ms = now_ms(d);
            }
        }
    }
    log_event(d, 0, "configuration reloaded: %u slaves", d->cfg.n_slaves);
    publish_all_retained(d);
}

/* ---- main step ------------------------------------------------------------------------------------------------------------------ */

int hubusd_step(hubusd_t *d)
{
    uint64_t now = now_ms(d);
    hubusd_request_t q;
    unsigned handled = 0;
    int busy = 0;
    int republish, reload;
    hubusd_config_t *nc = NULL;
    unsigned mcid;

    pthread_mutex_lock(&d->lock);
    republish = d->republish;
    d->republish = 0;
    reload = d->reload_pending;
    if (reload) {
        nc = d->reload_cfg;
        d->reload_cfg = NULL;
        d->reload_pending = 0;
    }
    pthread_mutex_unlock(&d->lock);

    if (reload && nc) {
        apply_reload(d, nc);
        free(nc);
        busy = 1;
    }
    if (republish) {
        publish_all_retained(d);
        busy = 1;
    }

    if (d->bus != HUBUSD_BUS_OK) {
        if (!d->started && d->bus == HUBUSD_BUS_STARTING) {
            publish_master_status(d, 1);
            if (d->cfg.mqtt.discovery && d->cfg.mqtt.discovery_cleanup) {
                for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
                    if (!d->slave[mcid].configured) {
                        remove_slave_discovery(d, mcid, NULL);
                    }
                }
            }
            publish_all_retained(d);
            bus_start(d);
            return 1;
        }
        if (d->bus == HUBUSD_BUS_LINE_FAULT && now >= d->next_startup_ms) {
            bus_start(d);
            if (d->bus == HUBUSD_BUS_OK) {
                publish_all_retained(d);
            }
            return 1;
        }
        /* Commands cannot be executed now; report them instead of letting them pile up. */
        while (dequeue(d, &q)) {
            if (q.kind == HUBUSD_REQ_STATS) {
                publish_stats(d);
            } else {
                reject(d, "bus is %s", bus_text(d));
            }
        }
        if (d->cfg.mqtt.stats_interval_s && now >= d->next_stats_ms) {
            d->next_stats_ms = now + (uint64_t)d->cfg.mqtt.stats_interval_s * 1000u;
            publish_stats(d);
        }
        return busy;
    }

    while (handled < 4 && dequeue(d, &q)) {
        exec_request(d, &q);
        handled++;
        busy = 1;
        if (d->bus != HUBUSD_BUS_OK) {
            return 1;
        }
    }

    {
        hubus_burst_stats_t st;
        hubus_status_t r = hubus_master_service(&d->master, on_data, d, &st);

        if (r != HUBUS_OK) {
            bus_fault(d, r);
            return 1;
        }
        if (st.polls) {
            account_burst(d, &st);
            publish_diagnosis(d, 0);
            busy = 1;
        }
    }

    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX && d->cfg.bus.ping_interval_s; mcid++) {
        if (d->slave[mcid].configured && now >= d->slave[mcid].next_ping_ms) {
            ping(d, mcid, d->cfg.bus.ping_failures);
            refresh_states(d);
            publish_state(d, 0);
            busy = 1;
            break; /* one probe per step keeps the latency for commands low */
        }
    }
    /* Periodic state read for slaves that never announce changes (poll_interval_s): REQUEST_STATE, or STATUS_CAPTURE
     * and a burst for a slave without a state. */
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX && d->bus == HUBUSD_BUS_OK; mcid++) {
        hubusd_slave_t *s = &d->slave[mcid];
        unsigned interval = s->configured ? d->cfg.slaves[s->cfg_index].poll_interval_s : 0u;

        if (interval && now >= s->next_poll_ms) {
            s->next_poll_ms = now + (uint64_t)interval * 1000u;
            if (s->online == 1) {
                s->needs_refresh = 1;
                refresh_states(d);
                busy = 1;
                break; /* one read per step, like the pings */
            }
        }
    }
    if (d->bus == HUBUSD_BUS_OK && d->cfg.bus.scan_interval_s && now >= d->next_scan_ms) {
        d->next_scan_ms = now + (uint64_t)d->cfg.bus.scan_interval_s * 1000u;
        do_scan(d, 0);
        refresh_states(d);
        busy = 1;
    }
    if (d->cfg.mqtt.stats_interval_s && now >= d->next_stats_ms) {
        d->next_stats_ms = now + (uint64_t)d->cfg.mqtt.stats_interval_s * 1000u;
        publish_stats(d);
        for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
            if (d->slave[mcid].configured) {
                publish_slave_stats(d, mcid);
            }
        }
    }
    return busy;
}

void hubusd_shutdown(hubusd_t *d)
{
    unsigned mcid;

    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        if (d->slave[mcid].configured) {
            d->slave[mcid].online = 0;
            publish_availability(d, mcid);
        }
    }
    publish_master_status(d, 0);
    if (d->bus == HUBUSD_BUS_OK) {
        (void)hubus_master_shutdown(&d->master);
    }
}
