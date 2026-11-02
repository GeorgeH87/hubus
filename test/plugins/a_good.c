/* A plugin that is as simple as a module gets: data only, no functions. */
#include "hubus_module.h"

static const hubus_module_rule_t rules[] = {
    {.pattern = {0x90, 0x01}, .mask = {0xFF, 0xFF}, .len = 2},
};

static const hubus_entity_t entities[] = {
    {
        .key = "temperature",
        .name = "Temperature",
        .component = HUBUS_COMPONENT_SENSOR,
        .device_class = HUBUS_DEVICE_CLASS_TEMPERATURE,
        .unit = HUBUS_UNIT_CELSIUS,
        .decode = {.offset = 0, .len = 2, .big_endian = true, .scale = 1.0, .add = -273.0},
    },
};

static const hubus_module_t module = {
    HUBUS_MODULE_INIT,
    .name = "plugin_good",
    .rules = rules,
    .n_rules = HUBUS_MODULE_LEN(rules),
    .max_command = 1,
    .has_state = true,
    .entities = entities,
    .n_entities = HUBUS_MODULE_LEN(entities),
};

const hubus_module_t *hubus_module_entry(void)
{
    return &module;
}
