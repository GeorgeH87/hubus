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
#include "hubus_update.h"
#include "hubus_json.h"

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
    return d->slave[mcid].configured ? d->cfg.rt.slaves[d->slave[mcid].cfg_index].name : "unconfigured";
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
    d->worker[slot] = hubusd_worker_start(m, d->cfg.mqtt.base_topic);
    return d->worker[slot];
}

int hubusd_modules_busy(hubusd_t *d)
{
    size_t k;

    for (k = 0; k < HUBUS_MODULE_SET_MAX; k++) {
        if (d->worker[k] && hubusd_worker_busy(d->worker[k])) {
            return 1;
        }
    }
    return 0;
}

static int module_failed(hubusd_t *d, const hubus_module_t *m)
{
    hubusd_worker_t *w = m ? worker_of(d, m, 0) : NULL;

    return w && hubusd_worker_failed(w);
}

static int slave_readback(const hubusd_t *d, unsigned mcid)
{
    const hubusd_slave_t *s = &d->slave[mcid];
    int rb = s->configured ? d->cfg.rt.slaves[s->cfg_index].readback : -1;

    if (rb < 0) {
        rb = d->cfg.rt.bus.readback;
    }
    /* A slave with neither a state nor STATUS_CAPTURE cannot be read back: its state exists only in the cache. */
    if (rb == HUBUSD_READBACK_SLAVE && !info_has_state(s) && !info_can_capture(s)) {
        rb = HUBUSD_READBACK_CACHE;
    }
    return rb;
}

/* The same for the module of the slave: whether it has to publish the state its command establishes. */
static hubus_module_readback_t module_readback(const hubusd_t *d, unsigned mcid)
{
    switch (slave_readback(d, mcid)) {
    case HUBUSD_READBACK_SLAVE: return HUBUS_READBACK_SLAVE;
    case HUBUSD_READBACK_CACHE: return HUBUS_READBACK_CACHE;
    default: return HUBUS_READBACK_NONE;
    }
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
    char buf[1024];
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
    /* the timing in use: the configuration, raised by the speed class of the slowest known slave */
    jw_obj_begin(&w, "timing");
    jw_uint(&w, "clk_low_us", d->master.cfg.t_clk_low_ns / 1000u);
    jw_uint(&w, "clk_high_us", d->master.cfg.t_clk_high_ns / 1000u);
    jw_uint(&w, "cond_us", d->master.cfg.t_cond_ns / 1000u);
    jw_uint(&w, "turnaround_us", d->master.cfg.t_turnaround_ns / 1000u);
    if (hubus_master_speed_limit(&d->master)) {
        jw_uint(&w, "limited_by", hubus_master_speed_limit(&d->master));
    }
    jw_obj_end(&w);
    /* a firmware update keeps the bus: what needs it is refused meanwhile */
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        const hubusd_slave_t *s = &d->slave[mcid];

        if (s->updating) {
            jw_obj_begin(&w, "update");
            jw_uint(&w, "mcid", mcid);
            jw_str(&w, "module", s->update_module ? s->update_module->name : "");
            jw_uint(&w, "percent", d->update_percent);
            jw_obj_end(&w);
        }
    }
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

/* Warns once when the speed class of a slave slows down the whole bus (docs/spec/01-physical-layer.md section 1.5). The
 * master raises the timing by itself whenever it reads a SLAVE_INFO; this only reports the change. */
