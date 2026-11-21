/*
 * Master modules (docs/spec/06-operation.md section 6.10): the forms of the hardware identifier, the validation of a
 * module, the selection by match rules, the blind times per opcode, and the topics below a slave that a module may use.
 * The table of entities of hubus_module_ha.h has its own test (test_module_ha.c).
 */
#include <stdio.h>
#include <string.h>

#include "hubus_module.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- two modules as a module author writes them -------------------------------------------------------------------- */

/* Functions that do nothing: the master needs all four, and these tests never call them. */
static hubus_module_instance_t *none_create(const hubus_module_host_t *host)
{
    (void)host;
    return NULL;
}

static void none_destroy(hubus_module_instance_t *self)
{
    (void)self;
}

static void none_on_frame(hubus_module_instance_t *self, const hubus_module_host_t *host, hubus_frame_kind_t kind,
                          const uint8_t *data, uint8_t len, bool gap)
{
    (void)self;
    (void)host;
    (void)kind;
    (void)data;
    (void)len;
    (void)gap;
}

static void none_on_message(hubus_module_instance_t *self, const hubus_module_host_t *host, const char *topic,
                            const char *payload, size_t len)
{
    (void)self;
    (void)host;
    (void)topic;
    (void)payload;
    (void)len;
}

#define NO_FUNCTIONS .create = none_create, .destroy = none_destroy, .on_frame = none_on_frame, .on_message = none_on_message

static const hubus_module_rule_t temperature_rules[] = {
    {.pattern = {0x83}, .mask = {0xFF}, .len = 1},
};

static const hubus_module_t temperature_module = {
    HUBUS_MODULE_INIT,
    .name = "temperature",
    .rules = temperature_rules,
    .n_rules = HUBUS_MODULE_LEN(temperature_rules),
    .max_command = 1,
    .has_state = true,
    .has_events = true,
    NO_FUNCTIONS,
};

#define OP_FILL 0x41u
#define OP_SHOW 0x42u

static const hubus_module_rule_t ledstrip_rules[] = {
    {.pattern = {0x84, 0x01}, .mask = {0xFF, 0xFF}, .len = 2},
};

static const hubus_module_blind_t ledstrip_blind[] = {
    {.opcode = OP_FILL, .ms = 0},
};

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
    NO_FUNCTIONS,
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
    m.create = none_create;
    m.destroy = none_destroy;
    m.on_frame = none_on_frame;
    m.on_message = none_on_message;
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

static void test_every_module_has_all_four_functions(void)
{
    hubus_module_t m = temperature_module;

    m.create = NULL;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_FUNCTIONS, hubus_module_validate(&m));
    m = temperature_module;
    m.destroy = NULL;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_FUNCTIONS, hubus_module_validate(&m));
    m = temperature_module;
    m.on_frame = NULL;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_FUNCTIONS, hubus_module_validate(&m));
    m = temperature_module;
    m.on_message = NULL;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_FUNCTIONS, hubus_module_validate(&m));
}

static void test_a_firmware_image_needs_a_length_and_update_support(void)
{
    static const uint8_t image[4] = {1, 2, 3, 4};
    hubus_module_t m = temperature_module;

    m.firmware = image;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_FIRMWARE, hubus_module_validate(&m)); /* no length */
    m.firmware_len = sizeof image;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_FIRMWARE, hubus_module_validate(&m)); /* no update support */
    m.has_update = true;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, hubus_module_validate(&m));
    m.firmware = NULL;
    TEST_ASSERT_EQUAL(HUBUS_MODULE_ERR_FIRMWARE, hubus_module_validate(&m)); /* a length without an image */
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

/* ---- topics below a slave --------------------------------------------------------------------------------------------- */

static void test_topics_are_levels_of_safe_characters(void)
{
    static const char *const good[] = {"a", "relay/set", "lamp/rgb/set", "Channel_2/low-byte",
                                       "012345678901234567890123456789012345678901234567890123456789012"};
    static const char *const bad[] = {"",     "/a",   "a/",   "a//b", "a b", "\xC3\xA4", "a.b",
                                      "+",    "#",    "a/+",  "a/#",  "a+/b", "a/b#",    "a/+b",
                                      "0123456789012345678901234567890123456789012345678901234567890123"};
    size_t i;

    for (i = 0; i < sizeof good / sizeof good[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(hubus_module_topic_valid(good[i], false), good[i]);
        TEST_ASSERT_TRUE_MESSAGE(hubus_module_topic_valid(good[i], true), good[i]);
    }
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(hubus_module_topic_valid(bad[i], false), bad[i]);
    }
    TEST_ASSERT_FALSE(hubus_module_topic_valid(NULL, false));
}

