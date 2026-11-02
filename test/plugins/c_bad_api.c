/* A plugin built for another interface version. */
#include "hubus_module.h"

static const hubus_module_rule_t rules[] = {
    {.pattern = {0x92}, .mask = {0xFF}, .len = 1},
};

static const hubus_module_t module = {
    .api_version = HUBUS_MODULE_API_VERSION + 1u,
    .struct_size = sizeof(hubus_module_t),
    .name = "plugin_bad_api",
    .rules = rules,
    .n_rules = HUBUS_MODULE_LEN(rules),
    .max_command = 1,
};

const hubus_module_t *hubus_module_entry(void)
{
    return &module;
}
