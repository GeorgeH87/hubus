/* The name of a_good for another device. */
#include "hubus_module.h"

static const hubus_module_rule_t rules[] = {
    {.pattern = {0x91}, .mask = {0xFF}, .len = 1},
};

static const hubus_module_t module = {
    HUBUS_MODULE_INIT,
    .name = "plugin_good",
    .rules = rules,
    .n_rules = HUBUS_MODULE_LEN(rules),
    .max_command = 1,
};

const hubus_module_t *hubus_module_entry(void)
{
    return &module;
}
