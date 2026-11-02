/*
 * Master modules (docs/spec/06-operation.md section 6.10): the forms of the hardware identifier, the validation of a
 * module, the selection by match rules, and the blind times per opcode.
 */
#include <string.h>

#include "hubus_module.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- two modules as a module author writes them -------------------------------------------------------------------- */

static const hubus_module_rule_t temperature_rules[] = {
    {.pattern = {0x83}, .mask = {0xFF}, .len = 1},
};

static const hubus_entity_t temperature_entities[] = {
    {
        .key = "temperature",
        .name = "Temperature",
        .component = HUBUS_COMPONENT_SENSOR,
        .device_class = HUBUS_DEVICE_CLASS_TEMPERATURE,
        .unit = HUBUS_UNIT_CELSIUS,
        .decode = {.offset = 0, .len = 2, .big_endian = true, .scale = 1.0, .add = -273.0},
    },
};

static const hubus_module_t temperature_module = {
    HUBUS_MODULE_INIT,
    .name = "temperature",
    .rules = temperature_rules,
    .n_rules = HUBUS_MODULE_LEN(temperature_rules),
    .max_command = 1,
    .has_state = true,
    .has_events = true,
    .entities = temperature_entities,
    .n_entities = HUBUS_MODULE_LEN(temperature_entities),
};

#define OP_FILL 0x41u
#define OP_SHOW 0x42u

static const hubus_module_rule_t ledstrip_rules[] = {
    {.pattern = {0x84, 0x01}, .mask = {0xFF, 0xFF}, .len = 2},
};

static const hubus_module_blind_t ledstrip_blind[] = {
    {.opcode = OP_FILL, .ms = 0},
};

static const hubus_entity_t ledstrip_entities[] = {
    {.key = "color", .name = "LED strip", .component = HUBUS_COMPONENT_LIGHT},
    {.key = "button", .name = "Button", .component = HUBUS_COMPONENT_BINARY_SENSOR, .decode = {.len = 1, .scale = 1.0}},
};

static hubus_module_status_t ledstrip_on_set(hubus_module_instance_t *self, const hubus_module_host_t *host,
                                             const hubus_entity_t *entity, const hubus_value_t *value)
{
    uint8_t fill[7] = {0, 0, 0, 180, 0, 0, 0};
    hubus_module_status_t st;

    (void)self;
    if (strcmp(entity->key, "color") != 0 || value->type != HUBUS_VALUE_RGB) {
        return HUBUS_MODULE_REJECTED;
    }
    fill[4] = value->as.rgb.g; /* WS2812B: GRB */
    fill[5] = value->as.rgb.r;
    fill[6] = value->as.rgb.b;
    st = host->send(host, OP_FILL, fill, sizeof fill);
    return st == HUBUS_MODULE_OK ? host->send(host, OP_SHOW, NULL, 0) : st;
}

static const hubus_module_t ledstrip_module = {
    HUBUS_MODULE_INIT,
    .name = "ledstrip",
    .rules = ledstrip_rules,
    .n_rules = HUBUS_MODULE_LEN(ledstrip_rules),
    .max_command = 31,
    .has_state = true,
    .has_events = true,
    .blind_default_ms = 4,
    .blind_ops = ledstrip_blind,
    .n_blind_ops = HUBUS_MODULE_LEN(ledstrip_blind),
    .entities = ledstrip_entities,
    .n_entities = HUBUS_MODULE_LEN(ledstrip_entities),
    .on_set = ledstrip_on_set,
};

/* A module with one rule, for the selection tests. */
static hubus_module_t rule_module(const hubus_module_rule_t *rule)
{
    hubus_module_t m;

    memset(&m, 0, sizeof m);
    m.api_version = HUBUS_MODULE_API_VERSION;
    m.struct_size = sizeof m;
    m.name = "rule";
    m.rules = rule;
    m.n_rules = 1;
    m.max_command = 1;
    return m;
}

/* ---- forms of the hardware identifier -------------------------------------------------------------------------------- */

