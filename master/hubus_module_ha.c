/*
 * Entities of Home Assistant for master modules: topics, values, and discovery from the table of a module.
 */
#include "hubus_module_ha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hubus_json.h"

#define MODULE_MAX_PAYLOAD 31u
#define MODULE_MAX_DECIMALS 6u
#define TOPIC_LEN (HUBUS_MODULE_TOPIC_MAX + 1u)
#define FULL_TOPIC_LEN 192u

/* ---- the table ------------------------------------------------------------------------------------------------------ */

const hubus_ha_t *hubus_ha_of(const hubus_module_t *m)
{
    static const hubus_ha_t empty = {NULL, 0, NULL};

    return m->data ? (const hubus_ha_t *)m->data : &empty;
}

static bool key_valid(const char *key)
{
    size_t i;

    if (key == NULL || key[0] == '\0' || strlen(key) > HUBUS_ENTITY_KEY_MAX || hubus_module_topic_reserved(key)) {
        return false;
    }
    for (i = 0; key[i] != '\0'; i++) {
        char c = key[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool decode_valid(const hubus_module_decode_t *d)
{
    if (d->len > 2u || (unsigned)d->offset + d->len > MODULE_MAX_PAYLOAD || d->decimals > MODULE_MAX_DECIMALS) {
        return false;
    }
    return d->len == 0u || d->scale != 0.0;
}

static bool command_valid(const hubus_module_command_t *c, uint8_t max_command)
{
    return !c->used || (c->n_args <= HUBUS_MODULE_CMD_MAX_ARGS && (unsigned)c->n_args + 1u <= max_command);
}

static bool entity_valid(const hubus_module_t *m, const hubus_ha_t *ha, const hubus_entity_t *e)
{
    if (!key_valid(e->key) || e->name == NULL || (unsigned)e->component >= HUBUS_COMPONENT_COUNT ||
        (unsigned)e->device_class >= HUBUS_DEVICE_CLASS_COUNT || (unsigned)e->unit >= HUBUS_UNIT_COUNT ||
        !decode_valid(&e->decode) || !command_valid(&e->on, m->max_command) ||
        !command_valid(&e->off, m->max_command)) {
        return false;
    }
    switch (e->component) {
    case HUBUS_COMPONENT_SENSOR:
    case HUBUS_COMPONENT_BINARY_SENSOR:
        return e->decode.len != 0u;
    case HUBUS_COMPONENT_SWITCH:
        return ha->on_set != NULL || (e->on.used && e->off.used);
    case HUBUS_COMPONENT_LIGHT:
    case HUBUS_COMPONENT_TEXT:
        return ha->on_set != NULL;
    case HUBUS_COMPONENT_NUMBER:
        return ha->on_set != NULL && e->min < e->max && e->step >= 0.0 && e->step <= e->max - e->min;
    default:
        return false;
    }
}

bool hubus_ha_valid(const hubus_module_t *m)
{
    const hubus_ha_t *ha = hubus_ha_of(m);
    uint8_t i, j;

    if ((ha->n_entities > 0u && ha->entities == NULL) || ha->n_entities > HUBUS_MODULE_ENTITIES_MAX) {
        return false;
    }
    for (i = 0; i < ha->n_entities; i++) {
        if (!entity_valid(m, ha, &ha->entities[i])) {
            return false;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(ha->entities[i].key, ha->entities[j].key) == 0) {
                return false;
            }
        }
    }
    return true;
}

int hubus_ha_find_entity(const hubus_ha_t *ha, const char *key)
{
    uint8_t i;

    for (i = 0; i < ha->n_entities; i++) {
        if (strcmp(ha->entities[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

size_t hubus_entity_decode(const hubus_entity_t *e, const uint8_t *data, uint8_t len, char *out, size_t out_size)
{
    const hubus_module_decode_t *d = &e->decode;
    int32_t raw;
    int n;

    if (d->len == 0u || (unsigned)d->offset + d->len > len) {
        return 0;
    }
    if (d->len == 1u) {
        raw = d->is_signed ? (int32_t)(int8_t)data[d->offset] : (int32_t)data[d->offset];
    } else {
        uint16_t hi = d->big_endian ? data[d->offset] : data[d->offset + 1u];
        uint16_t lo = d->big_endian ? data[d->offset + 1u] : data[d->offset];
        uint16_t v = (uint16_t)((hi << 8) | lo);

        raw = d->is_signed ? (int32_t)(int16_t)v : (int32_t)v;
    }
    if (e->component == HUBUS_COMPONENT_SENSOR) {
        n = snprintf(out, out_size, "%.*f", d->decimals, (double)raw * d->scale + d->add);
    } else {
        n = snprintf(out, out_size, "%s", raw != 0 ? "ON" : "OFF");
    }
    return (n > 0 && (size_t)n < out_size) ? (size_t)n : 0u;
}

/* ---- names of Home Assistant ------------------------------------------------------------------------------------------- */

const char *hubus_ha_component_name(hubus_component_t component)
{
    switch (component) {
    case HUBUS_COMPONENT_SENSOR: return "sensor";
    case HUBUS_COMPONENT_BINARY_SENSOR: return "binary_sensor";
    case HUBUS_COMPONENT_SWITCH: return "switch";
    case HUBUS_COMPONENT_LIGHT: return "light";
    case HUBUS_COMPONENT_TEXT: return "text";
    case HUBUS_COMPONENT_NUMBER: return "number";
    case HUBUS_COMPONENT_COUNT: break;
    }
    return "sensor";
}

const char *hubus_ha_device_class_name(hubus_device_class_t device_class)
{
    switch (device_class) {
    case HUBUS_DEVICE_CLASS_TEMPERATURE: return "temperature";
    case HUBUS_DEVICE_CLASS_HUMIDITY: return "humidity";
    case HUBUS_DEVICE_CLASS_ILLUMINANCE: return "illuminance";
    case HUBUS_DEVICE_CLASS_PRESSURE: return "pressure";
    case HUBUS_DEVICE_CLASS_POWER: return "power";
    case HUBUS_DEVICE_CLASS_VOLTAGE: return "voltage";
    case HUBUS_DEVICE_CLASS_CURRENT: return "current";
    case HUBUS_DEVICE_CLASS_MOTION: return "motion";
    case HUBUS_DEVICE_CLASS_DOOR: return "door";
    case HUBUS_DEVICE_CLASS_DISTANCE: return "distance";
    case HUBUS_DEVICE_CLASS_NONE:
    case HUBUS_DEVICE_CLASS_COUNT: break;
    }
    return NULL;
}

const char *hubus_ha_unit_text(hubus_unit_t unit)
{
    switch (unit) {
    case HUBUS_UNIT_CELSIUS: return "\xC2\xB0" "C";
    case HUBUS_UNIT_PERCENT: return "%";
    case HUBUS_UNIT_LUX: return "lx";
    case HUBUS_UNIT_HECTOPASCAL: return "hPa";
    case HUBUS_UNIT_WATT: return "W";
    case HUBUS_UNIT_VOLT: return "V";
    case HUBUS_UNIT_AMPERE: return "A";
    case HUBUS_UNIT_CENTIMETER: return "cm";
    case HUBUS_UNIT_NONE:
    case HUBUS_UNIT_COUNT: break;
    }
    return NULL;
}

/* ---- announcing the entities ------------------------------------------------------------------------------------------- */

static bool takes_values(const hubus_entity_t *e)
{
    return e->component == HUBUS_COMPONENT_SWITCH || e->component == HUBUS_COMPONENT_LIGHT ||
           e->component == HUBUS_COMPONENT_TEXT || e->component == HUBUS_COMPONENT_NUMBER;
}

/* `topic` of the entity, as its full name in the discovery message. */
static void jw_topic(jw_t *w, const hubus_module_host_t *host, const char *name, const char *key, const char *topic)
{
    char sub[TOPIC_LEN], full[FULL_TOPIC_LEN];

    snprintf(sub, sizeof sub, "%s/%s", key, topic);
    if (host->topic(host, sub, full, sizeof full) == 0u) {
        full[0] = '\0';
    }
    jw_str(w, name, full);
}

static hubus_module_status_t publish_discovery(const hubus_module_host_t *host, const hubus_ha_t *ha, const hubus_entity_t *e)
{
    char buf[HUBUS_MODULE_PAYLOAD_MAX + 1u];
    const char *text;
    jw_t w;

    jw_init(&w, buf, sizeof buf);
    jw_obj_begin(&w, NULL);
    if (ha->n_entities == 1u) {
        jw_null(&w, "name"); /* the entity carries the name of the device */
    } else {
        jw_str(&w, "name", e->name);
    }
    jw_bool(&w, "has_entity_name", 1);
    jw_topic(&w, host, "state_topic", e->key, "state");
    if (takes_values(e)) {
        jw_topic(&w, host, "command_topic", e->key, "set");
    }
    if (e->component == HUBUS_COMPONENT_LIGHT) {
        jw_topic(&w, host, "rgb_command_topic", e->key, "rgb/set");
    }
    if ((text = hubus_ha_unit_text(e->unit)) != NULL) {
        jw_str(&w, "unit_of_measurement", text);
    }
    if ((text = hubus_ha_device_class_name(e->device_class)) != NULL) {
        jw_str(&w, "device_class", text);
    }
    if (e->component == HUBUS_COMPONENT_SENSOR) {
        jw_str(&w, "state_class", "measurement");
    } else if (e->component == HUBUS_COMPONENT_TEXT) {
        jw_uint(&w, "max", HUBUS_MODULE_TEXT_MAX);
    } else if (e->component == HUBUS_COMPONENT_NUMBER) {
        jw_double(&w, "min", e->min, 3);
        jw_double(&w, "max", e->max, 3);
        jw_double(&w, "step", e->step > 0.0 ? e->step : 1.0, 3);
        jw_str(&w, "mode", "box");
    } else {
        jw_str(&w, "payload_on", "ON");
        jw_str(&w, "payload_off", "OFF");
        if (e->component == HUBUS_COMPONENT_SWITCH) {
            jw_str(&w, "state_on", "ON");
            jw_str(&w, "state_off", "OFF");
        }
    }
    jw_obj_end(&w);
    if (!jw_ok(&w)) {
        return HUBUS_MODULE_REJECTED;
    }
    return host->discovery(host, hubus_ha_component_name(e->component), e->key, buf);
}

static hubus_module_status_t subscribe_entity(const hubus_module_host_t *host, const hubus_entity_t *e, const char *topic)
{
    char sub[TOPIC_LEN];

    snprintf(sub, sizeof sub, "%s/%s", e->key, topic);
    return host->subscribe(host, sub);
}

hubus_module_status_t hubus_ha_announce(const hubus_module_host_t *host)
{
    const hubus_ha_t *ha = hubus_ha_of(host->module);
    hubus_module_status_t st = HUBUS_MODULE_OK;
    uint8_t i;

    if (!hubus_ha_valid(host->module)) {
        (void)host->log(host, "the table of entities of the module is not valid");
        return HUBUS_MODULE_REJECTED;
    }
    for (i = 0; i < ha->n_entities && st == HUBUS_MODULE_OK; i++) {
        const hubus_entity_t *e = &ha->entities[i];

        if (takes_values(e)) {
            st = subscribe_entity(host, e, "set");
        }
        if (st == HUBUS_MODULE_OK && e->component == HUBUS_COMPONENT_LIGHT) {
            st = subscribe_entity(host, e, "rgb/set");
        }
        if (st == HUBUS_MODULE_OK) {
            st = publish_discovery(host, ha, e);
        }
    }
    return st;
}

hubus_module_status_t hubus_ha_publish_state(const hubus_module_host_t *host, const char *key, const char *text)
{
    char sub[TOPIC_LEN];

    if (key == NULL || text == NULL || hubus_ha_find_entity(hubus_ha_of(host->module), key) < 0 ||
        strlen(text) > HUBUS_MODULE_TEXT_MAX) {
        return HUBUS_MODULE_REJECTED;
    }
    snprintf(sub, sizeof sub, "%s/state", key);
    return host->publish(host, sub, text, strlen(text), true);
}

/* ---- the functions of a module that is all table -------------------------------------------------------------------- */

static char no_state; /* the instance of such a module: it keeps nothing */

hubus_module_instance_t *hubus_ha_create(const hubus_module_host_t *host)
{
    return hubus_ha_announce(host) == HUBUS_MODULE_OK ? (hubus_module_instance_t *)(void *)&no_state : NULL;
}

void hubus_ha_destroy(hubus_module_instance_t *self)
{
    (void)self;
}

void hubus_ha_on_frame(hubus_module_instance_t *self, const hubus_module_host_t *host, hubus_frame_kind_t kind,
                       const uint8_t *data, uint8_t len, bool gap)
{
    const hubus_ha_t *ha = hubus_ha_of(host->module);
    char text[32];
    uint8_t i;

    (void)self;
    (void)kind;
    (void)gap;
    for (i = 0; i < ha->n_entities; i++) {
        if (hubus_entity_decode(&ha->entities[i], data, len, text, sizeof text)) {
            (void)hubus_ha_publish_state(host, ha->entities[i].key, text);
        }
    }
}

static void rejectf(const hubus_module_host_t *host, const char *fmt, const char *a, const char *b)
{
    char msg[HUBUS_MODULE_LOG_MAX + 1u];

    snprintf(msg, sizeof msg, fmt, a, b);
    (void)host->reject(host, msg);
}

/* ON or OFF, in any case. */
static bool parse_on_off(const char *text, bool *on)
{
    if (strcasecmp(text, "ON") == 0) {
        *on = true;
        return true;
    }
    if (strcasecmp(text, "OFF") == 0) {
        *on = false;
        return true;
    }
    return false;
}

/* "r,g,b" with three numbers 0..255. */
static bool parse_rgb(const char *text, hubus_value_t *v)
{
    unsigned r, g, b;
    char extra;

    if (sscanf(text, " %u , %u , %u %c", &r, &g, &b, &extra) != 3 || r > 255u || g > 255u || b > 255u) {
        return false;
    }
    v->type = HUBUS_VALUE_RGB;
    v->as.rgb.r = (uint8_t)r;
    v->as.rgb.g = (uint8_t)g;
    v->as.rgb.b = (uint8_t)b;
    return true;
}

static void set_value(hubus_module_instance_t *self, const hubus_module_host_t *host, const hubus_ha_t *ha,
                      const hubus_entity_t *e, const hubus_value_t *v)
{
    hubus_module_status_t st;

    if (!ha->on_set) {
        rejectf(host, "entity '%s' has no command and its module %s has no function for it", e->key, host->module->name);
        return;
    }
    st = ha->on_set(self, host, e, v);
    if (st != HUBUS_MODULE_OK) {
        rejectf(host, st == HUBUS_MODULE_REJECTED ? "module %s rejected the value for entity '%s'"
                                                  : "module %s could not use the value for entity '%s'",
                host->module->name, e->key);
    }
}

/* The fixed command of a switch. With readback cache nobody reads the slave: the state is what the command establishes. */
static void switch_fixed(const hubus_module_host_t *host, const hubus_entity_t *e, bool on)
{
    const hubus_module_command_t *cmd = on ? &e->on : &e->off;

    if (host->send(host, cmd->opcode, cmd->args, cmd->n_args) != HUBUS_MODULE_OK) {
        rejectf(host, "the command for entity '%s' of module %s could not be queued", e->key, host->module->name);
    } else if (host->readback == HUBUS_READBACK_CACHE) {
        (void)hubus_ha_publish_state(host, e->key, on ? "ON" : "OFF");
    }
}

void hubus_ha_on_message(hubus_module_instance_t *self, const hubus_module_host_t *host, const char *topic,
                         const char *payload, size_t len)
{
    const hubus_ha_t *ha = hubus_ha_of(host->module);
    char key[TOPIC_LEN];
    const char *slash = strchr(topic, '/');
    size_t key_len = slash ? (size_t)(slash - topic) : 0u;
    const hubus_entity_t *e;
    hubus_value_t v;
    bool rgb;
    int idx;

    if (!slash || key_len >= sizeof key || (strcmp(slash, "/set") != 0 && strcmp(slash, "/rgb/set") != 0)) {
        rejectf(host, "module %s takes nothing on '%s'", host->module->name, topic);
        return;
    }
    memcpy(key, topic, key_len);
    key[key_len] = '\0';
    rgb = slash[1] == 'r';
    if ((idx = hubus_ha_find_entity(ha, key)) < 0) {
        rejectf(host, "module %s has no entity '%s'", host->module->name, key);
        return;
    }
    e = &ha->entities[idx];
    memset(&v, 0, sizeof v);
    if (rgb) {
        if (e->component != HUBUS_COMPONENT_LIGHT) {
            rejectf(host, "entity '%s' is no light and takes no colour%s", key, "");
        } else if (!parse_rgb(payload, &v)) {
            rejectf(host, "'%s' is not a colour for entity '%s' (r,g,b with numbers 0..255)", payload, key);
        } else {
            set_value(self, host, ha, e, &v);
        }
    } else if (e->component == HUBUS_COMPONENT_TEXT) {
        if (len > HUBUS_MODULE_TEXT_MAX) {
            rejectf(host, "the text for entity '%s' is longer than %s characters", key, "95");
        } else {
            v.type = HUBUS_VALUE_TEXT;
            v.as.text = payload;
            set_value(self, host, ha, e, &v);
        }
    } else if (e->component == HUBUS_COMPONENT_NUMBER) {
        char *end;
        double num = strtod(payload, &end);

        if (end == payload || *end != '\0' || !(num >= e->min && num <= e->max)) {
            char range[HUBUS_MODULE_TOPIC_MAX + 48u];

            snprintf(range, sizeof range, "%s (%g..%g)", key, e->min, e->max);
            rejectf(host, "'%s' is not a number for entity '%s'", payload, range);
        } else {
            v.type = HUBUS_VALUE_NUMBER;
            v.as.number = num;
            set_value(self, host, ha, e, &v);
        }
    } else if (e->component != HUBUS_COMPONENT_SWITCH && e->component != HUBUS_COMPONENT_LIGHT) {
        rejectf(host, "entity '%s' takes no commands%s", key, "");
    } else if (!parse_on_off(payload, &v.as.on)) {
        rejectf(host, "'%s' is not a valid command for entity '%s' (ON or OFF)", payload, key);
    } else if (e->component == HUBUS_COMPONENT_SWITCH && e->on.used) {
        switch_fixed(host, e, v.as.on);
    } else {
        v.type = HUBUS_VALUE_BOOL;
        set_value(self, host, ha, e, &v);
    }
}
