#include "hubusd_modules.h"

#define RULE_VARIANT(type, variant) {.pattern = {(type), (variant)}, .mask = {0xFF, 0xFF}, .len = 2}

#define OP_LED_SHOW 0x42u

static const hubus_module_rule_t relay_rules[] = {RULE_VARIANT(0x81, 0x01)};
static const hubus_module_t relay = {
    HUBUS_MODULE_INIT,
    .name = "relay",
    .rules = relay_rules,
    .n_rules = HUBUS_MODULE_LEN(relay_rules),
    .max_command = HUBUS_MAX_PAYLOAD,
    .has_state = true,
    .has_events = true,
    .has_status_capture = true,
};

static const hubus_module_rule_t relay_simple_rules[] = {RULE_VARIANT(0x81, 0x02)};
static const hubus_module_t relay_simple = {
    HUBUS_MODULE_INIT,
    .name = "relay-simple",
    .rules = relay_simple_rules,
    .n_rules = HUBUS_MODULE_LEN(relay_simple_rules),
    .max_command = HUBUS_MAX_PAYLOAD,
};

static const hubus_module_rule_t switch_rules[] = {RULE_VARIANT(0x82, 0x01)};
static const hubus_module_t switch_module = {
    HUBUS_MODULE_INIT,
    .name = "switch",
    .rules = switch_rules,
    .n_rules = HUBUS_MODULE_LEN(switch_rules),
    .max_command = HUBUS_MAX_PAYLOAD,
    .has_state = true,
    .has_events = true,
    .has_status_capture = true,
};

static const hubus_module_rule_t switch_update_rules[] = {RULE_VARIANT(0x82, 0x02)};
static const hubus_module_t switch_update = {
    HUBUS_MODULE_INIT,
    .name = "switch-update",
    .rules = switch_update_rules,
    .n_rules = HUBUS_MODULE_LEN(switch_update_rules),
    .max_command = HUBUS_MAX_PAYLOAD,
    .has_state = true,
    .has_events = true,
    .has_status_capture = true,
    .has_update = true,
};

static const hubus_module_rule_t temperature_rules[] = {RULE_VARIANT(0x83, 0x01)};
static const hubus_module_t temperature = {
    HUBUS_MODULE_INIT,
    .name = "temperature",
    .rules = temperature_rules,
    .n_rules = HUBUS_MODULE_LEN(temperature_rules),
    .max_command = HUBUS_MAX_PAYLOAD,
    .has_events = true,
    .has_status_capture = true,
};

static const hubus_module_rule_t temperature_passive_rules[] = {RULE_VARIANT(0x83, 0x02)};
static const hubus_module_t temperature_passive = {
    HUBUS_MODULE_INIT,
    .name = "temperature-passive",
    .rules = temperature_passive_rules,
    .n_rules = HUBUS_MODULE_LEN(temperature_passive_rules),
    .max_command = HUBUS_MAX_PAYLOAD,
    .has_state = true,
};

/* Only SHOW makes the strip blind: 180 bytes of 12.5 us, the reset time of the strip, and the wait for the slave to be
 * free (docs/example-ledstrip.md). */
static const hubus_module_rule_t ledstrip_rules[] = {RULE_VARIANT(0x84, 0x01)};
static const hubus_module_blind_t ledstrip_blind[] = {{.opcode = OP_LED_SHOW, .ms = 4}};
static const hubus_module_t ledstrip = {
    HUBUS_MODULE_INIT,
    .name = "ledstrip",
    .rules = ledstrip_rules,
    .n_rules = HUBUS_MODULE_LEN(ledstrip_rules),
    .max_command = HUBUS_MAX_PAYLOAD,
    .has_state = true,
    .has_events = true,
    .has_status_capture = true,
    .blind_ops = ledstrip_blind,
    .n_blind_ops = HUBUS_MODULE_LEN(ledstrip_blind),
};

/* One module for every updater; what differs per device is in the updater information. */
static const hubus_module_rule_t updater_rules[] = {{.pattern = {HUBUS_HWID_TYPE_UPDATER}, .mask = {0xFF}, .len = 1}};
static const hubus_module_t updater = {
    HUBUS_MODULE_INIT,
    .name = "updater",
    .rules = updater_rules,
    .n_rules = HUBUS_MODULE_LEN(updater_rules),
    .max_command = HUBUS_MAX_PAYLOAD,
    .has_state = true,
};

static const hubus_module_t *const modules[] = {
    &relay, &relay_simple, &switch_module, &switch_update, &temperature, &temperature_passive, &ledstrip, &updater,
};

const hubus_module_t *const *hubusd_builtin_modules(size_t *count)
{
    *count = sizeof modules / sizeof modules[0];
    return modules;
}