static void test_hwid_forms(void)
{
    uint8_t hwid[HUBUS_HWID_MAX_LEN + 1u];

    memset(hwid, 0, sizeof hwid);
    TEST_ASSERT_EQUAL(HUBUS_HWID_INVALID, hubus_hwid_form(NULL, 0)); /* no HWID at all */
    TEST_ASSERT_EQUAL(HUBUS_HWID_INVALID, hubus_hwid_form(hwid, 1)); /* type 0x00 */

    hwid[0] = 0x01;
    TEST_ASSERT_EQUAL(HUBUS_HWID_SHORT, hubus_hwid_form(hwid, 1));
    hwid[0] = 0x7F;
    TEST_ASSERT_EQUAL(HUBUS_HWID_SHORT, hubus_hwid_form(hwid, 1));
    TEST_ASSERT_EQUAL(HUBUS_HWID_INVALID, hubus_hwid_form(hwid, 2)); /* short form longer than one byte */

    hwid[0] = 0x80;
    TEST_ASSERT_EQUAL(HUBUS_HWID_INVALID, hubus_hwid_form(hwid, 3)); /* full form shorter than four bytes */
    TEST_ASSERT_EQUAL(HUBUS_HWID_FULL, hubus_hwid_form(hwid, 4));
    hwid[0] = 0xFE;
    TEST_ASSERT_EQUAL(HUBUS_HWID_FULL, hubus_hwid_form(hwid, HUBUS_HWID_MAX_LEN)); /* 26 bytes of serial number */
    TEST_ASSERT_EQUAL(HUBUS_HWID_INVALID, hubus_hwid_form(hwid, HUBUS_HWID_MAX_LEN + 1u));

    hwid[0] = 0xFF;
    TEST_ASSERT_EQUAL(HUBUS_HWID_UPDATER, hubus_hwid_form(hwid, HUBUS_HWID_UPDATER_LEN));
    TEST_ASSERT_EQUAL(HUBUS_HWID_INVALID, hubus_hwid_form(hwid, HUBUS_HWID_UPDATER_LEN - 1u));
    TEST_ASSERT_EQUAL(HUBUS_HWID_INVALID, hubus_hwid_form(hwid, 1));
}

/* ---- selection ------------------------------------------------------------------------------------------------------- */

static const hubus_module_rule_t rule_short = {.pattern = {0x05}, .mask = {0xFF}, .len = 1};
static const hubus_module_rule_t rule_type = {.pattern = {0x81}, .mask = {0xFF}, .len = 1};
static const hubus_module_rule_t rule_variant = {.pattern = {0x81, 0x02}, .mask = {0xFF, 0xFF}, .len = 2};
static const hubus_module_rule_t rule_version = {.pattern = {0x81, 0x02, 0x07}, .mask = {0xFF, 0xFF, 0xFF}, .len = 3};
static const hubus_module_rule_t rule_revision = {.pattern = {0x81, 0x02, 0x00, 0x01},
                                                  .mask = {0xFF, 0xFF, 0x00, 0xFF},
                                                  .len = 4};
static const hubus_module_rule_t rule_updater = {.pattern = {0xFF}, .mask = {0xFF}, .len = 1};

static const hubus_module_t *select_one(const hubus_module_t *const *modules, size_t n, const uint8_t *hwid, uint8_t len)
{
    const hubus_module_t *out = &temperature_module; /* must be overwritten */

    TEST_ASSERT_EQUAL(HUBUS_MODULE_FOUND, hubus_module_select(modules, n, hwid, len, &out));
    return out;
}

