/*
 * Entities of Home Assistant for master modules (hubus_module_ha.h): the checks of a table, the values in a frame, and
 * what the ready-made functions publish, subscribe to, and send through a host that records everything.
 */
#include <stdio.h>
#include <string.h>

#include "hubus_module_ha.h"
#include "unity.h"

/* ---- a host that records ------------------------------------------------------------------------------------------- */

#define REC_MAX 16u

typedef struct {
    char topic[HUBUS_MODULE_TOPIC_MAX + 1u];
    char payload[HUBUS_MODULE_PAYLOAD_MAX + 1u];
    char component[32];
    bool retain;
} rec_msg_t;

typedef struct {
    unsigned n_send;
    uint8_t opcode[REC_MAX];
    uint8_t args[REC_MAX][HUBUS_MAX_PAYLOAD];
    uint8_t len[REC_MAX];
    unsigned n_sub;
    char sub[REC_MAX][HUBUS_MODULE_TOPIC_MAX + 1u];
    unsigned n_pub;
    rec_msg_t pub[REC_MAX];
    unsigned n_disc;
    rec_msg_t disc[REC_MAX];
    unsigned n_reject, n_log;
    char last_reject[HUBUS_MODULE_LOG_MAX + 1u];
    hubus_module_status_t send_result;
} rec_t;

static rec_t rec;

static hubus_module_status_t rec_send(const hubus_module_host_t *host, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    (void)host;
    TEST_ASSERT_TRUE(rec.n_send < REC_MAX);
    rec.opcode[rec.n_send] = opcode;
    rec.len[rec.n_send] = len;
    if (len) {
        memcpy(rec.args[rec.n_send], args, len);
    }
    rec.n_send++;
    return rec.send_result;
}

static hubus_module_status_t rec_subscribe(const hubus_module_host_t *host, const char *topic)
{
    (void)host;
    TEST_ASSERT_TRUE(hubus_module_topic_valid(topic, true));
    TEST_ASSERT_TRUE(rec.n_sub < REC_MAX);
    snprintf(rec.sub[rec.n_sub++], sizeof rec.sub[0], "%s", topic);
    return HUBUS_MODULE_OK;
}

static hubus_module_status_t rec_unsubscribe(const hubus_module_host_t *host, const char *topic)
{
    (void)host;
    (void)topic;
    return HUBUS_MODULE_OK;
}

static hubus_module_status_t rec_publish(const hubus_module_host_t *host, const char *topic, const void *payload,
                                         size_t len, bool retain)
{
    rec_msg_t *m = &rec.pub[rec.n_pub];

    (void)host;
    TEST_ASSERT_TRUE(hubus_module_topic_valid(topic, false));
    TEST_ASSERT_TRUE(rec.n_pub < REC_MAX);
    snprintf(m->topic, sizeof m->topic, "%s", topic);
    memcpy(m->payload, payload, len);
    m->payload[len] = '\0';
    m->retain = retain;
    rec.n_pub++;
    return HUBUS_MODULE_OK;
}

static hubus_module_status_t rec_discovery(const hubus_module_host_t *host, const char *component, const char *object,
                                           const char *json)
{
    rec_msg_t *m = &rec.disc[rec.n_disc];

    (void)host;
    TEST_ASSERT_TRUE(rec.n_disc < REC_MAX);
    snprintf(m->component, sizeof m->component, "%s", component);
    snprintf(m->topic, sizeof m->topic, "%s", object);
    snprintf(m->payload, sizeof m->payload, "%s", json);
    rec.n_disc++;
    return HUBUS_MODULE_OK;
}

static size_t rec_topic(const hubus_module_host_t *host, const char *topic, char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "hubus/slave/%u/%s", (unsigned)host->mcid, topic);

    return n > 0 && (size_t)n < out_size ? (size_t)n : 0u;
}

static hubus_module_status_t rec_log(const hubus_module_host_t *host, const char *text)
{
    (void)host;
    (void)text;
    rec.n_log++;
    return HUBUS_MODULE_OK;
}

static hubus_module_status_t rec_reject(const hubus_module_host_t *host, const char *text)
{
    (void)host;
    snprintf(rec.last_reject, sizeof rec.last_reject, "%s", text);
    rec.n_reject++;
    return HUBUS_MODULE_OK;
}

