#include "hubusd_core.h"
#include "hubusd_modules.h"
#include "hubusd_worker.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hubus_frame.h"
#include "hubusd_json.h"

#define PAYLOAD_MAX 1024
#define MODULE_STOP_WAIT_MS 2000u

static void reject(hubusd_t *d, const char *fmt, ...);

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

/* The type byte of the hardware identifier. */
static int info_device_type(const hubusd_slave_t *s)
{
    return (s->have_info && s->info.len > HUBUS_INFO_HWID) ? s->info.payload[HUBUS_INFO_HWID + HUBUS_HWID_TYPE] : -1;
}

static hubus_hwid_form_t info_hwid_form(const hubus_slave_frame_t *info)
{
    return info->len > HUBUS_INFO_HWID ? hubus_hwid_form(&info->payload[HUBUS_INFO_HWID], (uint8_t)(info->len - HUBUS_INFO_HWID))
                                       : HUBUS_HWID_INVALID;
}

/* Field `field` of a full-form HWID, -1 otherwise. */
static int info_field(const hubusd_slave_t *s, unsigned field)
{
    return s->have_info && info_hwid_form(&s->info) == HUBUS_HWID_FULL ? s->info.payload[HUBUS_INFO_HWID + field] : -1;
}

/* The serial number of a full-form HWID as hex text. Returns 0 if it has none. */
static int info_serial(const hubusd_slave_t *s, char *out)
{
    const unsigned start = HUBUS_INFO_HWID + HUBUS_HWID_SERIAL;

    if (!s->have_info || info_hwid_form(&s->info) != HUBUS_HWID_FULL || s->info.len <= start) {
        return 0;
    }
    hex(&s->info.payload[start], (unsigned)s->info.len - start, out);
    return 1;
}

static int info_can_capture(const hubusd_slave_t *s)
{
    return !s->have_info || (s->module && s->module->has_status_capture);
}

/* The slave exposes a live state: REQUEST_STATE reads it in one transaction. */
static int info_has_state(const hubusd_slave_t *s)
{
    return s->module && s->module->has_state;
}

/* ---- module functions ------------------------------------------------------------------------------------------- */

static int module_has_functions(const hubus_module_t *m)
{
    return m->create != NULL || m->on_frame != NULL || m->on_set != NULL;
}

/* The worker of a module, started on demand. NULL if the module has none (yet). */
static hubusd_worker_t *worker_of(hubusd_t *d, const hubus_module_t *m, int start)
{
    size_t k, slot = HUBUS_MODULE_SET_MAX;

    for (k = 0; k < HUBUS_MODULE_SET_MAX; k++) {
        if (d->worker[k] && hubusd_worker_module(d->worker[k]) == m) {
            return d->worker[k];
        }
        if (!d->worker[k] && slot == HUBUS_MODULE_SET_MAX) {
            slot = k;
        }
    }
    if (!start || d->stopping || slot == HUBUS_MODULE_SET_MAX) {
        return NULL;
    }
    d->worker[slot] = hubusd_worker_start(m);
    return d->worker[slot];
}

static int module_failed(hubusd_t *d, const hubus_module_t *m)
{
    hubusd_worker_t *w = m ? worker_of(d, m, 0) : NULL;

    return w && hubusd_worker_failed(w);
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

/* Blind time after a command in milliseconds (docs/spec/06-operation.md section 6.9): the value of the module plus one
 * tick, so that a partly elapsed millisecond never shortens it. A slave with a blind time keeps quiet after every command,
 * so also an opcode without one needs that tick: the end of the blind time brings the slave back. 0: never blind. */
static uint64_t blind_ms(const hubusd_slave_t *s, uint8_t opcode)
{
    if (!s->module || !hubus_module_has_blind_time(s->module)) {
        return 0;
    }
    return (uint64_t)hubus_module_blind_ms(s->module, opcode) + 1u;
}

static int is_blind(const hubusd_t *d, unsigned mcid)
{
    return d->slave[mcid].blind_until_ms != 0;
}

/* 1 if a frame to `target_len`/`prefix` would reach a slave that is blind (6: exact). */
static int target_blind(const hubusd_t *d, uint8_t target_len, uint8_t prefix)
{
    unsigned mcid;

    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        if (is_blind(d, mcid) && hubus_mcid_match((uint8_t)mcid, prefix, target_len)) {
            return 1;
        }
    }
    return 0;
}

static int any_blind(const hubusd_t *d)
{
    return target_blind(d, 0, 0);
}

/* A command reached the slaves of `target_len`/`prefix`: those with a blind time are blind from now on. */
static void command_sent(hubusd_t *d, uint8_t target_len, uint8_t prefix, uint8_t opcode)
{
    uint64_t now = now_ms(d);
    unsigned mcid;

    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        hubusd_slave_t *s = &d->slave[mcid];
        uint64_t ms = blind_ms(s, opcode);

        if (ms && hubus_mcid_match((uint8_t)mcid, prefix, target_len)) {
            s->blind_until_ms = now + ms;
        }
    }
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

