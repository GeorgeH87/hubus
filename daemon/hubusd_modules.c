#include "hubusd_modules.h"

#include <stdio.h>

#define RULE_VARIANT(type, variant) {.pattern = {(type), (variant)}, .mask = {0xFF, 0xFF}, .len = 2}

#define OP_RELAY_SET 0x10u

/* The relay answers with its state; it is switched by OP_RELAY_SET 1 or 0. */
static const hubus_entity_t relay_entities[] = {{
    .key = "relay",
    .name = "Relay",
    .component = HUBUS_COMPONENT_SWITCH,
    .decode = {.offset = 0, .len = 1, .scale = 1.0},
    .on = {.used = true, .opcode = OP_RELAY_SET, .n_args = 1, .args = {1}},
    .off = {.used = true, .opcode = OP_RELAY_SET, .n_args = 1, .args = {0}},
}};

/* Without a state and without events the master only knows what it commanded. */
static const hubus_entity_t relay_simple_entities[] = {{
    .key = "relay",
    .name = "Relay",
    .component = HUBUS_COMPONENT_SWITCH,
    .on = {.used = true, .opcode = OP_RELAY_SET, .n_args = 1, .args = {1}},
    .off = {.used = true, .opcode = OP_RELAY_SET, .n_args = 1, .args = {0}},
}};

static const hubus_entity_t button_entities[] = {{
    .key = "button",
    .name = "Button",
    .component = HUBUS_COMPONENT_BINARY_SENSOR,
    .decode = {.offset = 0, .len = 1, .scale = 1.0},
}};

/* The ATtiny85 internal sensor: about 1 LSB per kelvin, 273 LSB at 0 degC. Needs a calibration per device. */
static const hubus_entity_t temperature_entities[] = {{
    .key = "temperature",
    .name = "Temperature",
    .component = HUBUS_COMPONENT_SENSOR,
    .device_class = HUBUS_DEVICE_CLASS_TEMPERATURE,
    .unit = HUBUS_UNIT_CELSIUS,
    .decode = {.offset = 0, .len = 2, .big_endian = true, .scale = 1.0, .add = -273.0},
}};

#define ENTITIES(array) .entities = (array), .n_entities = HUBUS_MODULE_LEN(array)

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
    ENTITIES(relay_entities),
};

static const hubus_module_rule_t relay_simple_rules[] = {RULE_VARIANT(0x81, 0x02)};
static const hubus_module_t relay_simple = {
    HUBUS_MODULE_INIT,
    .name = "relay-simple",
    .rules = relay_simple_rules,
    .n_rules = HUBUS_MODULE_LEN(relay_simple_rules),
    .max_command = HUBUS_MAX_PAYLOAD,
    ENTITIES(relay_simple_entities),
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
    ENTITIES(button_entities),
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
    ENTITIES(button_entities),
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
    ENTITIES(temperature_entities),
};

static const hubus_module_rule_t temperature_passive_rules[] = {RULE_VARIANT(0x83, 0x02)};
static const hubus_module_t temperature_passive = {
    HUBUS_MODULE_INIT,
    .name = "temperature-passive",
    .rules = temperature_passive_rules,
    .n_rules = HUBUS_MODULE_LEN(temperature_passive_rules),
    .max_command = HUBUS_MAX_PAYLOAD,
    .has_state = true,
    ENTITIES(temperature_entities),
};

/* The DHT11 is only read: the state holds the humidity in percent, the temperature in degrees Celsius, and a fault byte
 * (1: no good reading yet, or the last readings failed; the values are then old). */