static hubus_module_host_t host_for(const hubus_module_t *m, hubus_module_readback_t readback)
{
    hubus_module_host_t h;

    memset(&h, 0, sizeof h);
    h.module = m;
    h.mcid = 7;
    h.readback = readback;
    h.send = rec_send;
    h.subscribe = rec_subscribe;
    h.unsubscribe = rec_unsubscribe;
    h.publish = rec_publish;
    h.discovery = rec_discovery;
    h.topic = rec_topic;
    h.log = rec_log;
    h.reject = rec_reject;
    return h;
}

static const rec_msg_t *published(const char *topic)
{
    unsigned i;

    for (i = rec.n_pub; i-- > 0u;) {
        if (strcmp(rec.pub[i].topic, topic) == 0) {
            return &rec.pub[i];
        }
    }
    return NULL;
}

static void message(const hubus_module_t *m, const hubus_module_host_t *h, const char *topic, const char *payload)
{
    m->on_message(NULL, h, topic, payload, strlen(payload));
}

void setUp(void)
{
    memset(&rec, 0, sizeof rec);
    rec.send_result = HUBUS_MODULE_OK;
}

void tearDown(void) {}

/* ---- modules as a module author writes them ------------------------------------------------------------------------ */

#define RULE(type) {.pattern = {(type)}, .mask = {0xFF}, .len = 1}

static const hubus_module_rule_t any_rule[] = {RULE(0x83)};

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

static const hubus_ha_t temperature_ha = {temperature_entities, HUBUS_MODULE_LEN(temperature_entities), NULL};

static const hubus_module_t temperature_module = {
    HUBUS_MODULE_INIT,
    .name = "temperature",
    .rules = any_rule,
    .n_rules = 1,
    .max_command = 1,
    .has_state = true,
    .has_events = true,
    .data = &temperature_ha,
    HUBUS_HA_FUNCTIONS,
};

#define OP_RELAY 0x10u
#define OP_FILL 0x41u
#define OP_SHOW 0x42u

/* The parsed values that reach on_set, for the tests to look at. */
static unsigned n_set;
static hubus_value_t last_value;
static char last_text[HUBUS_MODULE_TEXT_MAX + 1u];
static const hubus_entity_t *last_entity;

static hubus_module_status_t mixed_on_set(hubus_module_instance_t *self, const hubus_module_host_t *host,
                                          const hubus_entity_t *entity, const hubus_value_t *value)
{
    (void)self;
    n_set++;
    last_entity = entity;
    last_value = *value;
    if (value->type == HUBUS_VALUE_TEXT) {
        snprintf(last_text, sizeof last_text, "%s", value->as.text);
    }
    if (strcmp(entity->key, "lamp") == 0 && value->type == HUBUS_VALUE_RGB) {
        uint8_t fill[7] = {0, 0, 0, 180, value->as.rgb.g, value->as.rgb.r, value->as.rgb.b}; /* WS2812B: GRB */

        return host->send(host, OP_FILL, fill, sizeof fill);
    }
    return strcmp(entity->key, "limit") == 0 && value->as.number == 5.0 ? HUBUS_MODULE_REJECTED : HUBUS_MODULE_OK;
}

static const hubus_entity_t mixed_entities[] = {
    {
        .key = "relay",
        .name = "Relay",
        .component = HUBUS_COMPONENT_SWITCH,
        .decode = {.offset = 0, .len = 1, .scale = 1.0},
        .on = {.used = true, .opcode = OP_RELAY, .n_args = 1, .args = {1}},
        .off = {.used = true, .opcode = OP_RELAY, .n_args = 1, .args = {0}},
    },
    {.key = "lamp", .name = "Lamp", .component = HUBUS_COMPONENT_LIGHT},
    {.key = "button", .name = "Button", .component = HUBUS_COMPONENT_BINARY_SENSOR, .decode = {.offset = 1, .len = 1, .scale = 1.0}},
    {.key = "note", .name = "Note", .component = HUBUS_COMPONENT_TEXT},
    {.key = "limit", .name = "Limit", .component = HUBUS_COMPONENT_NUMBER, .unit = HUBUS_UNIT_CENTIMETER, .min = 1, .max = 9},
};

static const hubus_ha_t mixed_ha = {mixed_entities, HUBUS_MODULE_LEN(mixed_entities), mixed_on_set};