static void check_timing(hubusd_t *d)
{
    const hubus_master_cfg_t *t = &d->master.cfg;
    uint8_t mcid = hubus_master_speed_limit(&d->master);
    uint8_t cls = mcid ? d->master.speed_class[mcid] : 0u;

    if (mcid == d->speed_limit && cls == d->speed_limit_class) {
        return;
    }
    d->speed_limit = mcid;
    d->speed_limit_class = cls;
    if (mcid) {
        log_event(d, 1,
                  "slave %u (speed class %u, %lu us) slows down the bus: clock low %lu us, high %lu us, conditions %lu us, "
                  "turnaround %lu us",
                  mcid, cls, (unsigned long)(hubus_speed_class_ns(cls) / 1000u), (unsigned long)(t->t_clk_low_ns / 1000u),
                  (unsigned long)(t->t_clk_high_ns / 1000u), (unsigned long)(t->t_cond_ns / 1000u),
                  (unsigned long)(t->t_turnaround_ns / 1000u));
    } else {
        log_event(d, 0, "bus timing back to the configured minimums");
    }
    publish_state(d, 0);
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

/* The module whose image this slave could get: its own, or for a slave in its updater the one its last update came from. */
static const hubus_module_t *firmware_module(const hubusd_slave_t *s)
{
    return (s->module && s->module->firmware) ? s->module : s->update_module;
}

/* What is installed and what the module brings, in the form of the update entity of Home Assistant. `percent` < 0: no progress. */
static void publish_firmware(hubusd_t *d, unsigned mcid, int in_progress, int percent)
{
    const hubusd_slave_t *s = &d->slave[mcid];
    const hubus_module_t *m = firmware_module(s);
    char buf[200], t[160], version[12];
    int installed = info_field(s, HUBUS_HWID_VERSION);
    jw_t w;

    if (!m) {
        return;
    }
    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    if (installed >= 0) {
        snprintf(version, sizeof version, "%d", installed);
        jw_str(&w, "installed_version", version);
    } else {
        jw_null(&w, "installed_version");
    }
    snprintf(version, sizeof version, "%u", (unsigned)m->firmware_version);
    jw_str(&w, "latest_version", version);
    jw_bool(&w, "in_progress", in_progress);
    if (percent >= 0) {
        jw_uint(&w, "update_percentage", (uint64_t)percent);
    } else {
        jw_null(&w, "update_percentage");
    }
    jw_obj_end(&w);
    topic(d, t, sizeof t, "slave/%u/firmware", mcid);
    pub_json(d, &w, 1, t);
}

static void publish_slave_info(hubusd_t *d, unsigned mcid)
{
    char buf[1800];
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
    jw_arr_begin(&w, "topics"); /* what its module takes */
    for (i = 0; i < s->n_topics; i++) {
        jw_str(&w, NULL, s->topics[i]);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    topic(d, t, sizeof t, "slave/%u/info", mcid);
    pub_json(d, &w, 1, t);
    publish_firmware(d, mcid, 0, -1);
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

/* What every discovery message of a slave ends with, in an open object: when it is available, and the device it belongs to. */
static void jw_slave_device(hubusd_t *d, jw_t *w, unsigned mcid)
{
    char t[160], master[96], serial[2 * HUBUS_MAX_PAYLOAD + 1], fw[16], dev[48];
    const hubusd_slave_t *s = &d->slave[mcid];

    snprintf(dev, sizeof dev, "hubus_%u", mcid);
    master_identifier(d, master, sizeof master);

    jw_arr_begin(w, "availability");
    jw_obj_begin(w, NULL);
    topic(d, t, sizeof t, "master/status");
    jw_str(w, "topic", t);
    jw_obj_end(w);
    jw_obj_begin(w, NULL);
    topic(d, t, sizeof t, "slave/%u/availability", mcid);
    jw_str(w, "topic", t);
    jw_obj_end(w);
    jw_arr_end(w);
    jw_str(w, "availability_mode", "all");
    jw_obj_begin(w, "device");
    jw_arr_begin(w, "identifiers");
    jw_str(w, NULL, dev);
    jw_arr_end(w);
    jw_str(w, "name", slave_name(d, mcid));
    jw_str(w, "manufacturer", "HUBUS");
    jw_str(w, "model", s->module ? s->module->name : "");
    if (info_field(s, HUBUS_HWID_VERSION) >= 0) {
        snprintf(fw, sizeof fw, "%d", info_field(s, HUBUS_HWID_VERSION));
        jw_str(w, "sw_version", fw);
    }
    if (info_serial(s, serial)) {
        jw_str(w, "serial_number", serial);
    }
    jw_str(w, "via_device", master);
    jw_obj_end(w);
}

/* The update entity of Home Assistant: its install command sends the name of the module to the trigger. */
static void publish_firmware_discovery(hubusd_t *d, unsigned mcid)
{
    char buf[1400];
    char t[160], tt[160], object[48];
    jw_t w;

    snprintf(object, sizeof object, "hubus_%u_firmware", mcid);
    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_str(&w, "name", "Firmware");
    jw_bool(&w, "has_entity_name", 1);
    jw_str(&w, "unique_id", object);
    jw_str(&w, "device_class", "firmware");
    topic(d, t, sizeof t, "slave/%u/firmware", mcid);
    jw_str(&w, "state_topic", t);
    topic(d, t, sizeof t, "slave/%u/update", mcid);
    jw_str(&w, "command_topic", t);
    jw_str(&w, "payload_install", d->slave[mcid].module->name);
    jw_slave_device(d, &w, mcid);
    jw_obj_end(&w);
    discovery_topic(d, tt, sizeof tt, "update", object);
    pub_json(d, &w, 1, tt);
}

/* The discovery that hubusd itself owns for a slave: the update entity of a module with a firmware image. The module of the
 * slave publishes the rest. */
static void publish_slave_discovery(hubusd_t *d, unsigned mcid)
{
    const hubusd_slave_t *s = &d->slave[mcid];

    if (d->cfg.rt.mqtt.discovery && s->configured && s->module && s->module->firmware) {
        publish_firmware_discovery(d, mcid);
    }
}

static void remove_firmware_discovery(hubusd_t *d, unsigned mcid)
{
    char tt[160], object[48];

    snprintf(object, sizeof object, "hubus_%u_firmware", mcid);
    discovery_topic(d, tt, sizeof tt, "update", object);
    pub(d, tt, "", 1);
}

/* The discovery entries hubusd owns for a slave with module `m`. Without a module (an MCID nobody configured) also the
 * single entry per slave that early versions published; what modules published is found by remove_stale_discovery(). */
static void remove_slave_discovery(hubusd_t *d, unsigned mcid, const hubus_module_t *m)
{
    static const char *const legacy[] = {"switch", "binary_sensor", "sensor"};
    char tt[160], object[48];
    uint8_t i;

    if (!d->cfg.rt.mqtt.discovery) {
        return;
    }
    if (m) {
        if (m->firmware) {
            remove_firmware_discovery(d, mcid);
        }
        return;
    }
    remove_firmware_discovery(d, mcid);
    snprintf(object, sizeof object, "hubus_%u", mcid);
    for (i = 0; i < sizeof legacy / sizeof legacy[0]; i++) {
        discovery_topic(d, tt, sizeof tt, legacy[i], object);
        pub(d, tt, "", 1);
    }
}

/* The slave of a discovery topic "<prefix>/<component>/<object>/config" whose object is hubus_<n> or hubus_<n>_<something>;
 * 0 for any other topic. `legacy`: the object is hubus_<n>, the single entity of early versions, which no module has. */
static unsigned discovery_mcid(const hubusd_t *d, const char *full, int *legacy)
{
    size_t pl = strlen(d->cfg.mqtt.discovery_prefix);
    const char *object, *config;
    unsigned long mcid;
    char *end;

    if (strncmp(full, d->cfg.mqtt.discovery_prefix, pl) != 0 || full[pl] != '/' ||
        (object = strchr(full + pl + 1u, '/')) == NULL || strncmp(object + 1, "hubus_", 6) != 0 ||
        !isdigit((unsigned char)object[7])) {
        return 0;
    }
    mcid = strtoul(object + 7, &end, 10);
    config = strchr(end, '/');
    if ((*end != '_' && *end != '/') || !config || strcmp(config, "/config") != 0 || mcid < HUBUS_MCID_MIN ||
        mcid > HUBUS_MCID_MAX) {
        return 0;
    }
    *legacy = *end == '/';
    return (unsigned)mcid;
}

/* A retained discovery message on the broker (mqtt.discovery_cleanup): one of an MCID that is not configured is removed, and
 * one of early versions. */
static void remove_stale_discovery(hubusd_t *d, const char *full)
{
    int legacy = 0;
    unsigned mcid = discovery_mcid(d, full, &legacy);

    if (d->cfg.rt.mqtt.discovery && mcid && (legacy || !d->slave[mcid].configured)) {
        pub(d, full, "", 1);
    }
}

/* ---- what the module of a slave publishes, and the topics it takes -------------------------------------------------------- */

static hubusd_retained_t *retained_find(const hubusd_slave_t *s, const char *full)
{
    hubusd_retained_t *r;

    for (r = s->retained; r; r = r->next) {
        if (strcmp(r->topic, full) == 0) {
            return r;
        }
    }
    return NULL;
}

static void retained_free(hubusd_retained_t *r)
{
    free(r->payload);
    free(r);
}

/* Keeps a retained message to publish it again; an empty one forgets the topic. A payload that grows needs memory once, so a
 * state that changes with every frame costs no allocation. 0 if it cannot be kept: too many topics, or no memory. */
static int retained_put(hubusd_slave_t *s, const char *full, const char *payload, size_t len, int discovery)
{
    hubusd_retained_t *r = retained_find(s, full), **p;

    if (len == 0u) {
        for (p = &s->retained; r && *p; p = &(*p)->next) {
            if (*p == r) {
                *p = r->next;
                retained_free(r);
                s->n_retained--;
                break;
            }
        }
        return 1;
    }
    if (!r) {
        if (s->n_retained >= HUBUSD_RETAINED_MAX || !(r = calloc(1, sizeof *r))) {
            return 0;
        }
        snprintf(r->topic, sizeof r->topic, "%s", full);
        r->next = s->retained;
        s->retained = r;
        s->n_retained++;
    }
    if (len + 1u > r->cap) {
        char *grown = realloc(r->payload, len + 1u);

        if (!grown) {
            return 0;
        }
        r->payload = grown;
        r->cap = len + 1u;
    }
    memcpy(r->payload, payload, len);
    r->payload[len] = '\0';
    r->len = len;
    r->discovery = (uint8_t)discovery;
    return 1;
}

/* A discovery message of the module of a slave on `tt` ("<prefix>/<component>/hubus_<n>_<object>/config"): hubusd adds the
 * unique id, availability, and the device of the slave, as they are now (name, firmware version). */
static void pub_discovery(hubusd_t *d, unsigned mcid, const char *tt, const char *json, size_t len)
{
    char id[HUBUSD_FULL_TOPIC_LEN], tail[1024], buf[HUBUS_MODULE_PAYLOAD_MAX + sizeof tail];
    const char *end = strrchr(tt, '/'), *start = end;
    jw_t w;

    while (start > tt && start[-1] != '/') {
        start--;
    }
    snprintf(id, sizeof id, "%.*s", (int)(end - start), start);
    jw_init(&w, tail, sizeof tail);
    jw_obj_begin(&w, NULL);
    jw_str(&w, "unique_id", id);
    jw_slave_device(d, &w, mcid);
    jw_obj_end(&w);
    if (!jw_ok(&w)) {
        d->stats.publish_errors++;
        return;
    }
    /* `json` is "{...}": its members, then those of the tail */
    snprintf(buf, sizeof buf, "%.*s%s%s", (int)(len - 1u), json, len > 2u ? "," : "", tail + 1);
    pub(d, tt, buf, 1);
}

/* Everything the module of a slave published retained, again; discovery only while it is on. */
static void publish_module_retained(hubusd_t *d, unsigned mcid)
{
    const hubusd_retained_t *r;

    for (r = d->slave[mcid].retained; r; r = r->next) {
        if (!r->discovery) {
            pub(d, r->topic, r->payload, 1);
        } else if (d->cfg.rt.mqtt.discovery) {
            pub_discovery(d, mcid, r->topic, r->payload, r->len);
        }
    }
}

/* Takes back what the module of a slave published retained: its slave left the configuration, or it has another module. */
static void retained_clear(hubusd_t *d, unsigned mcid)
{
    hubusd_slave_t *s = &d->slave[mcid];

    while (s->retained) {
        hubusd_retained_t *r = s->retained;

        s->retained = r->next;
        pub(d, r->topic, "", 1);
        retained_free(r);
    }
    s->n_retained = 0;
}

/* Removes the discovery messages of the module of a slave from the broker, but keeps them: discovery was switched off. */
static void retained_hide_discovery(hubusd_t *d, unsigned mcid)
{
    const hubusd_retained_t *r;

    for (r = d->slave[mcid].retained; r; r = r->next) {
        if (r->discovery) {
            pub(d, r->topic, "", 1);
        }
    }
}

static void module_publish(hubusd_t *d, unsigned mcid, const char *sub, const char *payload, size_t len, int retain)
{
    hubusd_slave_t *s = &d->slave[mcid];
    char t[HUBUSD_FULL_TOPIC_LEN];

    topic(d, t, sizeof t, "slave/%u/%s", mcid, sub);
    if (retain && !retained_put(s, t, payload, len, 0)) {
        log_event(d, 1, "module %s, slave %u: more than %u retained topics, '%s' is not kept", s->module->name, mcid,
                  HUBUSD_RETAINED_MAX, sub);
    }
    if (d->mqtt.publish(d->mqtt.ctx, t, payload, len, retain) != 0) {
        d->stats.publish_errors++;
    }
}

/* A discovery message of the module of a slave. The cache keeps what the module wrote: the device part can change. */
static void module_discovery(hubusd_t *d, unsigned mcid, const char *component, const char *object, const char *json,
                             size_t len)
{
    hubusd_slave_t *s = &d->slave[mcid];
    char tt[HUBUSD_FULL_TOPIC_LEN], id[96];

    snprintf(id, sizeof id, "hubus_%u_%s", mcid, object);
    discovery_topic(d, tt, sizeof tt, component, id);
    if (!retained_put(s, tt, json, len, 1)) {
        log_event(d, 1, "module %s, slave %u: more than %u retained topics, the discovery of '%s' is not kept",
                  s->module->name, mcid, HUBUSD_RETAINED_MAX, object);
    }
    if (d->cfg.rt.mqtt.discovery) {
        pub_discovery(d, mcid, tt, json, len);
    }
}

static int slave_topic_index(const hubusd_slave_t *s, const char *sub)
{
    uint8_t i;

    for (i = 0; i < s->n_topics; i++) {
        if (strcmp(s->topics[i], sub) == 0) {
            return i;
        }
    }
    return -1;
}

static void mqtt_subscribe(hubusd_t *d, unsigned mcid, const char *sub, int on)
{
    char t[HUBUSD_FULL_TOPIC_LEN];
    int (*fn)(void *, const char *) = on ? d->mqtt.subscribe : d->mqtt.unsubscribe;

    topic(d, t, sizeof t, "slave/%u/%s", mcid, sub);
    if (fn && fn(d->mqtt.ctx, t) != 0) {
        d->stats.publish_errors++;
    }
}

/* The instance of a slave takes a topic. The worker has checked it and keeps the same list. */
static void slave_subscribe(hubusd_t *d, unsigned mcid, const char *sub)
{
    hubusd_slave_t *s = &d->slave[mcid];

    if (slave_topic_index(s, sub) >= 0 || s->n_topics >= HUBUS_MODULE_TOPICS_MAX) {
        return;
    }
    snprintf(s->topics[s->n_topics++], sizeof s->topics[0], "%s", sub);
    mqtt_subscribe(d, mcid, sub, 1);
}

static void slave_unsubscribe(hubusd_t *d, unsigned mcid, const char *sub)
{
    hubusd_slave_t *s = &d->slave[mcid];
    int i = slave_topic_index(s, sub);

    if (i < 0) {
        return;
    }
    mqtt_subscribe(d, mcid, sub, 0);
    s->n_topics--;
    memmove(s->topics[i], s->topics[i + 1], (size_t)(s->n_topics - i) * sizeof s->topics[0]);
}

/* The instance of a slave goes away, and its topics with it. */
static void slave_drop_topics(hubusd_t *d, unsigned mcid)
{
    hubusd_slave_t *s = &d->slave[mcid];

    while (s->n_topics) {
        slave_unsubscribe(d, mcid, s->topics[s->n_topics - 1u]);
    }
}

/* After a new connection: the topics of all instances; the broker forgot them. */
static void resubscribe_all(hubusd_t *d)
{
    unsigned mcid;
    uint8_t i;

    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        for (i = 0; i < d->slave[mcid].n_topics; i++) {
            mqtt_subscribe(d, mcid, d->slave[mcid].topics[i], 1);
        }
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

    if (!d->cfg.rt.mqtt.discovery) {
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

static void remove_master_discovery(hubusd_t *d)
{
    char tt[200], master[96], object[160];
    unsigned i;

    master_identifier(d, master, sizeof master);
    for (i = 0; i < sizeof master_sensors / sizeof master_sensors[0]; i++) {
        snprintf(object, sizeof object, "%s_%s", master, master_sensors[i].key);
        discovery_topic(d, tt, sizeof tt, "sensor", object);
        pub(d, tt, "", 1);
    }
}

/* ---- the runtime part of the configuration over MQTT ---------------------------------------------------------- */

#define CONFIG_JSON_MAX (4096u + HUBUSD_MAX_SLAVES * (400u + HUBUS_MODULE_SETTINGS_MAX * (2u * HUBUS_MODULE_SETTING_MAX + 40u)))

static const char *readback_name(int readback)
{
    return readback == HUBUSD_READBACK_NONE ? "none" : readback == HUBUSD_READBACK_CACHE ? "cache" : "slave";
}

/* hubus/master/config: the runtime part, in the form master/config/set takes. */
static void publish_config(hubusd_t *d)
{
    char *buf = malloc(CONFIG_JSON_MAX);
    char t[160];
    unsigned i, k;
    jw_t w;

    if (!buf) {
        log_event(d, 2, "out of memory: hubus/master/config not published");
        return;
    }
    jw_init(&w, buf, CONFIG_JSON_MAX);
    jw_obj_begin(&w, NULL);
    jw_obj_begin(&w, "bus");
    jw_str(&w, "readback", readback_name(d->cfg.rt.bus.readback));
    jw_uint(&w, "ping_interval_s", d->cfg.rt.bus.ping_interval_s);
    jw_uint(&w, "ping_failures", d->cfg.rt.bus.ping_failures);
    jw_uint(&w, "scan_interval_s", d->cfg.rt.bus.scan_interval_s);
    jw_uint(&w, "startup_retry_s", d->cfg.rt.bus.startup_retry_s);
    jw_obj_end(&w);
    jw_obj_begin(&w, "mqtt");
    jw_uint(&w, "stats_interval_s", d->cfg.rt.mqtt.stats_interval_s);
    jw_bool(&w, "discovery", d->cfg.rt.mqtt.discovery);
    jw_bool(&w, "discovery_cleanup", d->cfg.rt.mqtt.discovery_cleanup);
    jw_obj_end(&w);
    jw_arr_begin(&w, "slaves");
    for (i = 0; i < d->cfg.rt.n_slaves; i++) {
        const hubusd_slave_cfg_t *sl = &d->cfg.rt.slaves[i];

        jw_obj_begin(&w, NULL);
        jw_uint(&w, "mcid", sl->mcid);
        jw_str(&w, "name", sl->name);
        jw_str(&w, "readback", sl->readback < 0 ? "default" : readback_name(sl->readback));
        jw_uint(&w, "poll_interval_s", sl->poll_interval_s);
        if (sl->settings.n) {
            jw_obj_begin(&w, "settings"); /* read only: they belong to the module */
            jw_str(&w, "module", sl->settings.module);
            for (k = 0; k < sl->settings.n; k++) {
                jw_str(&w, sl->settings.item[k].key, sl->settings.item[k].value);
            }
            jw_obj_end(&w);
        }
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    topic(d, t, sizeof t, "master/config");
    pub_json(d, &w, 1, t);
    free(buf);
}

/* Writes the runtime file. 1 if it is written, 0 if hubusd keeps no runtime file or it cannot be written (logged). */
static int persist_config(hubusd_t *d)
{
    char err[300];

    if (!d->cfg.runtime_file[0]) {
        return 0;
    }
    if (hubusd_config_write_runtime(&d->cfg.rt, d->cfg.runtime_file, err, sizeof err) != 0) {
        log_event(d, 2, "the runtime configuration is not saved: %s", err);
        return 0;
    }
    return 1;
}

/* A module saved a setting of its slave. A setting of another module goes: the slave has a new module. */
static void save_setting(hubusd_t *d, unsigned mcid, const hubus_module_t *m, const char *key, const char *value)
{
    hubusd_slave_cfg_t *sl = hubusd_config_slave(&d->cfg.rt, mcid);
    hubusd_settings_t *st;
    unsigned k;

    if (!sl) {
        return;
    }
    st = &sl->settings;
    if (strcmp(st->module, m->name) != 0) {
        memset(st, 0, sizeof *st);
        snprintf(st->module, sizeof st->module, "%s", m->name);
    }
    for (k = 0; k < st->n && strcmp(st->item[k].key, key) != 0; k++) {
    }
    if (!value) {
        if (k < st->n) {
            st->n--;
            memmove(&st->item[k], &st->item[k + 1u], (st->n - k) * sizeof st->item[0]);
        }
    } else if (k < st->n || st->n < HUBUS_MODULE_SETTINGS_MAX) {
        if (k == st->n) {
            snprintf(st->item[st->n++].key, sizeof st->item[0].key, "%s", key);
        }
        snprintf(st->item[k].value, sizeof st->item[0].value, "%s", value);
    }
    d->config_dirty = 1;
}

/* What modules saved goes to the runtime file and to hubus/master/config. */
static void config_changed(hubusd_t *d)
{
    d->config_dirty = 0;
    (void)persist_config(d);
    publish_config(d);
}

static void publish_all_retained(hubusd_t *d)
{
    unsigned mcid;

    publish_master_status(d, 1);
    publish_config(d);
    publish_master_discovery(d);
    publish_state(d, 1);
    publish_diagnosis(d, 1);
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        if (!d->slave[mcid].configured) {
            continue;
        }
        publish_availability(d, mcid);
        publish_slave_info(d, mcid);
        publish_slave_discovery(d, mcid);
        publish_module_retained(d, mcid);
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
    job.readback = module_readback(d, mcid);
    if (kind == HUBUSD_JOB_CREATE && d->slave[mcid].configured) {
        const hubusd_settings_t *st = &d->cfg.rt.slaves[d->slave[mcid].cfg_index].settings;

        if (strcmp(st->module, hubusd_worker_module(w)->name) == 0) {
            job.settings = *st; /* only for the module that saved them */
        }
    }
    return hubusd_worker_post(w, &job) || hubusd_worker_failed(w) || d->stopping;
}

/* Keeps one instance per slave in operation: creates the ones that are missing, destroys the ones whose slave or module is
 * gone, and with them their topics. Only queues jobs; the worker does the work. A create or destroy never gets lost: what does
 * not fit into a queue now is done in a later step. */
static void sync_instances(hubusd_t *d)
{
    unsigned mcid;

    d->sync_again = 0;
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        hubusd_slave_t *s = &d->slave[mcid];
        const hubus_module_t *want = (!d->stopping && !s->updating && s->configured && s->module) ? s->module : NULL;
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
        slave_drop_topics(d, mcid);
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
            s->no_instance = 0;
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

    if (!s->inst_module || strcmp(result, "duplicate") == 0 || !(w = worker_of(d, s->inst_module, 0)) ||
        hubusd_worker_failed(w)) {
        return;
    }
    memset(&job, 0, sizeof job);
    job.kind = HUBUSD_JOB_FRAME;
    job.mcid = f->mcid;
    job.readback = module_readback(d, f->mcid);
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

/* What module functions have produced since the last step: commands, messages, topics. Returns 1 if there was any. What does
 * not fit into the command queue stays with the worker until the next step: a module gets told only when its own queue is
 * full. */
static int drain_modules(hubusd_t *d)
{
    int busy = 0;
    size_t k;
    static hubusd_out_t o; /* large; the bus thread is the only one here */

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
            case HUBUSD_OUT_PUBLISH:
                module_publish(d, o.mcid, o.topic, o.payload, o.len, o.retain);
                break;
            case HUBUSD_OUT_SUBSCRIBE:
                if (s->inst_module == m) { /* not from an instance that is on its way out */
                    slave_subscribe(d, o.mcid, o.topic);
                    publish_slave_info(d, o.mcid);
                }
                break;
            case HUBUSD_OUT_UNSUBSCRIBE:
                slave_unsubscribe(d, o.mcid, o.topic);
                publish_slave_info(d, o.mcid);
                break;
            case HUBUSD_OUT_DISCOVERY:
                module_discovery(d, o.mcid, o.component, o.topic, o.payload, o.len);
                break;
            case HUBUSD_OUT_LOG:
                log_event(d, 1, "module %s, slave %u: %s", m->name, o.mcid, o.payload);
                break;
            case HUBUSD_OUT_REJECT:
                reject(d, "slave %u: %s", o.mcid, o.payload);
                break;
            case HUBUSD_OUT_NOT_RUNNING:
                reject(d, "module %s has no instance for slave %u yet; '%s' was not taken", m->name, o.mcid, o.topic);
                break;
            case HUBUSD_OUT_CREATE_FAILED:
                slave_drop_topics(d, o.mcid);
                s->no_instance = 1;
                retained_clear(d, o.mcid); /* what it published before it gave up has nothing behind it */
                log_event(d, 2, "module %s could not create an instance for slave %u", m->name, o.mcid);
                break;
            case HUBUSD_OUT_SAVE:
                if (s->inst_module == m) {
                    save_setting(d, o.mcid, m, o.topic, o.n ? NULL : o.payload);
                }
                break;
            }
        }
    }
    if (d->config_dirty) {
        config_changed(d);
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

static void stop_worker(hubusd_t *d, size_t k)
{
    const char *name = hubusd_worker_module(d->worker[k])->name;

    if (!hubusd_worker_stop(d->worker[k], MODULE_STOP_WAIT_MS)) {
        log_event(d, 2, "module %s did not stop within %u ms", name, MODULE_STOP_WAIT_MS);
    }
    d->worker[k] = NULL;
}

/* Destroys the instances and ends the threads. Can be called again. */
static void stop_modules(hubusd_t *d)
{
    size_t k;

    d->stopping = 1;
    sync_instances(d);
    for (k = 0; k < HUBUS_MODULE_SET_MAX; k++) {
        if (d->worker[k]) {
            stop_worker(d, k);
        }
    }
}

static int module_listed(const hubus_module_t *m, const hubus_module_t *const *modules, size_t n_modules)
{
    size_t i;

    for (i = 0; i < n_modules; i++) {
        if (modules[i] == m) {
            return 1;
        }
    }
    return 0;
}

const char *hubusd_modules_missing(const hubusd_t *d, const hubusd_config_t *cfg, const hubus_module_t *const *modules,
                                   size_t n_modules)
{
    unsigned i;

    for (i = 0; i < cfg->rt.n_slaves; i++) {
        const hubusd_slave_t *s = &d->slave[cfg->rt.slaves[i].mcid];
        const hubus_module_t *found;

        if (s->have_info && s->module &&
            hubus_module_select_info(modules, n_modules, s->info.payload, s->info.len, &found) != HUBUS_MODULE_FOUND) {
            return s->module->name;
        }
    }
    return NULL;
}

void hubusd_replace_modules(hubusd_t *d, const hubus_module_t *const *modules, size_t n_modules)
{
    unsigned mcid;
    size_t k;

    d->modules = modules;
    d->n_modules = n_modules;
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        if (d->slave[mcid].update_module && !module_listed(d->slave[mcid].update_module, modules, n_modules)) {
            d->slave[mcid].update_module = NULL;
        }
    }
    for (k = 0; k < HUBUS_MODULE_SET_MAX; k++) {
        if (d->worker[k] && !module_listed(hubusd_worker_module(d->worker[k]), modules, n_modules)) {
            const hubus_module_t *gone = hubusd_worker_module(d->worker[k]);

            stop_worker(d, k); /* waits for what runs, and destroys its instances */
            for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
                if (d->slave[mcid].inst_module == gone) {
                    d->slave[mcid].inst_module = NULL;
                    slave_drop_topics(d, mcid);
                }
            }
        }
    }
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        hubusd_slave_t *s = &d->slave[mcid];
        const hubus_module_t *before = s->module;

        if (!s->configured) {
            continue;
        }
        resolve_module(d, mcid);
        if (s->module != before) {
            if (before) { /* what the old module published goes; the new one publishes its own */
                remove_slave_discovery(d, mcid, before);
                if (before->firmware && !(s->module && s->module->firmware)) {
                    pub_topic(d, 1, "", "slave/%u/firmware", mcid);
                }
            }
            retained_clear(d, mcid);
            publish_slave_info(d, mcid);
            publish_slave_discovery(d, mcid);
        }
    }
    sync_instances(d);
}

void hubusd_unload_modules(hubus_module_set_t *set)
{
    size_t i;

    for (i = 0; i < set->n; i++) {
        if (hubusd_worker_abandoned(set->module[i])) {
            hubus_module_set_keep(set, i);
        }
    }
    hubus_module_set_unload(set);
}

int hubusd_reload_modules(hubusd_t *d, hubus_module_set_t *current, const hubusd_config_t *cfg, hubus_load_report_fn report,
                          void *report_ctx)
{
    hubus_module_set_t fresh, old;
    const char *missing;
    size_t plugins;

    hubus_module_set_init(&fresh);
    fresh.trust_all = current->trust_all;
    plugins = hubusd_load_modules(cfg, &fresh, report, report_ctx);
    missing = hubusd_modules_missing(d, cfg, fresh.module, fresh.n);
    if (missing) {
        log_event(d, 2, "modules not reloaded: a slave uses module %s, and no module would replace it", missing);
        hubus_module_set_unload(&fresh);
        return 0;
    }
    old = *current;
    *current = fresh; /* the daemon points into this structure */
    hubusd_replace_modules(d, current->module, current->n);
    hubusd_unload_modules(&old);
    log_event(d, 0, "modules reloaded: %u, %u of them plugins", (unsigned)current->n, (unsigned)plugins);
    return 1;
}

static void bind_config(hubusd_t *d)
{
    unsigned i;
    uint64_t now = now_ms(d), configured = 0;

    for (i = 0; i <= HUBUS_MCID_MAX; i++) {
        d->slave[i].configured = 0;
        d->slave[i].module = NULL;
    }
    for (i = 0; i < d->cfg.rt.n_slaves; i++) {
        hubusd_slave_t *s = &d->slave[d->cfg.rt.slaves[i].mcid];
        unsigned interval = d->cfg.rt.bus.ping_interval_s ? d->cfg.rt.bus.ping_interval_s : 1u;

        configured |= (uint64_t)1u << d->cfg.rt.slaves[i].mcid;
        s->configured = 1;
        s->cfg_index = (uint8_t)i;
        s->next_ping_ms = now + (uint64_t)i * interval * 1000u / (d->cfg.rt.n_slaves ? d->cfg.rt.n_slaves : 1u);
        s->next_poll_ms = now + (uint64_t)d->cfg.rt.slaves[i].poll_interval_s * 1000u;
        s->unknown_seen = 0;
        resolve_module(d, d->cfg.rt.slaves[i].mcid);
    }
    pthread_mutex_lock(&d->lock);
    d->configured = configured;
    pthread_mutex_unlock(&d->lock);
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
    d->discovery_cleanup = d->cfg.rt.mqtt.discovery_cleanup;
    d->config_tail = &d->config_head;
    d->next_stats_ms = d->start_ms + (uint64_t)d->cfg.rt.mqtt.stats_interval_s * 1000u;
    for (i = 0; i <= HUBUS_MCID_MAX; i++) {
        d->slave[i].online = -1;
    }
    bind_config(d);
}

void hubusd_destroy(hubusd_t *d)
{
    unsigned mcid;

    stop_modules(d);
    for (mcid = 0; mcid <= HUBUS_MCID_MAX; mcid++) {
        while (d->slave[mcid].retained) {
            hubusd_retained_t *r = d->slave[mcid].retained;

            d->slave[mcid].retained = r->next;
            retained_free(r);
        }
    }
    free(d->reload_cfg);
    d->reload_cfg = NULL;
    while (d->config_head) {
        hubusd_config_req_t *q = d->config_head;

        d->config_head = q->next;
        free(q);
    }
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

/* Publishes a frame of a configured slave (event data or a state read) and passes it to its module. */
static void deliver(hubusd_t *d, const hubus_slave_frame_t *f, const char *result, uint8_t lost)
{
    hubusd_slave_t *s = &d->slave[f->mcid];
    char buf[400], data[2 * HUBUS_MAX_PAYLOAD + 1], t[160];
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
    d->next_startup_ms = now_ms(d) + (uint64_t)d->cfg.rt.bus.startup_retry_s * 1000u;
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
            remove_slave_discovery(d, mcid, before);
            if (before->firmware && !firmware_module(s)) {
                pub_topic(d, 1, "", "slave/%u/firmware", mcid);
            }
            retained_clear(d, mcid); /* what the module it had published */
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

    s->next_ping_ms = now + (uint64_t)(d->cfg.rt.bus.ping_interval_s ? d->cfg.rt.bus.ping_interval_s : 30u) * 1000u;
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
    for (i = 0; i < d->cfg.rt.n_slaves && d->bus == HUBUSD_BUS_OK; i++) {
        ping(d, d->cfg.rt.slaves[i].mcid, 1);
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
    d->next_scan_ms = now_ms(d) + (uint64_t)d->cfg.rt.bus.scan_interval_s * 1000u;
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

/* Reads the state of `mcid` back after a command if its readback setting asks for it (slave). With cache the module
 * publishes what its command establishes; its host tells it the setting. */
static void read_back(hubusd_t *d, unsigned mcid)
{
    const hubusd_slave_t *s = &d->slave[mcid];
    hubus_status_t r;

    if (slave_readback(d, mcid) == HUBUSD_READBACK_SLAVE && info_has_state(s)) {
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

static void send_exact(hubusd_t *d, unsigned mcid, uint8_t opcode, const uint8_t *args, uint8_t n)
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
        read_back(d, mcid);
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
            read_back(d, mcid);
        }
    }
}

/* 1 if a queued request has to wait because it would reach a slave in its blind time. */
static int request_waits(const hubusd_t *d, const hubusd_request_t *q)
{
    switch (q->kind) {
    case HUBUSD_REQ_CAPTURE:
    case HUBUSD_REQ_READ:
    case HUBUSD_REQ_PROBE:
    case HUBUSD_REQ_UPDATE:
        return is_blind(d, q->prefix);
    case HUBUSD_REQ_SET_MCID:
        return is_blind(d, q->prefix) || is_blind(d, q->args[0]); /* a blind slave under the new MCID would look absent */
    case HUBUSD_REQ_RAW:
        return target_blind(d, q->target_len, q->prefix);
    case HUBUSD_REQ_SCAN:
        return any_blind(d); /* a blind slave would look absent */
    default:
        return 0;
    }
}

/* A message on a topic below a slave goes to the instance of its module that took the topic. */
static void route_message(hubusd_t *d, const hubusd_request_t *q)
{
    const hubusd_slave_t *s = &d->slave[q->prefix];
    hubusd_worker_t *w;
    uint8_t i;
    static hubusd_job_t job; /* large; the bus thread is the only one here */

    if (!s->configured) {
        reject(d, "slave %u is not configured", q->prefix);
        return;
    }
    if (!s->module) {
        reject(d, "slave %u has no module; use the raw command topic", q->prefix);
        return;
    }
    if (s->no_instance) {
        reject(d, "module %s has no instance for slave %u; '%s' was not taken", s->module->name, q->prefix, q->topic);
        return;
    }
    for (i = 0; i < s->n_topics && !hubus_module_topic_matches(s->topics[i], q->topic); i++) {
    }
    if (i == s->n_topics) {
        reject(d, "module %s of slave %u takes nothing on '%s'", s->module->name, q->prefix, q->topic);
        return;
    }
    w = worker_of(d, s->module, 0);
    if (w && hubusd_worker_failed(w)) {
        reject(d, "module %s has failed and is not called any more", s->module->name);
        return;
    }
    if (!w || s->inst_module != s->module) {
        reject(d, "module %s has no instance for slave %u", s->module->name, q->prefix);
        return;
    }
    memset(&job, 0, offsetof(hubusd_job_t, payload));
    job.kind = HUBUSD_JOB_MESSAGE;
    job.mcid = q->prefix;
    job.readback = module_readback(d, q->prefix);
    snprintf(job.topic, sizeof job.topic, "%s", q->topic);
    job.payload_len = q->text_len;
    memcpy(job.payload, q->text, q->text_len);
    job.payload[q->text_len] = '\0';
    if (!hubusd_worker_post(w, &job)) {
        d->stats.module_jobs_dropped++;
        reject(d, "module %s is busy: the message on '%s' for slave %u was dropped", s->module->name, q->topic, q->prefix);
    }
}

/* ---- firmware update from a module -------------------------------------------------------------------------------- */

static void exec_request(hubusd_t *d, const hubusd_request_t *q);

/* The bus thread only gets here between two pages of an update. What needs the bus or a module is refused at once, with the
 * reason, instead of waiting for seconds and then acting on what may be stale by then; what needs neither is done. Changes of
 * the configuration wait in their own list. */
static void refuse_during_update(hubusd_t *d, unsigned mcid)
{
    hubusd_request_t q;
    unsigned n = queue_depth(d);
    char what[HUBUSD_SUBTOPIC_LEN + 40];

    while (n-- > 0 && dequeue(d, &q)) {
        switch (q.kind) {
        case HUBUSD_REQ_REJECT:
        case HUBUSD_REQ_STATS:
        case HUBUSD_REQ_STALE:
            exec_request(d, &q);
            continue;
        case HUBUSD_REQ_MESSAGE:
            snprintf(what, sizeof what, "'%s' for slave %u", q.topic, q.prefix);
            break;
        case HUBUSD_REQ_RAW:
            if (q.target_len == HUBUS_TARGET_LEN_EXACT) {
                snprintf(what, sizeof what, "command 0x%02x for slave %u", q.opcode, q.prefix);
            } else {
                snprintf(what, sizeof what, "command 0x%02x for group %u/%u", q.opcode, q.prefix, q.target_len);
            }
            break;
        case HUBUSD_REQ_CAPTURE:
            snprintf(what, sizeof what, "capture of slave %u", q.prefix);
            break;
        case HUBUSD_REQ_READ:
            snprintf(what, sizeof what, "read of slave %u", q.prefix);
            break;
        case HUBUSD_REQ_PROBE:
            snprintf(what, sizeof what, "probe of MCID %u", q.prefix);
            break;
        case HUBUSD_REQ_UPDATE:
            snprintf(what, sizeof what, "update of slave %u", q.prefix);
            break;
        case HUBUSD_REQ_SET_MCID:
            snprintf(what, sizeof what, "change of the MCID of slave %u", q.prefix);
            break;
        case HUBUSD_REQ_SCAN:
            snprintf(what, sizeof what, "scan");
            break;
        default:
            continue;
        }
        reject(d, "the bus is busy with the firmware update of slave %u: %s refused", mcid, what);
    }
}

static void update_progress(void *ctx, uint16_t done, uint16_t total)
{
    hubusd_t *d = ((void **)ctx)[0];
    const hubusd_request_t *q = ((void **)ctx)[1];

    d->update_percent = total ? (unsigned)done * 100u / total : 0u;
    publish_firmware(d, q->prefix, 1, (int)d->update_percent);
    publish_state(d, 0);
    refuse_during_update(d, q->prefix);
}

/* The module a trigger means. NULL if it cannot be used: reported. A slave that runs an application takes only its own module:
 * in its updater it no longer says what it is, so the name decides; the signature only protects against the wrong device. */
static const hubus_module_t *update_source(hubusd_t *d, const hubusd_slave_t *s, unsigned mcid, const char *name)
{
    int in_updater = info_device_type(s) == HUBUS_HWID_TYPE_UPDATER;
    const hubus_module_t *m = NULL;
    size_t k;

    if (!s->configured) {
        reject(d, "slave %u is not configured", mcid);
        return NULL;
    }
    if (!s->have_info) {
        reject(d, "slave %u has not answered yet", mcid);
        return NULL;
    }
    if (name[0]) {
        for (k = 0; k < d->n_modules && !m; k++) {
            m = strcmp(d->modules[k]->name, name) == 0 ? d->modules[k] : NULL;
        }
        if (!m) {
            reject(d, "there is no module '%s' to update slave %u from", name, mcid);
            return NULL;
        }
        if (!in_updater && s->module != m) {
            reject(d, "slave %u uses module %s, not %s", mcid, s->module ? s->module->name : "(none)", name);
            return NULL;
        }
    } else {
        m = in_updater ? s->update_module : s->module;
        if (!m) {
            reject(d, in_updater ? "slave %u waits in its updater: name the module in the payload" : "slave %u has no module", mcid);
            return NULL;
        }
    }
    if (!m->firmware) {
        reject(d, "module %s has no firmware image", m->name);
        return NULL;
    }
    if (!in_updater && info_field(s, HUBUS_HWID_VERSION) == (int)m->firmware_version) {
        reject(d, "slave %u already runs version %u of module %s", mcid, (unsigned)m->firmware_version, m->name);
        return NULL;
    }
    return m;
}

/* Runs on the bus thread and keeps the bus until it is done. The instance of the module goes away first, since the slave
 * restarts, and comes back with what the slave says it is afterwards. */
static void run_update(hubusd_t *d, const hubusd_request_t *q)
{
    unsigned mcid = q->prefix;
    hubusd_slave_t *s = &d->slave[mcid];
    const hubus_module_t *m = update_source(d, s, mcid, q->text);
    hubus_update_image_t img;
    hubus_update_result_t res;
    hubus_slave_frame_t info;
    void *ctx[2];
    hubus_status_t r;

    if (!m) {
        return;
    }
    memset(&img, 0, sizeof img);
    memset(&res, 0, sizeof res);
    img.data = m->firmware;
    img.len = m->firmware_len;
    memcpy(img.signature, m->firmware_signature, sizeof img.signature);
    img.modules = d->modules;
    img.n_modules = d->n_modules;

    log_event(d, 0, "firmware update of slave %u from module %s: version %u, %u bytes", mcid, m->name,
              (unsigned)m->firmware_version, (unsigned)m->firmware_len);
    s->update_module = m;
    s->updating = 1;
    d->update_percent = 0;
    sync_instances(d);
    publish_firmware(d, mcid, 1, 0);
    publish_state(d, 0);
    refuse_during_update(d, mcid);

    ctx[0] = d;
    ctx[1] = (void *)q;
    r = hubus_master_update(&d->master, (uint8_t)mcid, &img, update_progress, ctx, &res);

    s->updating = 0;
    publish_state(d, 0);
    if (r == HUBUS_OK) {
        s->update_module = NULL;
        d->stats.commands_executed++;
        log_event(d, 0, "firmware update of slave %u done: %u pages, %u repeated", mcid, res.pages, res.retries);
    } else if (r == HUBUS_ERR_UPDATE) {
        log_event(d, 2, "firmware update of slave %u failed: %s", mcid, hubus_update_reason_str(res.reason));
        if (res.reason == HUBUS_UPD_REASON_NO_SUPPORT) {
            s->update_module = NULL; /* it still runs its application */
        }
    } else {
        log_event(d, 2, "firmware update of slave %u failed: %s", mcid, hubus_status_str(r));
    }
    if (r == HUBUS_ERR_LINE_FAULT) {
        sync_instances(d);
        publish_firmware(d, mcid, 0, -1);
        bus_fault(d, r);
        return;
    }
    if (hubus_master_request_info(&d->master, (uint8_t)mcid, &info) == HUBUS_OK) {
        store_info(d, mcid, &info);
        s->last_seen_ms = now_ms(d);
        s->needs_refresh = 1;
    }
    sync_instances(d);
    publish_firmware(d, mcid, 0, -1);
}

/* ---- change of the MCID ---------------------------------------------------------------------------------------------- */

static void publish_mcid_result(hubusd_t *d, unsigned mcid, unsigned new_mcid, const char *result)
{
    char buf[200], t[160];
    jw_t w;

    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    jw_uint(&w, "mcid", mcid);
    jw_uint(&w, "new_mcid", new_mcid);
    jw_str(&w, "result", result);
    jw_uint(&w, "ts", wall_s(d));
    jw_obj_end(&w);
    topic(d, t, sizeof t, "master/mcid");
    pub_json(d, &w, 0, t);
}

/*
 * Runs on the bus thread and keeps the bus until it is done (hubus_master_set_mcid()). hubusd does not write its
 * configuration: it takes the slave under the new MCID as the configuration has it there (configured, or reported as
 * unknown), and says what the configuration has to follow. The old MCID goes offline.
 */
static void apply_config(hubusd_t *d, const hubusd_runtime_cfg_t *nrt, const char *what);

/* The slave answers to another MCID now: its entry in the configuration, settings included, goes with it. */
static void move_slave_config(hubusd_t *d, unsigned mcid, unsigned new_mcid)
{
    hubusd_runtime_cfg_t *nrt = malloc(sizeof *nrt);
    hubusd_slave_cfg_t *sl;

    if (!nrt) {
        log_event(d, 2, "out of memory: the configuration still has slave %u, move it to MCID %u", mcid, new_mcid);
        return;
    }
    *nrt = d->cfg.rt;
    sl = hubusd_config_slave(nrt, mcid);
    sl->mcid = (uint8_t)new_mcid;
    apply_config(d, nrt, "changed with the MCID");
    free(nrt);
    log_event(d, 0, "the configuration has slave %u under MCID %u now", mcid, new_mcid);
    (void)persist_config(d);
}

static void run_set_mcid(hubusd_t *d, const hubusd_request_t *q)
{
    unsigned mcid = q->prefix, new_mcid = q->args[0];
    hubusd_slave_t *s = &d->slave[mcid], *n = &d->slave[new_mcid];
    hubus_update_result_t res;
    hubus_slave_frame_t info;
    hubus_status_t r;
    int move;

    log_event(d, 0, "MCID of slave %u (%s) to %u", mcid, slave_name(d, mcid), new_mcid);
    s->updating = 1; /* it restarts: no instance meanwhile */
    sync_instances(d);
    r = hubus_master_set_mcid(&d->master, (uint8_t)mcid, (uint8_t)new_mcid, d->modules, d->n_modules, &res);
    s->updating = 0;
    if (r == HUBUS_ERR_LINE_FAULT) {
        sync_instances(d);
        publish_mcid_result(d, mcid, new_mcid, hubus_status_str(r));
        bus_fault(d, r);
        return;
    }
    if (r != HUBUS_OK) {
        const char *why = r == HUBUS_ERR_UPDATE ? hubus_update_reason_str(res.reason) : hubus_status_str(r);

        log_event(d, 2, "MCID of slave %u not changed: %s", mcid, why);
        publish_mcid_result(d, mcid, new_mcid, why);
        if (s->configured && hubus_master_request_info(&d->master, (uint8_t)mcid, &info) == HUBUS_OK) {
            store_info(d, mcid, &info); /* it may have restarted */
            s->needs_refresh = 1;
        }
        sync_instances(d);
        refresh_states(d);
        return;
    }

    d->stats.commands_executed++;
    log_event(d, 0, "slave %u answers to MCID %u now", mcid, new_mcid);
    publish_mcid_result(d, mcid, new_mcid, "ok");
    n->update_module = s->update_module; /* an update that broke off goes on under the new MCID */
    s->update_module = NULL;
    s->unknown_seen = 0;
    move = s->configured && !n->configured;
    if (s->configured && !move) {
        set_online(d, mcid, 0);
        log_event(d, 1, "the configuration still has slave %u, and MCID %u is configured as well", mcid, new_mcid);
    } else {
        s->have_info = 0;
    }
    if (hubus_master_request_info(&d->master, (uint8_t)new_mcid, &info) == HUBUS_OK) {
        n->last_seen_ms = now_ms(d);
        n->ping_failures = 0;
        if (n->configured) {
            store_info(d, new_mcid, &info);
            set_online(d, new_mcid, 1);
            n->needs_refresh = 1;
        } else {
            n->info = info;
            n->have_info = 1;
            n->unknown_seen = !move;
            if (!move) {
                log_event(d, 1, "slave %u is not configured: add it with hubus/slave/%u/config/set", new_mcid, new_mcid);
            }
        }
    }
    if (move) {
        move_slave_config(d, mcid, new_mcid);
    }
    publish_state(d, 0);
    sync_instances(d);
    refresh_states(d);
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
    case HUBUSD_REQ_UPDATE:
        run_update(d, q);
        break;
    case HUBUSD_REQ_SET_MCID:
        run_set_mcid(d, q);
        break;
    case HUBUSD_REQ_MESSAGE:
        route_message(d, q);
        break;
    case HUBUSD_REQ_STALE:
        remove_stale_discovery(d, q->text);
        break;
    case HUBUSD_REQ_RAW:
        if (q->target_len == HUBUS_TARGET_LEN_EXACT) {
            if (q->opcode == HUBUS_OP_STATUS_CAPTURE) {
                capture_exact(d, q->prefix);
            } else {
                send_exact(d, q->prefix, q->opcode, q->args, q->n_args);
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

/* A discovery message (hubusd subscribes to them with mqtt.discovery_cleanup): one that names an MCID that is not configured,
 * or one of early versions, goes to the bus thread, which removes it. The broker sends back what hubusd publishes itself, so
 * the configured MCIDs are sorted out here and do not fill the queue. 1 for any topic below the discovery prefix. */
static int stale_discovery(hubusd_t *d, const char *topic_in, size_t len)
{
    size_t pl = strlen(d->cfg.mqtt.discovery_prefix);
    hubusd_request_t r;
    unsigned mcid;
    int configured, legacy = 0;

    pthread_mutex_lock(&d->lock);
    configured = d->discovery_cleanup; /* the bus thread may change it */
    pthread_mutex_unlock(&d->lock);
    if (!configured || strncmp(topic_in, d->cfg.mqtt.discovery_prefix, pl) != 0 || topic_in[pl] != '/') {
        return 0;
    }
    if (len == 0u || (mcid = discovery_mcid(d, topic_in, &legacy)) == 0u || strlen(topic_in) >= sizeof r.text) {
        return 1; /* a removal, or not one of ours */
    }
    pthread_mutex_lock(&d->lock);
    configured = (d->configured >> mcid) & 1u;
    pthread_mutex_unlock(&d->lock);
    if (!configured || legacy) {
        memset(&r, 0, sizeof r);
        r.kind = HUBUSD_REQ_STALE;
        memcpy(r.text, topic_in, strlen(topic_in) + 1u);
        enqueue(d, &r);
    }
    return 1;
}

/* master/config/set, slave/<n>/config/set, slave/<n>/config/remove: queued for the bus thread, which owns the configuration.
 * Returns 1 if `p` (below the base topic) is one of them. */
static int config_request(hubusd_t *d, const char *p, const char *topic_in, const void *payload, size_t len)
{
    hubusd_config_kind_t kind;
    hubusd_config_req_t *q;
    unsigned mcid = 0;
    const char *end;
    int full;

    if (strcmp(p, "master/config/set") == 0) {
        kind = HUBUSD_CONFIG_SET;
    } else if (strncmp(p, "slave/", 6) == 0 && parse_uint(p + 6, 999, &mcid, &end) &&
               (strcmp(end, "/config/set") == 0 || strcmp(end, "/config/remove") == 0)) {
        kind = strcmp(end, "/config/set") == 0 ? HUBUSD_CONFIG_SET_SLAVE : HUBUSD_CONFIG_REMOVE_SLAVE;
        if (mcid < HUBUS_MCID_MIN || mcid > HUBUS_MCID_MAX) {
            enqueue_reject(d, "MCID %u out of range 1..63 in '%s'", mcid, topic_in);
            return 1;
        }
    } else {
        return 0;
    }
    if (len > HUBUSD_CONFIG_PAYLOAD_MAX) {
        enqueue_reject(d, "payload of %zu bytes on '%s' is too long (at most %u)", len, topic_in, HUBUSD_CONFIG_PAYLOAD_MAX);
        return 1;
    }
    q = malloc(sizeof *q + len + 1u);
    if (!q) {
        enqueue_reject(d, "out of memory for '%s'", topic_in);
        return 1;
    }
    q->next = NULL;
    q->kind = kind;
    q->mcid = mcid;
    q->len = len;
    if (len) {
        memcpy(q->text, payload, len);
    }
    q->text[len] = '\0';
    pthread_mutex_lock(&d->lock);
    full = d->n_config >= HUBUSD_CONFIG_PENDING_MAX;
    if (!full) {
        *d->config_tail = q;
        d->config_tail = &q->next;
        d->n_config++;
    }
    pthread_mutex_unlock(&d->lock);
    if (full) {
        free(q);
        enqueue_reject(d, "too many changes of the configuration wait; '%s' is dropped", topic_in);
    }
    return 1;
}

void hubusd_handle_message(hubusd_t *d, const char *topic_in, const void *payload, size_t len)
{
    const char *base = d->cfg.mqtt.base_topic;
    size_t bl = strlen(base);
    const char *p;
    char text[PAYLOAD_MAX];
    hubusd_request_t r;
    unsigned a, b;

    if (stale_discovery(d, topic_in, len)) {
        return;
    }
    if (strncmp(topic_in, base, bl) != 0 || topic_in[bl] != '/') {
        return;
    }
    p = topic_in + bl + 1u;
    if (config_request(d, p, topic_in, payload, len)) {
        return;
    }
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
        if (!hubus_module_topic_reserved(end)) {
            /* a topic of the module of the slave: the bus thread knows which instance took it */
            if (!hubus_module_topic_valid(end, false)) {
                enqueue_reject(d, "invalid topic '%s'", topic_in);
                return;
            }
            if (len > HUBUS_MODULE_MESSAGE_MAX) {
                enqueue_reject(d, "message of %zu bytes on '%s' is too long (at most %u)", len, topic_in,
                               HUBUS_MODULE_MESSAGE_MAX);
                return;
            }
            r.kind = HUBUSD_REQ_MESSAGE;
            memcpy(r.topic, end, strlen(end) + 1u);
            memcpy(r.text, text, len + 1u);
            r.text_len = len;
        } else if (strcmp(end, "capture") == 0) {
            r.kind = HUBUSD_REQ_CAPTURE;
        } else if (strcmp(end, "read") == 0) {
            r.kind = HUBUSD_REQ_READ;
        } else if (strcmp(end, "update") == 0) {
            r.kind = HUBUSD_REQ_UPDATE;
            if (strlen(text) >= sizeof r.text) {
                enqueue_reject(d, "update payload for slave %u is too long", a);
                return;
            }
            memcpy(r.text, text, strlen(text) + 1u);
        } else if (strcmp(end, "mcid") == 0) {
            const char *rest;

            if (!parse_uint(text, 63, &b, &rest) || *rest || b < HUBUS_MCID_MIN || b == a) {
                enqueue_reject(d, "invalid new MCID '%s' for slave %u (1..63, not %u)", text, a, a);
                return;
            }
            r.kind = HUBUSD_REQ_SET_MCID;
            r.args[0] = (uint8_t)b;
            r.n_args = 1;
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

void hubusd_subscriptions(hubusd_t *d, void (*fn)(void *ctx, const char *filter), void *ctx)
{
    static const char *const filters[] = {"master/cmd/+",   "slave/+/cmd",  "slave/+/capture", "slave/+/read",
                                          "slave/+/update", "slave/+/mcid", "group/+/+/cmd",   "master/config/set",
                                          "slave/+/config/set", "slave/+/config/remove"};
    char t[160];
    unsigned i;
    int cleanup;

    for (i = 0; i < sizeof filters / sizeof filters[0]; i++) {
        topic(d, t, sizeof t, "%s", filters[i]);
        fn(ctx, t);
    }
    pthread_mutex_lock(&d->lock); /* the bus thread may change it */
    cleanup = d->discovery_cleanup;
    pthread_mutex_unlock(&d->lock);
    if (cleanup) { /* the retained discovery messages; stale_discovery() sorts them */
        snprintf(t, sizeof t, "%s/+/+/config", d->cfg.mqtt.discovery_prefix);
        fn(ctx, t);
    }
}

size_t hubusd_status_topic(const hubusd_t *d, char *out, size_t out_size)
{
    topic(d, out, out_size, "master/status");
    return strlen(out);
}

/* ---- reload and changes over MQTT ------------------------------------------------------------------------------------------------ */

/* mqtt.discovery_cleanup now: subscribes to the retained discovery messages, or gives them up. */
static void set_discovery_cleanup(hubusd_t *d, int on)
{
    char t[160];
    unsigned mcid;

    if (on == d->cfg.rt.mqtt.discovery_cleanup) {
        return;
    }
    d->cfg.rt.mqtt.discovery_cleanup = on;
    pthread_mutex_lock(&d->lock);
    d->discovery_cleanup = on;
    pthread_mutex_unlock(&d->lock);
    snprintf(t, sizeof t, "%s/+/+/config", d->cfg.mqtt.discovery_prefix);
    if ((on ? d->mqtt.subscribe : d->mqtt.unsubscribe)(d->mqtt.ctx, t) != 0) {
        log_event(d, 2, "cannot %s '%s'", on ? "subscribe to" : "unsubscribe from", t);
    }
    for (mcid = HUBUS_MCID_MIN; on && d->cfg.rt.mqtt.discovery && mcid <= HUBUS_MCID_MAX; mcid++) {
        if (!d->slave[mcid].configured) {
            remove_slave_discovery(d, mcid, NULL); /* as at the start */
        }
    }
}

/* Uses this runtime part from now on. It is all a change can bring: the fixed part (GPIO, timing, broker) is not in it. */
static void apply_config(hubusd_t *d, const hubusd_runtime_cfg_t *nrt, const char *what)
{
    unsigned mcid;
    hubusd_slave_t old[HUBUS_MCID_MAX + 1u];
    uint64_t now = now_ms(d);

    memcpy(old, d->slave, sizeof old);
    /* Remove discovery entries of slaves that are gone; the others are published again below. */
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        if (old[mcid].configured && old[mcid].module) {
            const hubus_module_t *m = old[mcid].module;
            int still = 0;
            unsigned i;

            for (i = 0; i < nrt->n_slaves; i++) {
                still |= nrt->slaves[i].mcid == mcid;
            }
            remove_slave_discovery(d, mcid, m);
            if (!still) {
                pub_topic(d, 1, "", "slave/%u/availability", mcid);
                if (m->firmware) {
                    pub_topic(d, 1, "", "slave/%u/firmware", mcid);
                }
                pub_topic(d, 1, "", "slave/%u/info", mcid);
                retained_clear(d, mcid); /* what its module published, discovery included */
            } else if (d->cfg.rt.mqtt.discovery && !nrt->mqtt.discovery) {
                retained_hide_discovery(d, mcid);
            }
        }
    }
    if (d->cfg.rt.mqtt.discovery && !nrt->mqtt.discovery) {
        remove_master_discovery(d);
    }
    if (nrt->mqtt.stats_interval_s != d->cfg.rt.mqtt.stats_interval_s) {
        d->next_stats_ms = now + (uint64_t)nrt->mqtt.stats_interval_s * 1000u;
    }
    if (nrt->bus.scan_interval_s != d->cfg.rt.bus.scan_interval_s) {
        d->next_scan_ms = now + (uint64_t)nrt->bus.scan_interval_s * 1000u;
    }
    d->cfg.rt.n_slaves = nrt->n_slaves;
    memcpy(d->cfg.rt.slaves, nrt->slaves, sizeof d->cfg.rt.slaves);
    d->cfg.rt.bus = nrt->bus;
    d->cfg.rt.mqtt.stats_interval_s = nrt->mqtt.stats_interval_s;
    d->cfg.rt.mqtt.discovery = nrt->mqtt.discovery;
    bind_config(d);
    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        d->slave[mcid].have_info = old[mcid].have_info;
        d->slave[mcid].info = old[mcid].info;
        d->slave[mcid].online = old[mcid].online;
        d->slave[mcid].last_seen_ms = old[mcid].last_seen_ms;
        d->slave[mcid].unknown_seen = d->slave[mcid].configured ? 0 : old[mcid].unknown_seen;
        d->slave[mcid].unknown_logged = old[mcid].unknown_logged;
        if (d->slave[mcid].configured) {
            resolve_module(d, mcid);
            if (!old[mcid].configured) {
                d->slave[mcid].needs_refresh = 1; /* new slave: probe and read its state */
                d->slave[mcid].online = -1;
                d->slave[mcid].next_ping_ms = now;
            }
        }
    }
    sync_instances(d);
    set_discovery_cleanup(d, nrt->mqtt.discovery_cleanup); /* after bind_config(): it knows the slaves now */
    log_event(d, 0, "configuration %s: %u slaves", what, d->cfg.rt.n_slaves);
    publish_all_retained(d);
}

static void publish_config_result(hubusd_t *d, const hubusd_config_req_t *q, const char *error, int saved)
{
    char buf[1024], t[160]; /* an error of 300 characters, escaped */
    jw_t w;

    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    if (q->kind == HUBUSD_CONFIG_SET) {
        topic(d, t, sizeof t, "master/config/set");
    } else {
        topic(d, t, sizeof t, "slave/%u/config/%s", q->mcid, q->kind == HUBUSD_CONFIG_SET_SLAVE ? "set" : "remove");
    }
    jw_str(&w, "topic", t);
    jw_str(&w, "result", error ? "error" : "ok");
    if (error) {
        jw_str(&w, "error", error);
    } else {
        jw_bool(&w, "saved", saved);
    }
    jw_uint(&w, "ts", wall_s(d));
    jw_obj_end(&w);
    topic(d, t, sizeof t, "master/config/result");
    pub_json(d, &w, 0, t);
}

/* A change over MQTT: checked on a copy of the runtime part, the only part it can reach, then used and written to the runtime
 * file. */
static void exec_config(hubusd_t *d, const hubusd_config_req_t *q)
{
    hubusd_runtime_cfg_t *nrt = malloc(sizeof *nrt);
    char err[300];
    int rc;

    if (!nrt) {
        reject(d, "out of memory for a change of the configuration");
        publish_config_result(d, q, "out of memory", 0);
        return;
    }
    *nrt = d->cfg.rt;
    err[0] = '\0';
    switch (q->kind) {
    case HUBUSD_CONFIG_SET:
        rc = hubusd_config_merge(q->text, q->len, nrt, err, sizeof err);
        break;
    case HUBUSD_CONFIG_SET_SLAVE:
        rc = hubusd_config_merge_slave(q->text, q->len, nrt, q->mcid, err, sizeof err);
        break;
    default:
        rc = hubusd_config_remove_slave(nrt, q->mcid);
        if (rc != 0) {
            snprintf(err, sizeof err, "slave %u is not configured", q->mcid);
        }
        break;
    }
    if (rc != 0) {
        reject(d, "configuration not changed: %s", err);
        publish_config_result(d, q, err, 0);
    } else {
        apply_config(d, nrt, "changed over MQTT");
        publish_config_result(d, q, NULL, persist_config(d));
    }
    free(nrt);
}

/* The changes over MQTT that have come in since the last step. Returns 1 if there was one. */
static int exec_configs(hubusd_t *d)
{
    hubusd_config_req_t *q;

    pthread_mutex_lock(&d->lock);
    q = d->config_head;
    d->config_head = NULL;
    d->config_tail = &d->config_head;
    d->n_config = 0;
    pthread_mutex_unlock(&d->lock);
    if (!q) {
        return 0;
    }
    while (q) {
        hubusd_config_req_t *next = q->next;

        exec_config(d, q);
        free(q);
        q = next;
    }
    return 1;
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
        d->cfg.modules.call_timeout_ms = nc->modules.call_timeout_ms; /* the rest of the fixed part needs a restart */
        apply_config(d, &nc->rt, "reloaded");
        free(nc);
        busy = 1;
    }
    if (exec_configs(d)) {
        busy = 1;
    }
    if (republish) {
        resubscribe_all(d); /* the topics of the instances; the client has subscribed to hubusd_subscriptions() */
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
    check_timing(d); /* a SLAVE_INFO of the last step may have raised it */

    if (d->bus != HUBUSD_BUS_OK) {
        if (!d->started && d->bus == HUBUSD_BUS_STARTING) {
            publish_master_status(d, 1);
            if (d->cfg.rt.mqtt.discovery && d->cfg.rt.mqtt.discovery_cleanup) {
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
        if (d->cfg.rt.mqtt.stats_interval_s && now >= d->next_stats_ms) {
            d->next_stats_ms = now + (uint64_t)d->cfg.rt.mqtt.stats_interval_s * 1000u;
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

    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX && d->cfg.rt.bus.ping_interval_s; mcid++) {
        if (d->slave[mcid].configured && now >= d->slave[mcid].next_ping_ms && !is_blind(d, mcid)) {
            ping(d, mcid, d->cfg.rt.bus.ping_failures);
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
        unsigned interval = s->configured ? d->cfg.rt.slaves[s->cfg_index].poll_interval_s : 0u;

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
    if (d->bus == HUBUSD_BUS_OK && d->cfg.rt.bus.scan_interval_s && now >= d->next_scan_ms && !any_blind(d)) {
        d->next_scan_ms = now + (uint64_t)d->cfg.rt.bus.scan_interval_s * 1000u;
        do_scan(d, 0);
        refresh_states(d);
        busy = 1;
    }
    if (d->cfg.rt.mqtt.stats_interval_s && now >= d->next_stats_ms) {
        d->next_stats_ms = now + (uint64_t)d->cfg.rt.mqtt.stats_interval_s * 1000u;
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