static void test_the_most_specific_rule_wins_in_any_order(void)
{
    const uint8_t v7_r1[] = {0x81, 0x02, 0x07, 0x01};
    const uint8_t v8_r1[] = {0x81, 0x02, 0x08, 0x01};
    const uint8_t v8_r2[] = {0x81, 0x02, 0x08, 0x02};
    const uint8_t other_variant[] = {0x81, 0x03, 0x07, 0x01};
    hubus_module_t type = rule_module(&rule_type);
    hubus_module_t variant = rule_module(&rule_variant);
    hubus_module_t version = rule_module(&rule_version);
    hubus_module_t revision = rule_module(&rule_revision);
    const hubus_module_t *const up[] = {&type, &variant, &version};
    const hubus_module_t *const down[] = {&version, &variant, &type};
    const hubus_module_t *const by_revision[] = {&type, &variant, &revision};

    TEST_ASSERT_EQUAL_PTR(&version, select_one(up, 3, v7_r1, sizeof v7_r1));
    TEST_ASSERT_EQUAL_PTR(&version, select_one(down, 3, v7_r1, sizeof v7_r1));
    TEST_ASSERT_EQUAL_PTR(&variant, select_one(up, 3, v8_r1, sizeof v8_r1));
    TEST_ASSERT_EQUAL_PTR(&variant, select_one(down, 3, v8_r1, sizeof v8_r1));
    TEST_ASSERT_EQUAL_PTR(&type, select_one(up, 3, other_variant, sizeof other_variant));
    TEST_ASSERT_EQUAL_PTR(&type, select_one(down, 3, other_variant, sizeof other_variant));

    /* a mask rule skips the version, and only that */
    TEST_ASSERT_EQUAL_PTR(&revision, select_one(by_revision, 3, v8_r1, sizeof v8_r1));
    TEST_ASSERT_EQUAL_PTR(&variant, select_one(by_revision, 3, v8_r2, sizeof v8_r2));
}

static void test_the_serial_number_is_ignored(void)
{
    const uint8_t with_serial[] = {0x81, 0x02, 0x07, 0x01, 0x00, 0x00, 0x2A, 0x17};
    hubus_module_t variant = rule_module(&rule_variant);
    const hubus_module_t *const modules[] = {&variant};

    TEST_ASSERT_EQUAL_PTR(&variant, select_one(modules, 1, with_serial, sizeof with_serial));
}

static void test_short_form_and_updater(void)
{
    const uint8_t short_form[] = {0x05};
    const uint8_t other_short[] = {0x06};
    uint8_t updater[HUBUS_HWID_UPDATER_LEN];
    const hubus_module_t *out;
    hubus_module_t small = rule_module(&rule_short);
    hubus_module_t upd = rule_module(&rule_updater);
    const hubus_module_t *const modules[] = {&small, &upd};

    memset(updater, 0, sizeof updater);
    updater[0] = 0xFF;
    TEST_ASSERT_EQUAL_PTR(&small, select_one(modules, 2, short_form, sizeof short_form));
    TEST_ASSERT_EQUAL_PTR(&upd, select_one(modules, 2, updater, sizeof updater));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_UNKNOWN, hubus_module_select(modules, 2, other_short, sizeof other_short, &out));
    TEST_ASSERT_NULL(out);
}

static void test_unknown_and_invalid_hwids_select_nothing(void)
{
    const uint8_t unknown[] = {0x82, 0x00, 0x00, 0x00};
    const uint8_t too_short[] = {0x81, 0x02};
    const uint8_t short_too_long[] = {0x05, 0x00};
    const hubus_module_t *out = &temperature_module;
    hubus_module_t type = rule_module(&rule_type);
    hubus_module_t small = rule_module(&rule_short);
    const hubus_module_t *const modules[] = {&type, &small};

    TEST_ASSERT_EQUAL(HUBUS_MODULE_UNKNOWN, hubus_module_select(modules, 2, unknown, sizeof unknown, &out));
    TEST_ASSERT_NULL(out);
    out = &temperature_module;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_INVALID, hubus_module_select(modules, 2, too_short, sizeof too_short, &out));
    TEST_ASSERT_NULL(out);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_INVALID,
                      hubus_module_select(modules, 2, short_too_long, sizeof short_too_long, &out));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_UNKNOWN, hubus_module_select(NULL, 0, unknown, sizeof unknown, &out));
}