static const hubus_module_t mixed_module = {
    HUBUS_MODULE_INIT,
    .name = "mixed",
    .rules = any_rule,
    .n_rules = 1,
    .max_command = 31,
    .has_state = true,
    .has_events = true,
    .data = &mixed_ha,
    HUBUS_HA_FUNCTIONS,
};

/* ---- checks of a table --------------------------------------------------------------------------------------------- */

static bool valid_with(hubus_entity_t e, bool with_on_set)
{
    hubus_ha_t ha = {&e, 1, with_on_set ? mixed_on_set : NULL};
    hubus_module_t m = temperature_module;

    m.data = &ha;
    return hubus_ha_valid(&m);
}

static void test_the_example_tables_are_valid(void)
{
    hubus_module_t none = temperature_module;

    TEST_ASSERT_TRUE(hubus_ha_valid(&temperature_module));
    TEST_ASSERT_TRUE(hubus_ha_valid(&mixed_module));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, hubus_module_validate(&temperature_module));
    TEST_ASSERT_EQUAL(HUBUS_MODULE_VALID, hubus_module_validate(&mixed_module));
    none.data = NULL; /* no table: no entities */
    TEST_ASSERT_TRUE(hubus_ha_valid(&none));
    TEST_ASSERT_EQUAL_UINT8(0, hubus_ha_of(&none)->n_entities);
}

static void test_entities_are_checked(void)
{
    const hubus_entity_t sensor = temperature_entities[0];
    const hubus_entity_t twice[] = {sensor, sensor};
    hubus_ha_t ha = {twice, HUBUS_MODULE_LEN(twice), NULL};
    hubus_module_t m = temperature_module;
    hubus_entity_t e;

    e = sensor;
    e.key = NULL;
    TEST_ASSERT_FALSE(valid_with(e, false));
    e = sensor;
    e.name = NULL;
    TEST_ASSERT_FALSE(valid_with(e, false));
    e = sensor;
    e.component = HUBUS_COMPONENT_COUNT; /* from a plugin the compiler cannot check */
    TEST_ASSERT_FALSE(valid_with(e, false));
    e = sensor;
    e.device_class = HUBUS_DEVICE_CLASS_COUNT;
    TEST_ASSERT_FALSE(valid_with(e, false));
    e = sensor;
    e.unit = HUBUS_UNIT_COUNT;
    TEST_ASSERT_FALSE(valid_with(e, false));

    /* a sensor needs a value that fits into a frame */
    e = sensor;
    e.decode.len = 0;
    TEST_ASSERT_FALSE(valid_with(e, false));
    e = sensor;
    e.decode.len = 3;
    TEST_ASSERT_FALSE(valid_with(e, false));
    e = sensor;
    e.decode.offset = 30;
    TEST_ASSERT_FALSE(valid_with(e, false));
    e.decode.offset = 29;
    TEST_ASSERT_TRUE(valid_with(e, false));
    e = sensor;
    e.decode.scale = 0.0; /* every value would read 0 */
    TEST_ASSERT_FALSE(valid_with(e, false));

    /* a switch needs both commands or on_set; a light always on_set */
    e = (hubus_entity_t){.key = "relay", .name = "Relay", .component = HUBUS_COMPONENT_SWITCH};
    TEST_ASSERT_FALSE(valid_with(e, false));
    TEST_ASSERT_TRUE(valid_with(e, true));
    e.on = (hubus_module_command_t){.used = true, .opcode = 0x10};
    TEST_ASSERT_FALSE(valid_with(e, false));
    e.off = (hubus_module_command_t){.used = true, .opcode = 0x10};
    TEST_ASSERT_TRUE(valid_with(e, false));
    e.on.n_args = 1; /* opcode and argument do not fit the 1 byte the module accepts */
    TEST_ASSERT_FALSE(valid_with(e, false));
    e = (hubus_entity_t){.key = "light", .name = "Light", .component = HUBUS_COMPONENT_LIGHT};
    TEST_ASSERT_FALSE(valid_with(e, false));
    TEST_ASSERT_TRUE(valid_with(e, true));

    /* a number needs on_set and a range */
    e = (hubus_entity_t){.key = "limit", .name = "Limit", .component = HUBUS_COMPONENT_NUMBER, .min = 1, .max = 9};
    TEST_ASSERT_FALSE(valid_with(e, false));
    TEST_ASSERT_TRUE(valid_with(e, true));
    e.step = 2;
    TEST_ASSERT_TRUE(valid_with(e, true));
    e.step = 10; /* larger than the range */
    TEST_ASSERT_FALSE(valid_with(e, true));
    e.step = -1;
    TEST_ASSERT_FALSE(valid_with(e, true));
    e.step = 0;
    e.max = 1; /* empty range */
    TEST_ASSERT_FALSE(valid_with(e, true));

    /* keys become topics */
    m.data = &ha;
    TEST_ASSERT_FALSE(hubus_ha_valid(&m));
    ha.n_entities = 1;
    ha.entities = NULL;
    TEST_ASSERT_FALSE(hubus_ha_valid(&m));
}