static const hubus_entity_t dht11_entities[] = {
    {
        .key = "temperature",
        .name = "Temperature",
        .component = HUBUS_COMPONENT_SENSOR,
        .device_class = HUBUS_DEVICE_CLASS_TEMPERATURE,
        .unit = HUBUS_UNIT_CELSIUS,
        .decode = {.offset = 1, .len = 1, .scale = 1.0},
    },
    {
        .key = "humidity",
        .name = "Humidity",
        .component = HUBUS_COMPONENT_SENSOR,
        .device_class = HUBUS_DEVICE_CLASS_HUMIDITY,
        .unit = HUBUS_UNIT_PERCENT,
        .decode = {.offset = 0, .len = 1, .scale = 1.0},
    },
    {
        .key = "fault",
        .name = "Sensor fault",
        .component = HUBUS_COMPONENT_BINARY_SENSOR,
        .decode = {.offset = 2, .len = 1, .scale = 1.0},
    },
};

static const hubus_module_rule_t dht11_rules[] = {RULE_VARIANT(0x86, 0x01)};
static const hubus_module_t dht11 = {
    HUBUS_MODULE_INIT,
    .name = "dht11",
    .rules = dht11_rules,
    .n_rules = HUBUS_MODULE_LEN(dht11_rules),
    .max_command = HUBUS_MAX_PAYLOAD,
    .has_state = true,
    .has_update = true,
    ENTITIES(dht11_entities),
};

/* The HC-SR501 has no state and no commands: one byte per event, 1 for motion and 0 for none. */
static const hubus_entity_t motion_entities[] = {{
    .key = "motion",
    .name = "Motion",
    .component = HUBUS_COMPONENT_BINARY_SENSOR,
    .device_class = HUBUS_DEVICE_CLASS_MOTION,
    .decode = {.offset = 0, .len = 1, .scale = 1.0},
}};

static const hubus_module_rule_t motion_rules[] = {RULE_VARIANT(0x88, 0x01)};
static const hubus_module_t motion = {
    HUBUS_MODULE_INIT,
    .name = "motion",
    .rules = motion_rules,
    .n_rules = HUBUS_MODULE_LEN(motion_rules),
    .max_command = HUBUS_MAX_PAYLOAD,
    .has_events = true,
    .has_update = true,
    ENTITIES(motion_entities),
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
    &relay, &relay_simple, &switch_module, &switch_update, &temperature, &temperature_passive, &hubusd_ledstrip_module, &dht11, &motion, &hubusd_distance_module, &updater,
};

const hubus_module_t *const *hubusd_builtin_modules(size_t *count)
{
    *count = sizeof modules / sizeof modules[0];
    return modules;
}

size_t hubusd_load_modules(const hubusd_config_t *cfg, hubus_module_set_t *set, hubus_load_report_fn report, void *ctx)
{
    size_t n, i;
    const hubus_module_t *const *builtin = hubusd_builtin_modules(&n);

    for (i = 0; i < n; i++) {
        (void)hubus_module_set_add(set, builtin[i]); /* validated by the tests of hubusd */
    }
    return cfg->modules.dir[0] ? hubus_module_set_load_dir(set, cfg->modules.dir, report, ctx) : 0u;
}

/* Writes what became of a plugin file to stderr, with the syslog priorities journald understands. */
void hubusd_report_module(void *ctx, const char *path, const hubus_load_result_t *r)
{
    (void)ctx;
    if (r->status == HUBUS_LOAD_OK) {
        fprintf(stderr, "<6>hubusd: module %s loaded from %s\n", r->name, path);
        return;
    }
    fprintf(stderr, "<3>hubusd: %s: %s", path, hubus_load_status_str(r->status));
    if (r->status == HUBUS_LOAD_INVALID) {
        fprintf(stderr, " (%s)", hubus_module_error_str(r->invalid));
    } else if (r->module) {
        fprintf(stderr, " (%s)", r->module->name);
    } else if (r->detail[0]) {
        fprintf(stderr, " (%s)", r->detail);
    }
    fprintf(stderr, "\n");
}

const char *hubusd_component_name(hubus_component_t component)
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

const char *hubusd_device_class_name(hubus_device_class_t device_class)
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

const char *hubusd_unit_text(hubus_unit_t unit)
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
