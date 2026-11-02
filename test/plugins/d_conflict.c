/* The same rule as a_good under another name: no module could win for such a device. */
#include "hubus_module.h"

static const hubus_module_rule_t rules[] = {
    {.pattern = {0x90, 0x01}, .mask = {0xFF, 0xFF}, .len = 2},
};

static const hubus_module_t module = {
    HUBUS_MODULE_INIT,
    .name = "plugin_conflict",
    .rules = rules,
    .n_rules = HUBUS_MODULE_LEN(rules),
    .max_command = 1,
};

const hubus_module_t *hubus_module_entry(void)
{
    return &module;
}