static void test_equally_specific_rules_of_different_modules_conflict(void)
{
    const uint8_t hwid[] = {0x81, 0x02, 0x07, 0x01};
    const hubus_module_t *out = &temperature_module;
    hubus_module_t a = rule_module(&rule_variant);
    hubus_module_t b = rule_module(&rule_variant);
    hubus_module_t version = rule_module(&rule_version);
    hubus_module_t revision = rule_module(&rule_revision);
    const hubus_module_t *const same[] = {&a, &b};
    const hubus_module_t *const overridden_later[] = {&a, &b, &version};
    const hubus_module_t *const overridden_first[] = {&version, &a, &b};
    const hubus_module_t *const same_bits[] = {&version, &revision}; /* 24 mask bits each */

    TEST_ASSERT_EQUAL(HUBUS_MODULE_CONFLICT, hubus_module_select(same, 2, hwid, sizeof hwid, &out));
    TEST_ASSERT_NULL(out);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_CONFLICT, hubus_module_select(same_bits, 2, hwid, sizeof hwid, &out));
    /* a conflict among less specific rules does not matter */
    TEST_ASSERT_EQUAL_PTR(&version, select_one(overridden_later, 3, hwid, sizeof hwid));
    TEST_ASSERT_EQUAL_PTR(&version, select_one(overridden_first, 3, hwid, sizeof hwid));
}

static void test_two_equal_rules_of_one_module_are_no_conflict(void)
{
    const uint8_t hwid[] = {0x81, 0x02, 0x07, 0x01};
    const hubus_module_rule_t rules[] = {
        {.pattern = {0x81, 0x02}, .mask = {0xFF, 0xFF}, .len = 2},
        {.pattern = {0x81, 0x02, 0x00, 0x00}, .mask = {0xFF, 0xFF, 0x00, 0x00}, .len = 4},
    };
    hubus_module_t m = rule_module(rules);
    const hubus_module_t *const modules[] = {&m};

    m.n_rules = 2;
    TEST_ASSERT_EQUAL_PTR(&m, select_one(modules, 1, hwid, sizeof hwid));
}

/* ---- validation ------------------------------------------------------------------------------------------------------ */

static void test_the_example_modules_are_valid(void)
{
    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, hubus_module_validate(&temperature_module));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, hubus_module_validate(&ledstrip_module));
}

static void test_the_interface_version_is_checked(void)
{
    hubus_module_t m = temperature_module;

    m.api_version = HUBUS_MODULE_API_VERSION + 1u;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_API, hubus_module_validate(&m));
    m = temperature_module;
    m.struct_size = sizeof m - 1u;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_API, hubus_module_validate(&m));
    m.struct_size = sizeof m + 8u; /* a newer module: this master ignores what it does not know */
    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, hubus_module_validate(&m));
}

static hubus_module_error_t validate_rule(hubus_module_rule_t r)
{
    hubus_module_t m = rule_module(&r);

    return hubus_module_validate(&m);
}

static void test_rules_are_checked(void)
{
    hubus_module_t m = temperature_module;

    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, validate_rule(rule_short));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, validate_rule(rule_revision));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, validate_rule(rule_updater));

    m.n_rules = 0;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_RULE, hubus_module_validate(&m));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_RULE,
                      validate_rule((hubus_module_rule_t){.pattern = {0x81}, .mask = {0xFF}, .len = 0}));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_RULE,
                      validate_rule((hubus_module_rule_t){.pattern = {0x81}, .mask = {0xFF}, .len = 5}));
    /* type 0x00 is invalid, and a rule names a whole type */
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_RULE,
                      validate_rule((hubus_module_rule_t){.pattern = {0x00}, .mask = {0xFF}, .len = 1}));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_RULE,
                      validate_rule((hubus_module_rule_t){.pattern = {0x80}, .mask = {0xF0}, .len = 1}));
    /* more than the type of a short form or the updater could never match */
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_RULE,
                      validate_rule((hubus_module_rule_t){.pattern = {0x05, 0x01}, .mask = {0xFF, 0xFF}, .len = 2}));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_RULE,
                      validate_rule((hubus_module_rule_t){.pattern = {0xFF, 0x01}, .mask = {0xFF, 0xFF}, .len = 2}));
    /* a pattern bit outside the mask has no meaning */
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_RULE,
                      validate_rule((hubus_module_rule_t){.pattern = {0x81, 0x02}, .mask = {0xFF, 0x00}, .len = 2}));
}

