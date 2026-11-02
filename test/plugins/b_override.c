/* Version 5 of the same device: a rule with more mask bits wins over the rule of a_good. */
#include "hubus_module.h"

static const hubus_module_rule_t rules[] = {
    {.pattern = {0x90, 0x01, 0x05}, .mask = {0xFF, 0xFF, 0xFF}, .len = 3},
};

static const hubus_module_t module = {
    HUBUS_MODULE_INIT,
    .name = "plugin_override",
    .rules = rules,
    .n_rules = HUBUS_MODULE_LEN(rules),
    .max_command = 8,
};

const hubus_module_t *hubus_module_entry(void)
{
    return &module;
}