static void test_a_filter_has_whole_level_wildcards_after_the_first_level(void)
{
    static const char *const good[] = {"a/+", "a/#", "a/+/set", "a/+/+", "a/b/#"};
    static const char *const bad[] = {"+", "#", "+/set", "#/a", "a/#/b", "a/+b", "a/b+", "a/#b", "a/++"};
    size_t i;

    for (i = 0; i < sizeof good / sizeof good[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(hubus_module_topic_valid(good[i], true), good[i]);
        TEST_ASSERT_FALSE_MESSAGE(hubus_module_topic_valid(good[i], false), good[i]); /* not for publish */
    }
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(hubus_module_topic_valid(bad[i], true), bad[i]);
    }
}

static void test_the_first_levels_of_the_master_are_reserved(void)
{
    static const char *const reserved[] = {"availability", "capture", "cmd",  "config", "data", "firmware",
                                           "info",         "mcid",    "read", "stats", "update"};
    char t[32];
    size_t i;

    for (i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(hubus_module_topic_reserved(reserved[i]), reserved[i]);
        TEST_ASSERT_FALSE_MESSAGE(hubus_module_topic_valid(reserved[i], false), reserved[i]);
        snprintf(t, sizeof t, "%s/x", reserved[i]);
        TEST_ASSERT_FALSE_MESSAGE(hubus_module_topic_valid(t, true), t);
        snprintf(t, sizeof t, "x/%s", reserved[i]); /* only the first level counts */
        TEST_ASSERT_TRUE_MESSAGE(hubus_module_topic_valid(t, false), t);
    }
    TEST_ASSERT_FALSE(hubus_module_topic_reserved("cmdx"));
    TEST_ASSERT_FALSE(hubus_module_topic_reserved("cm"));
    TEST_ASSERT_TRUE(hubus_module_topic_valid("set", false)); /* the master has no `set` of its own */
}

static void test_filters_match_as_in_mqtt(void)
{
    TEST_ASSERT_TRUE(hubus_module_topic_matches("relay/set", "relay/set"));
    TEST_ASSERT_FALSE(hubus_module_topic_matches("relay/set", "relay/set/x"));
    TEST_ASSERT_FALSE(hubus_module_topic_matches("relay/set", "relay"));
    TEST_ASSERT_FALSE(hubus_module_topic_matches("relay/set", "relay/sett"));
    TEST_ASSERT_TRUE(hubus_module_topic_matches("relay/+", "relay/set"));
    TEST_ASSERT_FALSE(hubus_module_topic_matches("relay/+", "relay/rgb/set"));
    TEST_ASSERT_TRUE(hubus_module_topic_matches("lamp/+/set", "lamp/rgb/set"));
    TEST_ASSERT_TRUE(hubus_module_topic_matches("lamp/#", "lamp/rgb/set"));
    TEST_ASSERT_TRUE(hubus_module_topic_matches("lamp/#", "lamp")); /* '#' takes the parent level as well */
    TEST_ASSERT_FALSE(hubus_module_topic_matches("lamp/#", "lampe"));
    TEST_ASSERT_FALSE(hubus_module_topic_matches("lamp/#", "light/set"));
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
    RUN_TEST(test_every_module_has_all_four_functions);
    RUN_TEST(test_a_firmware_image_needs_a_length_and_update_support);
    RUN_TEST(test_blind_time_per_opcode_with_the_general_value_as_fallback);
    RUN_TEST(test_a_blind_time_only_for_one_opcode_counts_as_a_blind_time);
    RUN_TEST(test_topics_are_levels_of_safe_characters);
    RUN_TEST(test_a_filter_has_whole_level_wildcards_after_the_first_level);
    RUN_TEST(test_the_first_levels_of_the_master_are_reserved);
    RUN_TEST(test_filters_match_as_in_mqtt);
    RUN_TEST(test_selection_from_slave_info);
    return UNITY_END();
}