static void test_properties_and_blind_times_are_checked(void)
{
    const hubus_module_blind_t twice[] = {{.opcode = 0x41, .ms = 0}, {.opcode = 0x41, .ms = 2}};
    hubus_module_t m = temperature_module;

    m.name = NULL;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_NAME, hubus_module_validate(&m));
    m = temperature_module;
    m.max_command = 0;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_PROPERTIES, hubus_module_validate(&m));
    m.max_command = 32;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_PROPERTIES, hubus_module_validate(&m));
    m = temperature_module;
    m.has_events = false;
    m.has_status_capture = true;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_PROPERTIES, hubus_module_validate(&m));
    m = temperature_module;
    m.blind_ops = twice;
    m.n_blind_ops = HUBUS_MODULE_LEN(twice);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_BLIND, hubus_module_validate(&m));
    m.blind_ops = NULL;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_BLIND, hubus_module_validate(&m));
}

static hubus_module_error_t validate_entity(hubus_entity_t e, bool with_on_set)
{
    hubus_module_t m = temperature_module;

    m.entities = &e;
    m.n_entities = 1;
    m.on_set = with_on_set ? ledstrip_on_set : NULL;
    return hubus_module_validate(&m);
}

static void test_entities_are_checked(void)
{
    const hubus_entity_t sensor = temperature_entities[0];
    const hubus_entity_t twice[] = {sensor, sensor};
    hubus_entity_t e;
    hubus_module_t m = temperature_module;

    e = sensor;
    e.key = NULL;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false));
    e = sensor;
    e.name = NULL;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false));
    e = sensor;
    e.component = HUBUS_COMPONENT_COUNT; /* from a plugin the compiler cannot check */
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false));
    e = sensor;
    e.device_class = HUBUS_DEVICE_CLASS_COUNT;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false));
    e = sensor;
    e.unit = HUBUS_UNIT_COUNT;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false));

    /* a sensor needs a value that fits into a frame */
    e = sensor;
    e.decode.len = 0;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false));
    e = sensor;
    e.decode.len = 3;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false));
    e = sensor;
    e.decode.offset = 30;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false));
    e.decode.offset = 29;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, validate_entity(e, false));
    e = sensor;
    e.decode.scale = 0.0; /* every value would read 0 */
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false));

    /* a switch needs both commands or a function; a light always a function */
    e = (hubus_entity_t){.key = "relay", .name = "Relay", .component = HUBUS_COMPONENT_SWITCH};
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, validate_entity(e, true));
    e.on = (hubus_module_command_t){.used = true, .opcode = 0x10};
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false));
    e.off = (hubus_module_command_t){.used = true, .opcode = 0x10};
    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, validate_entity(e, false));
    e.on.n_args = 1; /* opcode and argument do not fit the 1 byte the module accepts */
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false));
    e = (hubus_entity_t){.key = "light", .name = "Light", .component = HUBUS_COMPONENT_LIGHT};
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, validate_entity(e, true));

    /* keys become MQTT topics */
    m.entities = twice;
    m.n_entities = HUBUS_MODULE_LEN(twice);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, hubus_module_validate(&m));
    m.n_entities = 1;
    m.entities = NULL;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, hubus_module_validate(&m));
}

static hubus_module_instance_t *fake_create(const hubus_module_host_t *host)
{
    (void)host;
    return NULL;
}

static void test_create_and_destroy_come_together(void)
{
    hubus_module_t m = temperature_module;

    m.create = fake_create;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_FUNCTIONS, hubus_module_validate(&m));
}