static void test_keys_are_safe_for_a_topic_and_the_number_of_entities_is_limited(void)
{
    static const char *const good[] = {"a", "relay", "low-byte", "Channel_2", "0123456789012345678901"};
    static const char *const bad[] = {"", "a/b", "a b", "a+", "a#", "\xC3\xA4", "01234567890123456789012345", "cmd", "info"};
    hubus_entity_t e = temperature_entities[0];
    hubus_entity_t many[HUBUS_MODULE_ENTITIES_MAX + 1u];
    char keys[HUBUS_MODULE_ENTITIES_MAX + 1u][8];
    hubus_ha_t ha = {many, HUBUS_MODULE_ENTITIES_MAX, NULL};
    hubus_module_t m = temperature_module;
    size_t i;

    for (i = 0; i < sizeof good / sizeof good[0]; i++) {
        e.key = good[i];
        TEST_ASSERT_TRUE_MESSAGE(valid_with(e, false), good[i]);
    }
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        e.key = bad[i];
        TEST_ASSERT_FALSE_MESSAGE(valid_with(e, false), bad[i]); /* the last two: the first level of a master topic */
    }
    for (i = 0; i <= HUBUS_MODULE_ENTITIES_MAX; i++) {
        many[i] = temperature_entities[0];
        snprintf(keys[i], sizeof keys[i], "e%u", (unsigned)i);
        many[i].key = keys[i];
    }
    m.data = &ha;
    TEST_ASSERT_TRUE(hubus_ha_valid(&m));
    ha.n_entities = HUBUS_MODULE_ENTITIES_MAX + 1u;
    TEST_ASSERT_FALSE(hubus_ha_valid(&m));
    e = temperature_entities[0];
    e.decode.decimals = 7; /* a number that does not fit a short buffer */
    TEST_ASSERT_FALSE(valid_with(e, false));
}

/* ---- the values of an entity --------------------------------------------------------------------------------------- */

