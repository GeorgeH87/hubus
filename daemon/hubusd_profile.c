#include "hubusd_profile.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

void hubusd_profile_init(hubusd_profile_t *p)
{
    memset(p, 0, sizeof *p);
    p->device_type = -1;
    p->component = HUBUSD_COMP_SENSOR;
    p->byte_len = 1;
    p->scale = 1.0;
}

static hubusd_profile_t builtin[3];
static int builtin_ready;

static void build_builtin(void)
{
    hubusd_profile_t *r = &builtin[0];
    hubusd_profile_t *s = &builtin[1];
    hubusd_profile_t *t = &builtin[2];

    hubusd_profile_init(r);
    snprintf(r->name, sizeof r->name, "relay");
    r->device_type = 0x01; /* DEVICE_TYPE_RELAY in examples/slave-relay */
    r->component = HUBUSD_COMP_SWITCH;
    r->on = (hubusd_cmd_t){1, 0x10, 1, {1}};
    r->off = (hubusd_cmd_t){1, 0x10, 1, {0}};

    hubusd_profile_init(s);
    snprintf(s->name, sizeof s->name, "switch");
    s->device_type = 0x02; /* DEVICE_TYPE_SWITCH in examples/slave-switch */
    s->component = HUBUSD_COMP_BINARY_SENSOR;

    hubusd_profile_init(t);
    snprintf(t->name, sizeof t->name, "temperature");
    t->device_type = 0x03; /* DEVICE_TYPE_TEMPERATURE in examples/slave-sensor */
    t->component = HUBUSD_COMP_SENSOR;
    t->byte_len = 2;
    t->big_endian = 1;
    /* ATtiny85 internal sensor: about 1 LSB per kelvin, 273 LSB at 0 degC. Needs a per-device calibration. */
    t->scale = 1.0;
    t->add = -273.0;
    t->decimals = 0;
    snprintf(t->unit, sizeof t->unit, "\xC2\xB0" "C");
    snprintf(t->device_class, sizeof t->device_class, "temperature");
    builtin_ready = 1;
}

const hubusd_profile_t *hubusd_builtin_profile(const char *name)
{
    unsigned i;

    if (!builtin_ready) {
        build_builtin();
    }
    for (i = 0; i < 3; i++) {
        if (strcmp(builtin[i].name, name) == 0) {
            return &builtin[i];
        }
    }
    return NULL;
}

const hubusd_profile_t *hubusd_builtin_profile_by_type(int device_type)
{
    unsigned i;

    if (!builtin_ready) {
        build_builtin();
    }
    for (i = 0; i < 3; i++) {
        if (builtin[i].device_type == device_type) {
            return &builtin[i];
        }
    }
    return NULL;
}

size_t hubusd_profile_decode(const hubusd_profile_t *p, const uint8_t *data, uint8_t len, char *out, size_t out_size)
{
    int32_t raw;
    int n;

    if (p->byte_len == 0 || (unsigned)p->byte_offset + p->byte_len > len) {
        return 0;
    }
    if (p->byte_len == 1) {
        raw = p->is_signed ? (int32_t)(int8_t)data[p->byte_offset] : (int32_t)data[p->byte_offset];
    } else {
        uint16_t hi = p->big_endian ? data[p->byte_offset] : data[p->byte_offset + 1];
        uint16_t lo = p->big_endian ? data[p->byte_offset + 1] : data[p->byte_offset];
        uint16_t v = (uint16_t)((hi << 8) | lo);

        raw = p->is_signed ? (int32_t)(int16_t)v : (int32_t)v;
    }
    if (p->component == HUBUSD_COMP_SENSOR) {
        n = snprintf(out, out_size, "%.*f", p->decimals, (double)raw * p->scale + p->add);
    } else {
        n = snprintf(out, out_size, "%s", raw != 0 ? "ON" : "OFF");
    }
    return (n > 0 && (size_t)n < out_size) ? (size_t)n : 0;
}

int hubusd_profile_command(const hubusd_profile_t *p, const char *text, hubusd_cmd_t *cmd)
{
    if (p->component != HUBUSD_COMP_SWITCH) {
        return 0;
    }
    if (strcasecmp(text, "ON") == 0 && p->on.valid) {
        *cmd = p->on;
        return 1;
    }
    if (strcasecmp(text, "OFF") == 0 && p->off.valid) {
        *cmd = p->off;
        return 1;
    }
    return 0;
}