/* ---- blind times ------------------------------------------------------------------------------------------------------ */

static void test_blind_time_per_opcode_with_the_general_value_as_fallback(void)
{
    TEST_ASSERT_EQUAL_UINT16(0, hubus_module_blind_ms(&ledstrip_module, OP_FILL));
    TEST_ASSERT_EQUAL_UINT16(4, hubus_module_blind_ms(&ledstrip_module, OP_SHOW));
    TEST_ASSERT_EQUAL_UINT16(4, hubus_module_blind_ms(&ledstrip_module, 0x00)); /* also STATUS_CAPTURE */
    TEST_ASSERT_EQUAL_UINT16(0, hubus_module_blind_ms(&temperature_module, OP_SHOW)); /* never blind */
}

static void test_a_blind_time_only_for_one_opcode_counts_as_a_blind_time(void)
{
    const hubus_module_blind_t none[] = {{.opcode = 0x41, .ms = 0}};
    const hubus_module_blind_t one[] = {{.opcode = 0x42, .ms = 4}};
    hubus_module_t m = temperature_module;

    TEST_ASSERT_FALSE(hubus_module_has_blind_time(&m));
    m.blind_ops = none;
    m.n_blind_ops = 1;
    TEST_ASSERT_FALSE(hubus_module_has_blind_time(&m));
    m.blind_ops = one;
    TEST_ASSERT_TRUE(hubus_module_has_blind_time(&m));
    TEST_ASSERT_EQUAL_UINT16(0, hubus_module_blind_ms(&m, 0x41));
    TEST_ASSERT_EQUAL_UINT16(4, hubus_module_blind_ms(&m, 0x42));
    TEST_ASSERT_TRUE(hubus_module_has_blind_time(&ledstrip_module));
}

/* ---- the values of an entity ------------------------------------------------------------------------------------------- */

static void test_entities_are_found_by_key(void)
{
    TEST_ASSERT_EQUAL_INT(0, hubus_module_find_entity(&ledstrip_module, "color"));
    TEST_ASSERT_EQUAL_INT(1, hubus_module_find_entity(&ledstrip_module, "button"));
    TEST_ASSERT_EQUAL_INT(-1, hubus_module_find_entity(&ledstrip_module, "Button")); /* keys are case sensitive */
    TEST_ASSERT_EQUAL_INT(-1, hubus_module_find_entity(&ledstrip_module, ""));
}