static void test_entities_are_found_by_key(void)
{
    TEST_ASSERT_EQUAL_INT(1, hubus_ha_find_entity(&mixed_ha, "lamp"));
    TEST_ASSERT_EQUAL_INT(2, hubus_ha_find_entity(&mixed_ha, "button"));
    TEST_ASSERT_EQUAL_INT(-1, hubus_ha_find_entity(&mixed_ha, "Button")); /* keys are case sensitive */
    TEST_ASSERT_EQUAL_INT(-1, hubus_ha_find_entity(&mixed_ha, ""));
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
    const uint8_t on[2] = {0, 3}, off[2] = {0, 0};
    hubus_entity_t e = mixed_entities[2];
    char out[8];

    TEST_ASSERT_TRUE(hubus_entity_decode(&e, on, 2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("ON", out);
    TEST_ASSERT_TRUE(hubus_entity_decode(&e, off, 2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("OFF", out);
    e.component = HUBUS_COMPONENT_SWITCH;
    TEST_ASSERT_TRUE(hubus_entity_decode(&e, on, 2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("ON", out);
}

static void test_nothing_is_decoded_that_the_frame_or_the_buffer_cannot_hold(void)
{
    const uint8_t data[] = {0x01, 0x2A};
    char out[8];

    TEST_ASSERT_EQUAL_UINT(0, hubus_entity_decode(&temperature_entities[0], data, 1, out, sizeof out)); /* too short */
    TEST_ASSERT_EQUAL_UINT(0, hubus_entity_decode(&temperature_entities[0], data, 0, out, sizeof out));
    TEST_ASSERT_EQUAL_UINT(0, hubus_entity_decode(&temperature_entities[0], data, 2, out, 2)); /* "25" needs 3 */
    TEST_ASSERT_EQUAL_UINT(0, hubus_entity_decode(&mixed_entities[1], data, 2, out, sizeof out)); /* no value */
}

/* ---- announcing --------------------------------------------------------------------------------------------------------- */

static void test_create_subscribes_to_the_entities_that_take_values_and_publishes_discovery(void)
{
    const hubus_module_host_t h = host_for(&mixed_module, HUBUS_READBACK_SLAVE);

    TEST_ASSERT_NOT_NULL(mixed_module.create(&h));
    TEST_ASSERT_EQUAL_UINT(5, rec.n_sub); /* relay, lamp twice, note, limit; not the button */
    TEST_ASSERT_EQUAL_STRING("relay/set", rec.sub[0]);
    TEST_ASSERT_EQUAL_STRING("lamp/set", rec.sub[1]);
    TEST_ASSERT_EQUAL_STRING("lamp/rgb/set", rec.sub[2]);
    TEST_ASSERT_EQUAL_STRING("note/set", rec.sub[3]);
    TEST_ASSERT_EQUAL_STRING("limit/set", rec.sub[4]);

    TEST_ASSERT_EQUAL_UINT(5, rec.n_disc);
    TEST_ASSERT_EQUAL_STRING("switch", rec.disc[0].component);
    TEST_ASSERT_EQUAL_STRING("relay", rec.disc[0].topic);
    TEST_ASSERT_NOT_NULL(strstr(rec.disc[0].payload, "\"state_topic\":\"hubus/slave/7/relay/state\""));
    TEST_ASSERT_NOT_NULL(strstr(rec.disc[0].payload, "\"command_topic\":\"hubus/slave/7/relay/set\""));
    TEST_ASSERT_NOT_NULL(strstr(rec.disc[0].payload, "\"name\":\"Relay\""));
    TEST_ASSERT_NULL(strstr(rec.disc[0].payload, "unique_id")); /* the master adds it, with the device */
    TEST_ASSERT_EQUAL_STRING("light", rec.disc[1].component);
    TEST_ASSERT_NOT_NULL(strstr(rec.disc[1].payload, "\"rgb_command_topic\":\"hubus/slave/7/lamp/rgb/set\""));
    TEST_ASSERT_EQUAL_STRING("binary_sensor", rec.disc[2].component);
    TEST_ASSERT_NULL(strstr(rec.disc[2].payload, "command_topic"));
    TEST_ASSERT_EQUAL_STRING("text", rec.disc[3].component);
    TEST_ASSERT_EQUAL_STRING("number", rec.disc[4].component);
    TEST_ASSERT_NOT_NULL(strstr(rec.disc[4].payload, "\"min\":1.000"));
    TEST_ASSERT_NOT_NULL(strstr(rec.disc[4].payload, "\"unit_of_measurement\":\"cm\""));
}

static void test_a_single_entity_carries_the_name_of_the_device(void)
{
    const hubus_module_host_t h = host_for(&temperature_module, HUBUS_READBACK_SLAVE);

    TEST_ASSERT_NOT_NULL(temperature_module.create(&h));
    TEST_ASSERT_EQUAL_UINT(0, rec.n_sub); /* a sensor takes nothing */
    TEST_ASSERT_EQUAL_UINT(1, rec.n_disc);
    TEST_ASSERT_NOT_NULL(strstr(rec.disc[0].payload, "\"name\":null"));
    TEST_ASSERT_NOT_NULL(strstr(rec.disc[0].payload, "\"device_class\":\"temperature\""));
    TEST_ASSERT_NOT_NULL(strstr(rec.disc[0].payload, "\"state_class\":\"measurement\""));
}

static void test_an_invalid_table_gets_no_instance(void)
{
    const hubus_entity_t bad[] = {{.key = "light", .name = "Light", .component = HUBUS_COMPONENT_LIGHT}};
    const hubus_ha_t ha = {bad, 1, NULL}; /* a light without on_set */
    hubus_module_t m = temperature_module;
    hubus_module_host_t h;

    m.data = &ha;
    h = host_for(&m, HUBUS_READBACK_SLAVE);
    TEST_ASSERT_NULL(m.create(&h));
    TEST_ASSERT_EQUAL_UINT(1, rec.n_log);
    TEST_ASSERT_EQUAL_UINT(0, rec.n_sub + rec.n_disc);
}

/* ---- frames and messages ------------------------------------------------------------------------------------------------- */

static void test_a_frame_publishes_the_state_of_every_entity_it_holds(void)
{
    const hubus_module_host_t h = host_for(&mixed_module, HUBUS_READBACK_SLAVE);
    const uint8_t frame[2] = {1, 0};

    mixed_module.on_frame(NULL, &h, HUBUS_FRAME_STATE, frame, 2, false);
    TEST_ASSERT_EQUAL_UINT(2, rec.n_pub);
    TEST_ASSERT_EQUAL_STRING("ON", published("relay/state")->payload);
    TEST_ASSERT_TRUE(published("relay/state")->retain);
    TEST_ASSERT_EQUAL_STRING("OFF", published("button/state")->payload);

    rec.n_pub = 0;
    mixed_module.on_frame(NULL, &h, HUBUS_FRAME_EVENT, frame, 1, false); /* too short for the button */
    TEST_ASSERT_EQUAL_UINT(1, rec.n_pub);
}

static void test_a_switch_with_fixed_commands_sends_them(void)
{
    const hubus_module_host_t slave = host_for(&mixed_module, HUBUS_READBACK_SLAVE);
    const hubus_module_host_t cache = host_for(&mixed_module, HUBUS_READBACK_CACHE);

    message(&mixed_module, &slave, "relay/set", "on");
    TEST_ASSERT_EQUAL_UINT(1, rec.n_send);
    TEST_ASSERT_EQUAL_HEX8(OP_RELAY, rec.opcode[0]);
    TEST_ASSERT_EQUAL_UINT8(1, rec.len[0]);
    TEST_ASSERT_EQUAL_UINT8(1, rec.args[0][0]);
    TEST_ASSERT_EQUAL_UINT(0, rec.n_pub); /* the master reads the slave back */
    TEST_ASSERT_EQUAL_UINT(0, n_set);

    message(&mixed_module, &cache, "relay/set", "OFF");
    TEST_ASSERT_EQUAL_UINT(2, rec.n_send);
    TEST_ASSERT_EQUAL_UINT8(0, rec.args[1][0]);
    TEST_ASSERT_EQUAL_STRING("OFF", published("relay/state")->payload); /* nobody reads it: what the command establishes */

    rec.send_result = HUBUS_MODULE_FAILED;
    message(&mixed_module, &cache, "relay/set", "ON");
    TEST_ASSERT_EQUAL_UINT(1, rec.n_reject);
    TEST_ASSERT_EQUAL_STRING("OFF", published("relay/state")->payload); /* not sent, not published */
}

static void test_values_are_parsed_for_on_set(void)
{
    const hubus_module_host_t h = host_for(&mixed_module, HUBUS_READBACK_SLAVE);

    n_set = 0;
    message(&mixed_module, &h, "lamp/rgb/set", " 255 , 0,16 ");
    TEST_ASSERT_EQUAL_UINT(1, n_set);
    TEST_ASSERT_EQUAL_PTR(&mixed_entities[1], last_entity);
    TEST_ASSERT_EQUAL(HUBUS_VALUE_RGB, last_value.type);
    TEST_ASSERT_EQUAL_UINT8(255, last_value.as.rgb.r);
    TEST_ASSERT_EQUAL_UINT8(16, last_value.as.rgb.b);
    TEST_ASSERT_EQUAL_HEX8(OP_FILL, rec.opcode[0]);
    TEST_ASSERT_EQUAL_UINT8(0, rec.args[0][4]); /* green goes first */

    message(&mixed_module, &h, "lamp/set", "Off");
    TEST_ASSERT_EQUAL(HUBUS_VALUE_BOOL, last_value.type);
    TEST_ASSERT_FALSE(last_value.as.on);

    message(&mixed_module, &h, "note/set", "hello world");
    TEST_ASSERT_EQUAL(HUBUS_VALUE_TEXT, last_value.type);
    TEST_ASSERT_EQUAL_STRING("hello world", last_text);

    message(&mixed_module, &h, "limit/set", "2.5");
    TEST_ASSERT_EQUAL(HUBUS_VALUE_NUMBER, last_value.type);
    TEST_ASSERT_TRUE(last_value.as.number == 2.5);
    TEST_ASSERT_EQUAL_UINT(4, n_set);
    TEST_ASSERT_EQUAL_UINT(0, rec.n_reject);

    message(&mixed_module, &h, "limit/set", "5"); /* on_set refuses it */
    TEST_ASSERT_EQUAL_UINT(1, rec.n_reject);
    TEST_ASSERT_NOT_NULL(strstr(rec.last_reject, "rejected the value for entity 'limit'"));
}

static void test_what_does_not_fit_is_rejected_before_on_set(void)
{
    static const char *const bad[][2] = {
        {"lamp/rgb/set", "255,0"},    {"lamp/rgb/set", "256,0,0"}, {"lamp/rgb/set", "1,2,3,4"},
        {"relay/rgb/set", "1,2,3"},   {"lamp/set", "dim"},         {"button/set", "ON"},
        {"limit/set", "10"},          {"limit/set", "0.5"},        {"limit/set", "3cm"},
        {"limit/set", ""},            {"nothing/set", "ON"},       {"lamp/state", "ON"},
        {"lamp", "ON"},
    };
    const hubus_module_host_t h = host_for(&mixed_module, HUBUS_READBACK_SLAVE);
    char long_text[HUBUS_MODULE_TEXT_MAX + 2u];
    size_t i;

    n_set = 0;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        rec.n_reject = 0;
        message(&mixed_module, &h, bad[i][0], bad[i][1]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, rec.n_reject, bad[i][0]);
    }
    memset(long_text, 'x', sizeof long_text - 1u);
    long_text[sizeof long_text - 1u] = '\0';
    rec.n_reject = 0;
    message(&mixed_module, &h, "note/set", long_text);
    TEST_ASSERT_EQUAL_UINT(1, rec.n_reject);
    TEST_ASSERT_EQUAL_UINT(0, n_set);
    TEST_ASSERT_EQUAL_UINT(0, rec.n_send);
}

static void test_a_state_is_published_only_for_an_entity_of_the_table(void)
{
    const hubus_module_host_t h = host_for(&mixed_module, HUBUS_READBACK_SLAVE);
    char long_text[HUBUS_MODULE_TEXT_MAX + 2u];

    TEST_ASSERT_EQUAL(HUBUS_MODULE_OK, hubus_ha_publish_state(&h, "lamp", "ON"));
    TEST_ASSERT_EQUAL_STRING("ON", published("lamp/state")->payload);
    TEST_ASSERT_EQUAL(HUBUS_MODULE_REJECTED, hubus_ha_publish_state(&h, "nothing", "ON"));
    memset(long_text, 'x', sizeof long_text - 1u);
    long_text[sizeof long_text - 1u] = '\0';
    TEST_ASSERT_EQUAL(HUBUS_MODULE_REJECTED, hubus_ha_publish_state(&h, "note", long_text));
    TEST_ASSERT_EQUAL_UINT(1, rec.n_pub);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_example_tables_are_valid);
    RUN_TEST(test_entities_are_checked);
    RUN_TEST(test_keys_are_safe_for_a_topic_and_the_number_of_entities_is_limited);
    RUN_TEST(test_entities_are_found_by_key);
    RUN_TEST(test_a_sensor_value_is_decoded_with_scale_offset_and_decimals);
    RUN_TEST(test_the_signedness_and_the_offset_are_respected);
    RUN_TEST(test_a_switch_or_a_binary_sensor_reports_on_or_off);
    RUN_TEST(test_nothing_is_decoded_that_the_frame_or_the_buffer_cannot_hold);
    RUN_TEST(test_create_subscribes_to_the_entities_that_take_values_and_publishes_discovery);
    RUN_TEST(test_a_single_entity_carries_the_name_of_the_device);
    RUN_TEST(test_an_invalid_table_gets_no_instance);
    RUN_TEST(test_a_frame_publishes_the_state_of_every_entity_it_holds);
    RUN_TEST(test_a_switch_with_fixed_commands_sends_them);
    RUN_TEST(test_values_are_parsed_for_on_set);
    RUN_TEST(test_what_does_not_fit_is_rejected_before_on_set);
    RUN_TEST(test_a_state_is_published_only_for_an_entity_of_the_table);
    return UNITY_END();
}
