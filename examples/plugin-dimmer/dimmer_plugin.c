/*
 * A master module as a plugin, with functions: a dimmer for a lamp in one colour (docs/master-library.md, Master Modules).
 *
 * Home Assistant shows a light. The slave, which is not part of this repository, takes one opcode with the brightness and
 * reports its brightness as its state:
 *
 *   0x10 SET_LEVEL  level     0..255
 *   state           [0] level
 *
 * Install it where hubusd looks for plugins (modules.dir). The instance remembers the level, so that ON returns to the level
 * before OFF; the first frame of the slave tells it what the slave shows.
 */
#include <stdlib.h>

#include "hubus_module.h"

#define OP_SET_LEVEL 0x10u
#define FULL 255u

struct hubus_module_instance {
    uint8_t level; /* for ON; never 0 */
};

static hubus_module_instance_t *dimmer_create(const hubus_module_host_t *host)
{
    hubus_module_instance_t *self = malloc(sizeof *self);

    (void)host;
    if (self) {
        self->level = FULL;
    }
    return self;
}

static void dimmer_destroy(hubus_module_instance_t *self)
{
    free(self);
}

static uint8_t brightest(const hubus_value_t *value)
{
    uint8_t level = value->as.rgb.r;

    if (value->as.rgb.g > level) {
        level = value->as.rgb.g;
    }
    return value->as.rgb.b > level ? value->as.rgb.b : level;
}

/* The light has one channel, so a colour counts by its brightest part. Runs on the thread of the module: only queues. */
static hubus_module_status_t dimmer_on_set(hubus_module_instance_t *self, const hubus_module_host_t *host,
                                           const hubus_entity_t *entity, const hubus_value_t *value)
{
    uint8_t level;

    (void)entity;
    if (value->type == HUBUS_VALUE_BOOL) {
        level = value->as.on ? self->level : 0u;
    } else if (value->type == HUBUS_VALUE_RGB) {
        level = brightest(value);
        if (level != 0u) {
            self->level = level;
        }
    } else {
        return HUBUS_MODULE_REJECTED;
    }
    return host->send(host, OP_SET_LEVEL, &level, 1);
}

/* The state of the light follows the slave: a command that got lost shows, and so does a slave that restarted. */
static void dimmer_on_frame(hubus_module_instance_t *self, const hubus_module_host_t *host, hubus_frame_kind_t kind,
                            const uint8_t *data, uint8_t len, bool gap)
{
    (void)kind;
    (void)gap;
    if (len < 1u) {
        return;
    }
    if (data[0] != 0u) {
        self->level = data[0];
    }
    (void)host->set_state(host, "light", data[0] != 0u ? "ON" : "OFF");
}

static const hubus_module_rule_t rules[] = {
    {.pattern = {0x85, 0x01}, .mask = {0xFF, 0xFF}, .len = 2},
};

static const hubus_entity_t entities[] = {
    {.key = "light", .name = "Light", .component = HUBUS_COMPONENT_LIGHT},
};

static const hubus_module_t module = {
    HUBUS_MODULE_INIT,
    .name = "dimmer",
    .rules = rules,
    .n_rules = HUBUS_MODULE_LEN(rules),
    .max_command = 2,
    .has_state = true,
    .entities = entities,
    .n_entities = HUBUS_MODULE_LEN(entities),
    .create = dimmer_create,
    .destroy = dimmer_destroy,
    .on_frame = dimmer_on_frame,
    .on_set = dimmer_on_set,
};

const hubus_module_t *hubus_module_entry(void)
{
    return &module;
}