static void test_a_sensor_value_is_decoded_with_scale_offset_and_decimals(void)
{
    const uint8_t temperature[] = {0x01, 0x2A}; /* 298 */
    hubus_entity_t e = temperature_entities[0];
    char out[16];

    TEST_ASSERT_EQUAL_UINT(2, hubus_entity_decode(&e, temperature, 2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("25", out);
    e.decode.big_endian = false; /* 0x2A01 */
    TEST_ASSERT_EQUAL_UINT(5, hubus_entity_decode(&e, temperature, 2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("10480", out);
    e.decode.big_endian = true;
    e.decode.scale = 0.1;
    e.decode.add = 900.0;
    e.decode.decimals = 1;
    TEST_ASSERT_EQUAL_UINT(5, hubus_entity_decode(&e, temperature, 2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("929.8", out);
}

static void test_the_signedness_and_the_offset_are_respected(void)
{
    const uint8_t data[] = {0x00, 0xFF, 0xFE};
    hubus_entity_t e = temperature_entities[0];
    char out[16];

    e.decode = (hubus_module_decode_t){.offset = 1, .len = 1, .scale = 1.0};
    TEST_ASSERT_TRUE(hubus_entity_decode(&e, data, 3, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("255", out);
    e.decode.is_signed = true;
    TEST_ASSERT_TRUE(hubus_entity_decode(&e, data, 3, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("-1", out);
    e.decode = (hubus_module_decode_t){.offset = 1, .len = 2, .big_endian = true, .is_signed = true, .scale = 1.0};
    TEST_ASSERT_TRUE(hubus_entity_decode(&e, data, 3, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("-2", out); /* 0xFFFE */
}

static void test_a_switch_or_a_binary_sensor_reports_on_or_off(void)
{
    const uint8_t on = 3, off = 0;
    hubus_entity_t e = ledstrip_entities[1];
    char out[8];

    TEST_ASSERT_TRUE(hubus_entity_decode(&e, &on, 1, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("ON", out);
    TEST_ASSERT_TRUE(hubus_entity_decode(&e, &off, 1, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("OFF", out);
    e.component = HUBUS_COMPONENT_SWITCH;
    TEST_ASSERT_TRUE(hubus_entity_decode(&e, &on, 1, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("ON", out);
}

static void test_nothing_is_decoded_that_the_frame_or_the_buffer_cannot_hold(void)
{
    const uint8_t data[] = {0x01, 0x2A};
    char out[8];

    TEST_ASSERT_EQUAL_UINT(0, hubus_entity_decode(&temperature_entities[0], data, 1, out, sizeof out)); /* too short */
    TEST_ASSERT_EQUAL_UINT(0, hubus_entity_decode(&temperature_entities[0], data, 0, out, sizeof out));
    TEST_ASSERT_EQUAL_UINT(0, hubus_entity_decode(&temperature_entities[0], data, 2, out, 2)); /* "25" needs 3 */
    TEST_ASSERT_EQUAL_UINT(0, hubus_entity_decode(&ledstrip_entities[0], data, 2, out, sizeof out)); /* no value */
}

static void test_keys_are_safe_for_a_topic_and_the_number_of_entities_is_limited(void)
{
    static const char *const good[] = {"a", "relay", "low-byte", "Channel_2", "0123456789012345678901"};
    static const char *const bad[] = {"", "a/b", "a b", "a+", "a#", "\xC3\xA4", "01234567890123456789012345"};
    hubus_entity_t e = temperature_entities[0];
    hubus_entity_t many[HUBUS_MODULE_ENTITIES_MAX + 1u];
    char keys[HUBUS_MODULE_ENTITIES_MAX + 1u][8];
    hubus_module_t m = temperature_module;
    size_t i;

    for (i = 0; i < sizeof good / sizeof good[0]; i++) {
        e.key = good[i];
        TEST_ASSERT_EQUAL_MESSAGE(HUBUS_MODULE_VALID, validate_entity(e, false), good[i]);
    }
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        e.key = bad[i];
        TEST_ASSERT_EQUAL_MESSAGE(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false), bad[i]);
    }
    for (i = 0; i <= HUBUS_MODULE_ENTITIES_MAX; i++) {
        many[i] = temperature_entities[0];
        snprintf(keys[i], sizeof keys[i], "e%u", (unsigned)i);
        many[i].key = keys[i];
    }
    m.entities = many;
    m.n_entities = HUBUS_MODULE_ENTITIES_MAX;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, hubus_module_validate(&m));
    m.n_entities = HUBUS_MODULE_ENTITIES_MAX + 1u;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, hubus_module_validate(&m));
    e = temperature_entities[0];
    e.decode.decimals = 7; /* a number that does not fit a short buffer */
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_ENTITY, validate_entity(e, false));
}

/* ---- selection from a SLAVE_INFO payload ------------------------------------------------------------------------------ */

static void test_selection_from_slave_info(void)
{
    const uint8_t good[] = {0x03, 0x83, 0x01, 0x07, 0x00};
    const uint8_t later_version[] = {0x13, 0x83, 0x01, 0x07, 0x00};
    const uint8_t no_hwid[] = {0x03};
    const hubus_module_t *const modules[] = {&temperature_module};
    const hubus_module_t *out;

    TEST_ASSERT_EQUAL(HUBUS_MODULE_FOUND, hubus_module_select_info(modules, 1, good, sizeof good, &out));
    TEST_ASSERT_EQUAL_PTR(&temperature_module, out);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_INVALID, hubus_module_select_info(modules, 1, later_version, sizeof later_version, &out));
    TEST_ASSERT_NULL(out);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_INVALID, hubus_module_select_info(modules, 1, no_hwid, sizeof no_hwid, &out));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_INVALID, hubus_module_select_info(modules, 1, good, 0, &out));
}

/* ---- functions of a module -------------------------------------------------------------------------------------------- */

typedef struct {
    unsigned n;
    uint8_t opcode[2];
    uint8_t len[2];
    uint8_t first_color;
    hubus_module_status_t result;
} fake_bus_t;

static hubus_module_status_t fake_send(const hubus_module_host_t *host, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    fake_bus_t *bus = host->ctx;

    TEST_ASSERT_TRUE(bus->n < 2u);
    bus->opcode[bus->n] = opcode;
    bus->len[bus->n] = len;
    if (bus->n == 0u) {
        bus->first_color = args[4];
    }
    bus->n++;
    return bus->result;
}

static void test_a_function_sends_through_the_host_with_typed_values(void)
{
    fake_bus_t bus = {.result = HUBUS_MODULE_OK};
    const hubus_module_host_t host = {.ctx = &bus, .mcid = 9, .send = fake_send};
    hubus_value_t red = {.type = HUBUS_VALUE_RGB};
    const hubus_value_t on = {.type = HUBUS_VALUE_BOOL, .as.on = true};

    red.as.rgb.r = 255;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_OK, ledstrip_module.on_set(NULL, &host, &ledstrip_entities[0], &red));
    TEST_ASSERT_EQUAL_UINT(2, bus.n);
    TEST_ASSERT_EQUAL_HEX8(OP_FILL, bus.opcode[0]);
    TEST_ASSERT_EQUAL_UINT8(7, bus.len[0]);
    TEST_ASSERT_EQUAL_UINT8(0, bus.first_color); /* green goes first */
    TEST_ASSERT_EQUAL_HEX8(OP_SHOW, bus.opcode[1]);

    bus.n = 0;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_REJECTED, ledstrip_module.on_set(NULL, &host, &ledstrip_entities[0], &on));
    TEST_ASSERT_EQUAL_UINT(0, bus.n);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hwid_forms);
    RUN_TEST(test_the_most_specific_rule_wins_in_any_order);
    RUN_TEST(test_the_serial_number_is_ignored);
    RUN_TEST(test_short_form_and_updater);
    RUN_TEST(test_unknown_and_invalid_hwids_select_nothing);
    RUN_TEST(test_equally_specific_rules_of_different_modules_conflict);
    RUN_TEST(test_two_equal_rules_of_one_module_are_no_conflict);
    RUN_TEST(test_the_example_modules_are_valid);
    RUN_TEST(test_the_interface_version_is_checked);
    RUN_TEST(test_rules_are_checked);
    RUN_TEST(test_properties_and_blind_times_are_checked);
    RUN_TEST(test_entities_are_checked);
    RUN_TEST(test_create_and_destroy_come_together);
    RUN_TEST(test_blind_time_per_opcode_with_the_general_value_as_fallback);
    RUN_TEST(test_a_blind_time_only_for_one_opcode_counts_as_a_blind_time);
    RUN_TEST(test_entities_are_found_by_key);
    RUN_TEST(test_a_sensor_value_is_decoded_with_scale_offset_and_decimals);
    RUN_TEST(test_the_signedness_and_the_offset_are_respected);
    RUN_TEST(test_a_switch_or_a_binary_sensor_reports_on_or_off);
    RUN_TEST(test_nothing_is_decoded_that_the_frame_or_the_buffer_cannot_hold);
    RUN_TEST(test_keys_are_safe_for_a_topic_and_the_number_of_entities_is_limited);
    RUN_TEST(test_selection_from_slave_info);
    RUN_TEST(test_a_function_sends_through_the_host_with_typed_values);
    return UNITY_END();
}
