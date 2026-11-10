/*
 * The module of the distance sensor (examples/slave-distance, docs/example-distance.md): the distance, whether an object is
 * nearer than the trigger distance, and the trigger distance itself, which the user sets here and the master sends to the slave.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hubusd_modules.h"

#define OP_SET_THRESHOLD 0x40u

#define DEFAULT_CM 30u
#define MAX_CM 400u

/* State and events of the slave: near, distance in mm (2 bytes), threshold in mm (2 bytes). */
#define STATE_THRESHOLD 3u
#define STATE_LEN 5u

struct hubus_module_instance {
    uint16_t threshold_cm; /* what the slave should have; the slave forgets it at a power-up */
};

static hubus_module_status_t send_threshold(const hubus_module_instance_t *self, const hubus_module_host_t *host)
{
    uint16_t mm = (uint16_t)(self->threshold_cm * 10u);
    uint8_t args[2] = {(uint8_t)(mm >> 8), (uint8_t)mm};

    return host->send(host, OP_SET_THRESHOLD, args, sizeof args);
}

static void publish_threshold(const hubus_module_instance_t *self, const hubus_module_host_t *host)
{
    char text[8];

    snprintf(text, sizeof text, "%u", self->threshold_cm);
    (void)host->set_state(host, "threshold", text);
}

static hubus_module_instance_t *distance_create(const hubus_module_host_t *host)
{
    hubus_module_instance_t *self = calloc(1, sizeof *self);

    if (self) {
        self->threshold_cm = DEFAULT_CM;
        publish_threshold(self, host);
    }
    return self;
}

static void distance_destroy(hubus_module_instance_t *self)
{
    free(self);
}

static hubus_module_status_t distance_on_set(hubus_module_instance_t *self, const hubus_module_host_t *host,
                                             const hubus_entity_t *entity, const hubus_value_t *value)
{
    uint16_t before = self->threshold_cm;
    hubus_module_status_t st;

    if (value->type != HUBUS_VALUE_NUMBER || strcmp(entity->key, "threshold") != 0 || !(value->as.number >= 0.0) ||
        value->as.number > (double)MAX_CM) {
        return HUBUS_MODULE_REJECTED;
    }
    self->threshold_cm = (uint16_t)(value->as.number + 0.5);
    st = send_threshold(self, host);
    if (st != HUBUS_MODULE_OK) {
        self->threshold_cm = before; /* so that the same value can try again */
        return st;
    }
    publish_threshold(self, host);
    return HUBUS_MODULE_OK;
}

/*
 * Every frame of the slave carries its threshold. After a power-up it is 0, and so it is for any frame that does not agree with
 * the value the user chose: the module sends it again. A frame that was on its way while the user changed the value makes one
 * more send of the same value, which does no harm.
 */
static void distance_on_frame(hubus_module_instance_t *self, const hubus_module_host_t *host, hubus_frame_kind_t kind,
                              const uint8_t *data, uint8_t len, bool gap)
{
    uint16_t mm;

    (void)kind;
    (void)gap;
    if (len < STATE_LEN) {
        return;
    }
    mm = (uint16_t)(data[STATE_THRESHOLD] << 8 | data[STATE_THRESHOLD + 1u]);
    if (mm != (uint16_t)(self->threshold_cm * 10u)) {
        (void)send_threshold(self, host);
    }
}

/* The distance is 0 when there is no echo: nothing within about 5 m, or an object closer than 2 cm. */
static const hubus_module_rule_t distance_rules[] = {{.pattern = {0x87, 0x01}, .mask = {0xFF, 0xFF}, .len = 2}};
static const hubus_entity_t distance_entities[] = {
    {
        .key = "distance",
        .name = "Distance",
        .component = HUBUS_COMPONENT_SENSOR,
        .device_class = HUBUS_DEVICE_CLASS_DISTANCE,
        .unit = HUBUS_UNIT_CENTIMETER,
        .decode = {.offset = 1, .len = 2, .big_endian = true, .scale = 0.1, .decimals = 1},
    },
    {
        .key = "near",
        .name = "Object near",
        .component = HUBUS_COMPONENT_BINARY_SENSOR,
        .decode = {.offset = 0, .len = 1, .scale = 1.0},
    },
    {
        .key = "threshold",
        .name = "Trigger distance",
        .component = HUBUS_COMPONENT_NUMBER,
        .device_class = HUBUS_DEVICE_CLASS_DISTANCE,
        .unit = HUBUS_UNIT_CENTIMETER,
        .min = 0,
        .max = MAX_CM,
        .step = 1,
    },
};

const hubus_module_t hubusd_distance_module = {
    HUBUS_MODULE_INIT,
    .name = "distance",
    .rules = distance_rules,
    .n_rules = HUBUS_MODULE_LEN(distance_rules),
    .max_command = HUBUS_MAX_PAYLOAD,
    .has_state = true,
    .has_events = true,
    .has_status_capture = true,
    .has_update = true,
    .entities = distance_entities,
    .n_entities = HUBUS_MODULE_LEN(distance_entities),
    .create = distance_create,
    .destroy = distance_destroy,
    .on_frame = distance_on_frame,
    .on_set = distance_on_set,
};