static int peek(hubusd_t *d, hubusd_request_t *r)
{
    int ok = 0;

    pthread_mutex_lock(&d->lock);
    if (d->q_count > 0) {
        *r = d->queue[d->q_head];
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

static int queue_has_room(hubusd_t *d)
{
    return queue_depth(d) < HUBUSD_QUEUE_LEN;
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
        if (info_serial(s, serial)) {
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
    char buf[1280];
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
    jw_uint(&w, "module_jobs_dropped", d->stats.module_jobs_dropped);
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
    char buf[640];
    uint8_t i;
    char t[160];
    char hwid[2 * HUBUS_MAX_PAYLOAD + 1];
    char serial[2 * HUBUS_MAX_PAYLOAD + 1];
    jw_t w;
    const hubusd_slave_t *s = &d->slave[mcid];
    const hubus_module_t *m = s->module;

    if (!s->have_info) {
        return;
    }
    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_uint(&w, "mcid", mcid);
    jw_str(&w, "name", slave_name(d, mcid));
    jw_uint(&w, "protocol", HUBUS_INFO_VERSION(s->info.payload[HUBUS_INFO_BYTE0]));
    jw_uint(&w, "speed_class", HUBUS_INFO_SPEED_CLASS(s->info.payload[HUBUS_INFO_BYTE0]));
    hex(&s->info.payload[HUBUS_INFO_HWID], s->info.len > HUBUS_INFO_HWID ? (unsigned)s->info.len - HUBUS_INFO_HWID : 0u, hwid);
    jw_str(&w, "hwid", hwid);
    if (info_device_type(s) >= 0) {
        jw_uint(&w, "device_type", (uint64_t)info_device_type(s));
    }
    if (info_field(s, HUBUS_HWID_VERSION) >= 0) {
        jw_uint(&w, "variant", (uint64_t)info_field(s, HUBUS_HWID_VARIANT));
        jw_uint(&w, "firmware", (uint64_t)info_field(s, HUBUS_HWID_VERSION));
        jw_uint(&w, "revision", (uint64_t)info_field(s, HUBUS_HWID_REVISION));
    }
    if (info_serial(s, serial)) {
        jw_str(&w, "serial", serial);
    }
    jw_str(&w, "module", m ? m->name : "");
    if (m) {
        jw_bool(&w, "events", m->has_events);
        jw_bool(&w, "status_capture", m->has_status_capture);
        jw_bool(&w, "state", m->has_state);
        jw_bool(&w, "update", m->has_update);
        jw_uint(&w, "max_command", m->max_command);
        jw_bool(&w, "blind", hubus_module_has_blind_time(m));
        jw_bool(&w, "module_failed", module_failed(d, m));
    }
    jw_bool(&w, "updater", info_device_type(s) == HUBUS_HWID_TYPE_UPDATER);
    jw_arr_begin(&w, "entities");
    for (i = 0; m && i < m->n_entities; i++) {
        jw_str(&w, NULL, m->entities[i].key);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    topic(d, t, sizeof t, "slave/%u/info", mcid);
    pub_json(d, &w, 1, t);
}

/* ---- Home Assistant discovery -------------------------------------------------------------- */

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

static int entity_has_discovery(const hubus_entity_t *e)
{
    return e->component < HUBUS_COMPONENT_COUNT;
}

static void entity_object(unsigned mcid, const hubus_entity_t *e, char *out, size_t size)
{
    snprintf(out, size, "hubus_%u_%s", mcid, e->key);
}

static void publish_entity_discovery(hubusd_t *d, unsigned mcid, const hubus_entity_t *e)
{
    char buf[1400];
    char t[160], tt[160], object[64], master[96], serial[2 * HUBUS_MAX_PAYLOAD + 1], fw[16], dev[48];
    const hubusd_slave_t *s = &d->slave[mcid];
    const char *text;
    jw_t w;

    entity_object(mcid, e, object, sizeof object);
    snprintf(dev, sizeof dev, "hubus_%u", mcid);
    master_identifier(d, master, sizeof master);

    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    if (s->module->n_entities == 1u) {
        jw_null(&w, "name"); /* the entity carries the name of the device */
    } else {
        jw_str(&w, "name", e->name);
    }
    jw_bool(&w, "has_entity_name", 1);
    jw_str(&w, "unique_id", object);
    topic(d, t, sizeof t, "slave/%u/%s/state", mcid, e->key);
    jw_str(&w, "state_topic", t);
    if (e->component == HUBUS_COMPONENT_SWITCH || e->component == HUBUS_COMPONENT_LIGHT ||
        e->component == HUBUS_COMPONENT_TEXT) {
        topic(d, t, sizeof t, "slave/%u/%s/set", mcid, e->key);
        jw_str(&w, "command_topic", t);
    }
    if (e->component == HUBUS_COMPONENT_LIGHT) {
        topic(d, t, sizeof t, "slave/%u/%s/rgb/set", mcid, e->key);
        jw_str(&w, "rgb_command_topic", t);
    }
    if ((text = hubusd_unit_text(e->unit)) != NULL) {
        jw_str(&w, "unit_of_measurement", text);
    }
    if ((text = hubusd_device_class_name(e->device_class)) != NULL) {
        jw_str(&w, "device_class", text);
    }
    if (e->component == HUBUS_COMPONENT_SENSOR) {
        jw_str(&w, "state_class", "measurement");
    } else if (e->component == HUBUS_COMPONENT_TEXT) {
        jw_uint(&w, "max", HUBUS_MODULE_TEXT_MAX);
    } else {
        jw_str(&w, "payload_on", "ON");
        jw_str(&w, "payload_off", "OFF");
        if (e->component == HUBUS_COMPONENT_SWITCH) {
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
    jw_str(&w, NULL, dev);
    jw_arr_end(&w);
    jw_str(&w, "name", slave_name(d, mcid));
    jw_str(&w, "manufacturer", "HUBUS");
    jw_str(&w, "model", s->module->name);
    if (info_field(s, HUBUS_HWID_VERSION) >= 0) {
        snprintf(fw, sizeof fw, "%d", info_field(s, HUBUS_HWID_VERSION));
        jw_str(&w, "sw_version", fw);
    }
    if (info_serial(s, serial)) {
        jw_str(&w, "serial_number", serial);
    }
    jw_str(&w, "via_device", master);
    jw_obj_end(&w);
    jw_obj_end(&w);
    discovery_topic(d, tt, sizeof tt, hubusd_component_name(e->component), object);
    pub_json(d, &w, 1, tt);
}

static void publish_slave_discovery(hubusd_t *d, unsigned mcid)
{
    const hubusd_slave_t *s = &d->slave[mcid];
    uint8_t i;

    if (!d->cfg.mqtt.discovery || !s->configured || !s->module) {
        return;
    }
    for (i = 0; i < s->module->n_entities; i++) {
        if (entity_has_discovery(&s->module->entities[i])) {
            publish_entity_discovery(d, mcid, &s->module->entities[i]);
        }
    }
}

static void remove_entity_discovery(hubusd_t *d, unsigned mcid, const hubus_entity_t *e)
{
    char tt[160], object[64];

    entity_object(mcid, e, object, sizeof object);
    discovery_topic(d, tt, sizeof tt, hubusd_component_name(e->component), object);
    pub(d, tt, "", 1);
}

/* True if an entity of the same kind and key came earlier in the list of modules. */
static int entity_listed_before(const hubusd_t *d, size_t k, uint8_t i)
{
    const hubus_entity_t *e = &d->modules[k]->entities[i];
    size_t k2;
    uint8_t i2;

    for (k2 = 0; k2 <= k; k2++) {
        for (i2 = 0; i2 < d->modules[k2]->n_entities && (k2 < k || i2 < i); i2++) {
            const hubus_entity_t *o = &d->modules[k2]->entities[i2];

            if (o->component == e->component && strcmp(o->key, e->key) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* The discovery entries of a slave. Without a module (an MCID nobody configured) every entity that any module could have
 * published, and the single entry per slave that earlier versions published. */
static void remove_slave_discovery(hubusd_t *d, unsigned mcid, const hubus_module_t *m)
{
    static const char *const legacy[] = {"switch", "binary_sensor", "sensor"};
    char tt[160], object[48];
    size_t k;
    uint8_t i;

    if (!d->cfg.mqtt.discovery) {
        return;
    }
    if (m) {
        for (i = 0; i < m->n_entities; i++) {
            remove_entity_discovery(d, mcid, &m->entities[i]);
        }
        return;
    }
    for (k = 0; k < d->n_modules; k++) {
        for (i = 0; i < d->modules[k]->n_entities; i++) {
            if (!entity_listed_before(d, k, i)) {
                remove_entity_discovery(d, mcid, &d->modules[k]->entities[i]);
            }
        }
    }
    snprintf(object, sizeof object, "hubus_%u", mcid);
    for (i = 0; i < sizeof legacy / sizeof legacy[0]; i++) {
        discovery_topic(d, tt, sizeof tt, legacy[i], object);
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
    uint8_t i;

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
        for (i = 0; s->module && i < s->module->n_entities; i++) {
            if (s->last_state[i][0]) {
                pub_topic(d, 1, s->last_state[i], "slave/%u/%s/state", mcid, s->module->entities[i].key);
            }
        }
    }
}

/* ---- configuration binding ---------------------------------------------------------------------- */

/* The module of a slave by its HWID (docs/spec/06-operation.md section 6.10). */
static void resolve_module(hubusd_t *d, unsigned mcid)
{
    hubusd_slave_t *s = &d->slave[mcid];
    hubus_module_match_t r;

    s->module = NULL;
    if (!s->have_info) {
        return;
    }
    r = hubus_module_select_info(d->modules, d->n_modules, s->info.payload, s->info.len, &s->module);
    if (r == HUBUS_MODULE_CONFLICT) {
        log_event(d, 2, "slave %u: modules conflict for its hardware identifier, not taken into operation", mcid);
    } else if (r != HUBUS_MODULE_FOUND) {
        log_event(d, 2, "slave %u: %s hardware identifier, not taken into operation", mcid,
                  r == HUBUS_MODULE_UNKNOWN ? "no module for its" : "invalid");
    }
}

/* ---- instances of module functions, and what they produce ------------------------------------------------------------- */

/* Queues a create or destroy. False only if it has to be tried again: the queue is full now. A module that has failed is
 * not called any more, so there is nothing to wait for. */
static int post_lifecycle(hubusd_t *d, hubusd_worker_t *w, hubusd_job_kind_t kind, unsigned mcid)
{
    hubusd_job_t job;

    memset(&job, 0, sizeof job);
    job.kind = kind;
    job.mcid = (uint8_t)mcid;
    return hubusd_worker_post(w, &job) || hubusd_worker_failed(w) || d->stopping;
}

/* Keeps one instance per slave whose module has functions: creates the ones that are missing, destroys the ones whose slave
 * or module is gone. Only queues jobs; the worker does the work. A create or destroy never gets lost: what does not fit into
 * a queue now is done in a later step. */
static void sync_instances(hubusd_t *d)
{
    unsigned mcid;

    d->sync_again = 0;
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        hubusd_slave_t *s = &d->slave[mcid];
        const hubus_module_t *want = (!d->stopping && s->configured && s->module && module_has_functions(s->module)) ? s->module : NULL;
        hubusd_worker_t *w;

        if (want == s->inst_module) {
            continue;
        }
        if (s->inst_module && (w = worker_of(d, s->inst_module, 0)) != NULL &&
            !post_lifecycle(d, w, HUBUSD_JOB_DESTROY, mcid)) {
            d->sync_again = 1;
            continue;
        }
        s->inst_module = NULL;
        if (!want) {
            continue;
        }
        w = worker_of(d, want, 1);
        if (!w) {
            log_event(d, 2, "module %s: no instance for slave %u: its thread cannot be started", want->name, mcid);
        } else if (hubusd_worker_failed(w)) {
            log_event(d, 2, "module %s: no instance for slave %u: the module has failed", want->name, mcid);
        } else if (post_lifecycle(d, w, HUBUSD_JOB_CREATE, mcid)) {
            s->inst_module = want;
        } else {
            d->sync_again = 1;
        }
    }
}

/* Passes a frame of a slave to the on_frame function of its module. A repeated frame is not news for the module. */
static void post_frame(hubusd_t *d, hubusd_slave_t *s, const hubus_slave_frame_t *f, const char *result, uint8_t lost)
{
    hubusd_worker_t *w;
    hubusd_job_t job;

    if (!s->inst_module || !s->inst_module->on_frame || strcmp(result, "duplicate") == 0 ||
        !(w = worker_of(d, s->inst_module, 0)) || hubusd_worker_failed(w)) {
        return;
    }
    memset(&job, 0, sizeof job);
    job.kind = HUBUSD_JOB_FRAME;
    job.mcid = f->mcid;
    job.frame_kind = strcmp(result, "state") == 0 ? HUBUS_FRAME_STATE : HUBUS_FRAME_EVENT;
    job.gap = lost > 0u || strcmp(result, "gap") == 0 || s->jobs_lost;
    job.len = f->len;
    memcpy(job.data, f->payload, f->len);
    if (hubusd_worker_post(w, &job)) {
        s->jobs_lost = 0;
    } else {
        s->jobs_lost = 1;
        d->stats.module_jobs_dropped++;
    }
}

/* The commands and states that module functions have produced since the last step. Returns 1 if there were any. What does not
 * fit into the command queue stays with the worker until the next step: a module gets told only when its own queue is full. */
static int drain_modules(hubusd_t *d)
{
    int busy = 0;
    size_t k;
    hubusd_out_t o;

    for (k = 0; k < HUBUS_MODULE_SET_MAX; k++) {
        hubusd_worker_t *w = d->worker[k];
        const hubus_module_t *m = w ? hubusd_worker_module(w) : NULL;

        while (w && queue_has_room(d) && hubusd_worker_take(w, &o)) {
            hubusd_slave_t *s = &d->slave[o.mcid];

            busy = 1;
            if (!s->configured || s->module != m) {
                continue; /* the slave went away or has another module: what the old one produced is stale */
            }
            switch (o.kind) {
            case HUBUSD_OUT_SEND: {
                hubusd_request_t r;

                memset(&r, 0, sizeof r);
                r.kind = HUBUSD_REQ_RAW;
                r.target_len = HUBUS_TARGET_LEN_EXACT;
                r.prefix = o.mcid;
                r.opcode = o.opcode;
                r.n_args = o.n;
                memcpy(r.args, o.args, o.n);
                enqueue(d, &r); /* like any command: in order, and after the blind time of the slave */
                break;
            }
            case HUBUSD_OUT_STATE:
                if (o.entity < m->n_entities) {
                    snprintf(s->last_state[o.entity], sizeof s->last_state[o.entity], "%s", o.text);
                    pub_topic(d, 1, o.text, "slave/%u/%s/state", o.mcid, m->entities[o.entity].key);
                }
                break;
            case HUBUSD_OUT_LOG:
                log_event(d, 1, "module %s, slave %u: %s", m->name, o.mcid, o.text);
                break;
            case HUBUSD_OUT_SET_REFUSED:
                reject(d, "module %s %s the value for entity '%s' of slave %u", m->name,
                       o.status == HUBUS_MODULE_REJECTED ? "rejected" : "could not use", m->entities[o.entity].key, o.mcid);
                break;
            case HUBUSD_OUT_NOT_RUNNING:
                reject(d, "module %s has no instance for slave %u yet; entity '%s' was not set", m->name, o.mcid,
                       m->entities[o.entity].key);
                break;
            case HUBUSD_OUT_CREATE_FAILED:
                log_event(d, 2, "module %s could not create an instance for slave %u", m->name, o.mcid);
                break;
            }
        }
    }
    return busy;
}

/* A call that has run longer than the limit stops its module. */
static void check_workers(hubusd_t *d, uint64_t now)
{
    size_t k;
    unsigned mcid;

    for (k = 0; k < HUBUS_MODULE_SET_MAX; k++) {
        if (d->worker[k] && hubusd_worker_check(d->worker[k], now, d->cfg.modules.call_timeout_ms)) {
            const hubus_module_t *m = hubusd_worker_module(d->worker[k]);

            log_event(d, 2, "module %s: a function did not return within %u ms; the module is stopped until hubusd restarts",
                      m->name, d->cfg.modules.call_timeout_ms);
            for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
                if (d->slave[mcid].configured && d->slave[mcid].module == m) {
                    publish_slave_info(d, mcid);
                }
            }
        }
    }
}

/* Destroys the instances and ends the threads. Can be called again. */
static void stop_modules(hubusd_t *d)
{
    size_t k;

    d->stopping = 1;
    sync_instances(d);
    for (k = 0; k < HUBUS_MODULE_SET_MAX; k++) {
        if (d->worker[k]) {
            const char *name = hubusd_worker_module(d->worker[k])->name;

            if (!hubusd_worker_stop(d->worker[k], MODULE_STOP_WAIT_MS)) {
                log_event(d, 2, "module %s did not stop within %u ms", name, MODULE_STOP_WAIT_MS);
            }
            d->worker[k] = NULL;
        }
    }
}

static void bind_config(hubusd_t *d)
{
    unsigned i;
    uint64_t now = now_ms(d);

    for (i = 0; i <= HUBUS_MCID_MAX; i++) {
        d->slave[i].configured = 0;
        d->slave[i].module = NULL;
    }
    for (i = 0; i < d->cfg.n_slaves; i++) {
        hubusd_slave_t *s = &d->slave[d->cfg.slaves[i].mcid];
        unsigned interval = d->cfg.bus.ping_interval_s ? d->cfg.bus.ping_interval_s : 1u;

        s->configured = 1;
        s->cfg_index = (uint8_t)i;
        s->next_ping_ms = now + (uint64_t)i * interval * 1000u / (d->cfg.n_slaves ? d->cfg.n_slaves : 1u);
        s->next_poll_ms = now + (uint64_t)d->cfg.slaves[i].poll_interval_s * 1000u;
        s->unknown_seen = 0;
        resolve_module(d, d->cfg.slaves[i].mcid);
    }
    sync_instances(d);
}

void hubusd_set_modules(hubusd_t *d, const hubus_module_t *const *modules, size_t n_modules)
{
    d->modules = modules;
    d->n_modules = n_modules;
}

void hubusd_init(hubusd_t *d, const hubusd_config_t *cfg, const hubus_master_hal_t *hal, hubusd_mqtt_t mqtt,
                 hubusd_clock_t clock)
{
    unsigned i;

    memset(d, 0, sizeof *d);
    d->cfg = *cfg;
    d->modules = hubusd_builtin_modules(&d->n_modules);
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
    stop_modules(d);
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

/* Publishes a frame of a configured slave (event data or a state read) and the values of its entities. */
static void deliver(hubusd_t *d, const hubus_slave_frame_t *f, const char *result, uint8_t lost)
{
    hubusd_slave_t *s = &d->slave[f->mcid];
    char buf[400], data[2 * HUBUS_MAX_PAYLOAD + 1], text[32], t[160];
    jw_t w;
    uint8_t i;

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

    for (i = 0; s->module && i < s->module->n_entities; i++) {
        const hubus_entity_t *e = &s->module->entities[i];

        if (hubus_entity_decode(e, f->payload, f->len, text, sizeof text)) {
            snprintf(s->last_state[i], sizeof s->last_state[i], "%s", text);
            pub_topic(d, 1, text, "slave/%u/%s/state", f->mcid, e->key);
        }
    }
    post_frame(d, s, f, result, lost);
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
        const hubus_module_t *before = s->module;

        resolve_module(d, mcid);
        if (before && before != s->module) {
            remove_slave_discovery(d, mcid, before); /* the entities of the module it had */
        }
        sync_instances(d);
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

        if (!s->configured || !s->needs_refresh || is_blind(d, mcid)) {
            continue; /* a blind slave keeps the flag for later */
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
                command_sent(d, 6, (uint8_t)mcid, HUBUS_OP_STATUS_CAPTURE);
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
    if (r == HUBUS_OK && info.len > HUBUS_INFO_HWID) {
        const hubus_module_t *m;

        (void)hubus_module_select_info(d->modules, d->n_modules, info.payload, info.len, &m);
        jw_uint(&w, "device_type", info.payload[HUBUS_INFO_HWID + HUBUS_HWID_TYPE]);
        jw_str(&w, "module", m ? m->name : "");
        jw_bool(&w, "state_capable", m && m->has_state);
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
        if (scan.state[mcid] == HUBUS_SCAN_PRESENT && scan.info[mcid].len > HUBUS_INFO_HWID) {
            jw_uint(&w, "device_type", scan.info[mcid].payload[HUBUS_INFO_HWID + HUBUS_HWID_TYPE]);
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
    command_sent(d, 6, (uint8_t)mcid, HUBUS_OP_STATUS_CAPTURE);
    run_burst(d);
}

/* Reads the state of `mcid` back after a command, in the way its readback setting asks for. */
static void read_back(hubusd_t *d, unsigned mcid, int entity, const char *expected)
{
    const hubusd_slave_t *s = &d->slave[mcid];
    hubus_status_t r;

    if (slave_readback(d, mcid) == HUBUSD_READBACK_CACHE) {
        /* no contact with the slave: report what the command establishes */
        if (expected && entity >= 0 && s->module && entity < s->module->n_entities) {
            hubusd_slave_t *w = &d->slave[mcid];

            snprintf(w->last_state[entity], sizeof w->last_state[entity], "%s", expected);
            pub_topic(d, 1, expected, "slave/%u/%s/state", mcid, s->module->entities[entity].key);
        }
    } else if (slave_readback(d, mcid) == HUBUSD_READBACK_SLAVE && info_has_state(s)) {
        /* commands are never acknowledged: read the state back, in one transaction */
        (void)read_state(d, mcid);
    } else if (slave_readback(d, mcid) == HUBUSD_READBACK_SLAVE && info_can_capture(s)) {
        /* a slave without a state captures it into the buffer, which the burst collects */
        r = hubus_master_status_capture(&d->master, 6, (uint8_t)mcid);
        if (r == HUBUS_OK) {
            command_sent(d, 6, (uint8_t)mcid, HUBUS_OP_STATUS_CAPTURE);
            run_burst(d);
        } else if (r == HUBUS_ERR_LINE_FAULT) {
            bus_fault(d, r);
        }
    }
}

/* `expected` is the state of entity `entity` that the command establishes (for readback: cache), NULL if it is not known. */
static void send_exact(hubusd_t *d, unsigned mcid, uint8_t opcode, const uint8_t *args, uint8_t n, int entity,
                       const char *expected)
{
    const hubusd_slave_t *s = &d->slave[mcid];
    hubus_status_t r;

    if (s->have_info && !s->module) {
        reject(d, "slave %u has no module: it gets no commands", mcid);
        return;
    }
    if (s->module && 1u + n > s->module->max_command) {
        reject(d, "command of %u bytes exceeds the %u bytes slave %u accepts", 1u + n, s->module->max_command, mcid);
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
    command_sent(d, 6, (uint8_t)mcid, opcode);
    if (slave_readback(d, mcid) == HUBUSD_READBACK_SLAVE && is_blind(d, mcid)) {
        d->slave[mcid].readback_due = 1; /* the slave cannot answer yet: read it back when its blind time ends */
    } else {
        read_back(d, mcid, entity, expected);
    }
}

/* Blind times that have run out (docs/spec/06-operation.md section 6.9). A RESET brings the slaves back, since a
 * slave that resynchronized ignores the bus until the next line condition; then the deferred readbacks follow. */
static void end_blind_times(hubusd_t *d)
{
    uint64_t now = now_ms(d);
    unsigned mcid;
    int ended = 0;
    hubus_status_t r;

    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        hubusd_slave_t *s = &d->slave[mcid];

        if (s->blind_until_ms && now >= s->blind_until_ms) {
            s->blind_until_ms = 0;
            ended = 1;
        }
    }
    if (!ended) {
        return;
    }
    r = hubus_master_reset(&d->master);
    if (r != HUBUS_OK) {
        bus_fault(d, r);
        return;
    }
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX && d->bus == HUBUSD_BUS_OK; mcid++) {
        hubusd_slave_t *s = &d->slave[mcid];

        if (s->readback_due && !is_blind(d, mcid)) {
            s->readback_due = 0;
            read_back(d, mcid, -1, NULL);
        }
    }
}

/* 1 if a queued request has to wait because it would reach a slave in its blind time. */
static int request_waits(const hubusd_t *d, const hubusd_request_t *q)
{
    switch (q->kind) {
    case HUBUSD_REQ_SET:
    case HUBUSD_REQ_CAPTURE:
    case HUBUSD_REQ_READ:
    case HUBUSD_REQ_PROBE:
        return is_blind(d, q->prefix);
    case HUBUSD_REQ_RAW:
        return target_blind(d, q->target_len, q->prefix);
    case HUBUSD_REQ_SCAN:
        return any_blind(d); /* a blind slave would look absent */
    default:
        return 0;
    }
}

/* ON or OFF, in any case. */
static int parse_on_off(const char *text, int *on)
{
    if (strcasecmp(text, "ON") == 0) {
        *on = 1;
        return 1;
    }
    if (strcasecmp(text, "OFF") == 0) {
        *on = 0;
        return 1;
    }
    return 0;
}

/* "r,g,b" with three numbers 0..255. */
static int parse_rgb(const char *text, hubus_value_t *v)
{
    unsigned r, g, b;
    char extra;

    if (sscanf(text, " %u , %u , %u %c", &r, &g, &b, &extra) != 3 || r > 255u || g > 255u || b > 255u) {
        return 0;
    }
    v->type = HUBUS_VALUE_RGB;
    v->as.rgb.r = (uint8_t)r;
    v->as.rgb.g = (uint8_t)g;
    v->as.rgb.b = (uint8_t)b;
    return 1;
}

/* Hands a value to the on_set function of the module of the slave. `text`: the content of a text value. */
static void dispatch_set(hubusd_t *d, unsigned mcid, int entity, const hubus_value_t *value, const char *text)
{
    const hubusd_slave_t *s = &d->slave[mcid];
    const char *key = s->module->entities[entity].key;
    hubusd_worker_t *w = worker_of(d, s->module, 0);
    hubusd_job_t job;

    if (!s->module->on_set) {
        reject(d, "entity '%s' of slave %u has no command and its module has no function for it", key, mcid);
    } else if (w && hubusd_worker_failed(w)) {
        reject(d, "module %s has failed and is not called any more", s->module->name);
    } else if (!w || s->inst_module != s->module) {
        reject(d, "module %s has no instance for slave %u", s->module->name, mcid);
    } else {
        memset(&job, 0, sizeof job);
        job.kind = HUBUSD_JOB_SET;
        job.mcid = (uint8_t)mcid;
        job.entity = (uint8_t)entity;
        job.value = *value;
        if (text) {
            snprintf(job.text, sizeof job.text, "%s", text);
        }
        if (!hubusd_worker_post(w, &job)) {
            d->stats.module_jobs_dropped++;
            reject(d, "module %s is busy: the value for entity '%s' of slave %u was dropped", s->module->name, key, mcid);
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
        const hubus_module_command_t *cmd = NULL;
        const hubus_entity_t *e;
        hubus_value_t v;
        int idx, on = 0;

        if (!s->configured) {
            reject(d, "slave %u is not configured", q->prefix);
        } else if (!s->module) {
            reject(d, "slave %u has no module; use the raw command topic", q->prefix);
        } else if ((idx = hubus_module_find_entity(s->module, q->key)) < 0) {
            reject(d, "slave %u has no entity '%s'", q->prefix, q->key);
        } else if (q->rgb) {
            e = &s->module->entities[idx];
            if (e->component != HUBUS_COMPONENT_LIGHT) {
                reject(d, "entity '%s' of slave %u is no light and takes no colour", q->key, q->prefix);
            } else if (!parse_rgb(q->text, &v)) {
                reject(d, "'%s' is not a colour for entity '%s' (r,g,b with numbers 0..255)", q->text, q->key);
            } else {
                dispatch_set(d, q->prefix, idx, &v, NULL);
            }
        } else if ((e = &s->module->entities[idx])->component == HUBUS_COMPONENT_TEXT) {
            v.type = HUBUS_VALUE_TEXT;
            dispatch_set(d, q->prefix, idx, &v, q->text);
        } else if (e->component != HUBUS_COMPONENT_SWITCH && e->component != HUBUS_COMPONENT_LIGHT) {
            reject(d, "entity '%s' of slave %u takes no commands", q->key, q->prefix);
        } else if (!parse_on_off(q->text, &on)) {
            reject(d, "'%s' is not a valid command for entity '%s' (ON or OFF)", q->text, q->key);
        } else {
            cmd = on ? &e->on : &e->off;
            if (e->component == HUBUS_COMPONENT_SWITCH && cmd->used) {
                send_exact(d, q->prefix, cmd->opcode, cmd->args, cmd->n_args, idx, on ? "ON" : "OFF");
            } else {
                v.type = HUBUS_VALUE_BOOL;
                v.as.on = on != 0;
                dispatch_set(d, q->prefix, idx, &v, NULL);
            }
        }
        break;
    }
    case HUBUSD_REQ_RAW:
        if (q->target_len == HUBUS_TARGET_LEN_EXACT) {
            if (q->opcode == HUBUS_OP_STATUS_CAPTURE) {
                capture_exact(d, q->prefix);
            } else {
                send_exact(d, q->prefix, q->opcode, q->args, q->n_args, -1, NULL);
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
                command_sent(d, q->target_len, q->prefix, q->opcode);
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
        const char *slash;

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
        slash = strchr(end, '/');
        if (slash && slash > end && (strcmp(slash + 1, "set") == 0 || strcmp(slash + 1, "rgb/set") == 0)) {
            size_t key_len = (size_t)(slash - end);

            r.kind = HUBUSD_REQ_SET;
            r.rgb = slash[1] == 'r';
            if (key_len >= sizeof r.key) {
                enqueue_reject(d, "entity key in '%s' is too long", topic_in);
                return;
            }
            memcpy(r.key, end, key_len);
            if (strlen(text) >= sizeof r.text) {
                enqueue_reject(d, "set payload for slave %u is too long", a);
                return;
            }
            memcpy(r.text, text, strlen(text) + 1u);
        } else if (strcmp(end, "set") == 0) {
            enqueue_reject(d, "a command for slave %u goes to an entity: slave/%u/<key>/set", a, a);
            return;
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
    static const char *const filters[] = {"master/cmd/+", "slave/+/+/set", "slave/+/+/rgb/set", "slave/+/cmd", "slave/+/capture", "slave/+/read",
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
    /* Remove discovery entries of slaves that are gone; the others are published again below. */
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        if (old[mcid].configured && old[mcid].module) {
            const hubus_module_t *m = old[mcid].module;
            int still = 0;
            unsigned i;

            for (i = 0; i < nc->n_slaves; i++) {
                still |= nc->slaves[i].mcid == mcid;
            }
            remove_slave_discovery(d, mcid, m);
            if (!still) {
                uint8_t k;

                pub_topic(d, 1, "", "slave/%u/availability", mcid);
                for (k = 0; k < m->n_entities; k++) {
                    pub_topic(d, 1, "", "slave/%u/%s/state", mcid, m->entities[k].key);
                }
                pub_topic(d, 1, "", "slave/%u/info", mcid);
            }
        }
    }
    d->cfg.n_slaves = nc->n_slaves;
    memcpy(d->cfg.slaves, nc->slaves, sizeof d->cfg.slaves);
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
            resolve_module(d, mcid);
            if (!old[mcid].configured) {
                d->slave[mcid].needs_refresh = 1; /* new slave: probe and read its state */
                d->slave[mcid].online = -1;
                d->slave[mcid].next_ping_ms = now_ms(d);
            }
        }
    }
    sync_instances(d);
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
    check_workers(d, now);
    if (d->sync_again) {
        sync_instances(d);
    }
    if (drain_modules(d)) {
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

    end_blind_times(d);
    if (d->bus != HUBUSD_BUS_OK) {
        return 1;
    }
    /* in order: a request for a slave in its blind time holds up the ones behind it */
    while (handled < 4 && peek(d, &q) && !request_waits(d, &q) && dequeue(d, &q)) {
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
        if (d->slave[mcid].configured && now >= d->slave[mcid].next_ping_ms && !is_blind(d, mcid)) {
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

        if (interval && now >= s->next_poll_ms && !is_blind(d, mcid)) {
            s->next_poll_ms = now + (uint64_t)interval * 1000u;
            if (s->online == 1) {
                s->needs_refresh = 1;
                refresh_states(d);
                busy = 1;
                break; /* one read per step, like the pings */
            }
        }
    }
    if (d->bus == HUBUSD_BUS_OK && d->cfg.bus.scan_interval_s && now >= d->next_scan_ms && !any_blind(d)) {
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
    stop_modules(d);
    if (d->bus == HUBUSD_BUS_OK) {
        (void)hubus_master_shutdown(&d->master);
    }
}
