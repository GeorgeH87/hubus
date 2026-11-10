/*
 * hubusd core against the bus simulation and a fake MQTT client: startup,
 * events, commands, availability, statistics, discovery, faults, reload.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hubus_crc.h"
#include "hubus_sim.h"
#include "hubusd_core.h"
#include "hubusd_ledc.h"
#include "hubusd_modules.h"
#include "ledvm.h"
#include "unity.h"

/* ---- fake MQTT ------------------------------------------------------------------ */

#define MSG_MAX 4096

typedef struct {
    char topic[200];
    char payload[1400];
    int retain;
} msg_t;

static msg_t msgs[MSG_MAX];
static unsigned n_msgs;
static int fail_publish;

static int fake_publish(void *ctx, const char *topic, const char *payload, size_t len, int retain)
{
    (void)ctx;
    if (fail_publish) {
        return -1;
    }
    TEST_ASSERT_TRUE(n_msgs < MSG_MAX);
    TEST_ASSERT_TRUE(len < sizeof msgs[0].payload);
    snprintf(msgs[n_msgs].topic, sizeof msgs[0].topic, "%s", topic);
    memcpy(msgs[n_msgs].payload, payload, len);
    msgs[n_msgs].payload[len] = '\0';
    msgs[n_msgs].retain = retain;
    n_msgs++;
    return 0;
}

static const msg_t *last_msg(const char *topic)
{
    unsigned i;

    for (i = n_msgs; i-- > 0;) {
        if (strcmp(msgs[i].topic, topic) == 0) {
            return &msgs[i];
        }
    }
    return NULL;
}

static const char *last(const char *topic)
{
    const msg_t *m = last_msg(topic);

    return m ? m->payload : NULL;
}

static unsigned count(const char *topic)
{
    unsigned i, n = 0;

    for (i = 0; i < n_msgs; i++) {
        n += strcmp(msgs[i].topic, topic) == 0;
    }
    return n;
}

static unsigned count_containing(const char *topic, const char *needle)
{
    unsigned i, n = 0;

    for (i = 0; i < n_msgs; i++) {
        n += strcmp(msgs[i].topic, topic) == 0 && strstr(msgs[i].payload, needle) != NULL;
    }
    return n;
}

static void assert_last(const char *topic, const char *expected)
{
    const char *p = last(topic);

    TEST_ASSERT_NOT_NULL_MESSAGE(p, topic);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, p, topic);
}

static void assert_contains(const char *topic, const char *needle)
{
    const char *p = last(topic);

    TEST_ASSERT_NOT_NULL_MESSAGE(p, topic);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(p, needle), p);
}

/* ---- simulated devices (behave like the example firmware) ---------------------------- */

enum { DEV_RELAY = 0x81, DEV_SWITCH = 0x82, DEV_TEMP = 0x83, DEV_UNKNOWN = 0x97, DEV_NO_ENTITIES = 0x98, DEV_FUNC = 0xF0 };

/* ---- modules of the test devices ---------------------------------------------------------------------------------- */

#define VARIANT_RULE(id, type, variant) \
    static const hubus_module_rule_t id##_rules[] = {{.pattern = {(type), (variant)}, .mask = {0xFF, 0xFF}, .len = 2}}
#define DEVICE_MODULE(id, with_state, ...) \
    static const hubus_module_t id = {     \
        HUBUS_MODULE_INIT,                 \
        .name = #id,                       \
        .rules = id##_rules,               \
        .n_rules = 1,                      \
        .max_command = HUBUS_MAX_PAYLOAD,  \
        .has_state = (with_state),         \
        .has_events = true,                \
        .has_status_capture = true,        \
        __VA_ARGS__                        \
    }

enum { VARIANT_PLAIN = 1, VARIANT_STATE = 2, VARIANT_BLIND = 3, VARIANT_STATE_BLIND = 4, VARIANT_BLIND_SHOW_ONLY = 5, VARIANT_NO_READBACK = 6, VARIANT_TWO_ENTITIES = 7, VARIANT_SWITCH_SECOND = 8 };
#define OP_BLIND_ONLY 0x42u

VARIANT_RULE(relay_plain, DEV_RELAY, VARIANT_PLAIN);
VARIANT_RULE(relay_state, DEV_RELAY, VARIANT_STATE);
VARIANT_RULE(relay_blind, DEV_RELAY, VARIANT_BLIND);
VARIANT_RULE(relay_state_blind, DEV_RELAY, VARIANT_STATE_BLIND);
VARIANT_RULE(relay_show_only, DEV_RELAY, VARIANT_BLIND_SHOW_ONLY);
VARIANT_RULE(relay_no_readback, DEV_RELAY, VARIANT_NO_READBACK);
VARIANT_RULE(no_entities, DEV_NO_ENTITIES, VARIANT_PLAIN);
VARIANT_RULE(temp_two, DEV_TEMP, VARIANT_TWO_ENTITIES);
VARIANT_RULE(relay_second, DEV_RELAY, VARIANT_SWITCH_SECOND);
VARIANT_RULE(switch_plain, DEV_SWITCH, VARIANT_PLAIN);
VARIANT_RULE(temp_plain, DEV_TEMP, VARIANT_PLAIN);
VARIANT_RULE(temp_state, DEV_TEMP, VARIANT_STATE);
static const hubus_module_blind_t show_only_blind[] = {{.opcode = OP_BLIND_ONLY, .ms = 4}};

/* What the daemon makes of the bytes of the devices: a relay with one state byte, a button, and a sensor. */
static const hubus_entity_t relay_entity[] = {{
    .key = "relay",
    .name = "Relay",
    .component = HUBUS_COMPONENT_SWITCH,
    .decode = {.offset = 0, .len = 1, .scale = 1.0},
    .on = {.used = true, .opcode = 0x10, .n_args = 1, .args = {1}},
    .off = {.used = true, .opcode = 0x10, .n_args = 1, .args = {0}},
}};
static const hubus_entity_t button_entity[] = {
    {.key = "button", .name = "Button", .component = HUBUS_COMPONENT_BINARY_SENSOR, .decode = {.offset = 0, .len = 1, .scale = 1.0}},
};
static const hubus_entity_t temp_entity[] = {{
    .key = "temperature",
    .name = "Temperature",
    .component = HUBUS_COMPONENT_SENSOR,
    .device_class = HUBUS_DEVICE_CLASS_TEMPERATURE,
    .unit = HUBUS_UNIT_CELSIUS,
    .decode = {.offset = 0, .len = 2, .big_endian = true, .scale = 1.0, .add = -273.0},
}};
/* Two entities from one frame: the temperature and whether the low byte of the raw value is not zero. */
static const hubus_entity_t two_entities[] = {
    {
        .key = "temperature",
        .name = "Temperature",
        .component = HUBUS_COMPONENT_SENSOR,
        .device_class = HUBUS_DEVICE_CLASS_TEMPERATURE,
        .unit = HUBUS_UNIT_CELSIUS,
        .decode = {.offset = 0, .len = 2, .big_endian = true, .scale = 1.0, .add = -273.0},
    },
    {.key = "low-byte", .name = "Low byte", .component = HUBUS_COMPONENT_BINARY_SENSOR, .decode = {.offset = 1, .len = 1, .scale = 1.0}},
};
/* The switch is the second entity: what the cache reports for it must not end up with the first. */
static const hubus_entity_t switch_second_entities[] = {
    {.key = "button", .name = "Button", .component = HUBUS_COMPONENT_BINARY_SENSOR, .decode = {.offset = 1, .len = 1, .scale = 1.0}},
    {
        .key = "relay",
        .name = "Relay",
        .component = HUBUS_COMPONENT_SWITCH,
        .on = {.used = true, .opcode = 0x10, .n_args = 1, .args = {1}},
        .off = {.used = true, .opcode = 0x10, .n_args = 1, .args = {0}},
    },
};
#define ENTITIES(array) .entities = (array), .n_entities = HUBUS_MODULE_LEN(array)

DEVICE_MODULE(relay_plain, false, ENTITIES(relay_entity));
DEVICE_MODULE(relay_state, true, ENTITIES(relay_entity));
DEVICE_MODULE(relay_blind, false, .blind_default_ms = 4, ENTITIES(relay_entity));
DEVICE_MODULE(relay_state_blind, true, .blind_default_ms = 4, ENTITIES(relay_entity));
DEVICE_MODULE(relay_show_only, true, .blind_ops = show_only_blind, .n_blind_ops = 1, ENTITIES(relay_entity));
DEVICE_MODULE(no_entities, false);
DEVICE_MODULE(switch_plain, false, ENTITIES(button_entity));
static const hubus_module_t relay_no_readback = {
    HUBUS_MODULE_INIT,
    .name = "relay_no_readback",
    .rules = relay_no_readback_rules,
    .n_rules = 1,
    .max_command = HUBUS_MAX_PAYLOAD,
    .has_events = true,
    ENTITIES(relay_entity),
};
DEVICE_MODULE(temp_plain, false, ENTITIES(temp_entity));
DEVICE_MODULE(temp_state, true, ENTITIES(temp_entity));
DEVICE_MODULE(temp_two, false, ENTITIES(two_entities));
DEVICE_MODULE(relay_second, false, ENTITIES(switch_second_entities));

/* A module with functions: a light and a switch without fixed commands, and a sensor that frames fill as data. */
#define OP_FUNC_COLOUR 0x40u
#define OP_FUNC_POWER 0x41u
#define OP_FUNC_BURST 0x42u
#define FUNC_BURST_COMMANDS 45u

static pthread_mutex_t func_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t func_changed = PTHREAD_COND_INITIALIZER;
static unsigned func_live, func_created, func_destroyed, func_frames;
static unsigned func_frame_byte;
static hubus_frame_kind_t func_frame_kind;
static bool func_frame_gap;
static int func_hang; /* the next call of on_set waits until this is 0 */
static unsigned func_hanging;
static int func_create_fails;

static hubus_module_instance_t *func_create(const hubus_module_host_t *host)
{
    static int instance;

    (void)host;
    pthread_mutex_lock(&func_lock);
    func_created++;
    if (!func_create_fails) {
        func_live++;
    }
    pthread_mutex_unlock(&func_lock);
    return func_create_fails ? NULL : (hubus_module_instance_t *)&instance;
}

static void func_destroy(hubus_module_instance_t *self)
{
    (void)self;
    pthread_mutex_lock(&func_lock);
    func_live--;
    func_destroyed++;
    pthread_mutex_unlock(&func_lock);
}

static void func_on_frame(hubus_module_instance_t *self, const hubus_module_host_t *host, hubus_frame_kind_t kind,
                          const uint8_t *data, uint8_t len, bool gap)
{
    (void)self;
    (void)host;
    pthread_mutex_lock(&func_lock);
    func_frames++;
    func_frame_byte = len ? data[0] : 0;
    func_frame_kind = kind;
    func_frame_gap = gap;
    pthread_mutex_unlock(&func_lock);
}

static hubus_module_status_t func_on_set(hubus_module_instance_t *self, const hubus_module_host_t *host,
                                         const hubus_entity_t *entity, const hubus_value_t *value)
{
    uint8_t args[3];
    unsigned i;

    (void)self;
    pthread_mutex_lock(&func_lock);
    if (func_hang) {
        func_hanging = 1;
        pthread_cond_broadcast(&func_changed);
        while (func_hang) {
            pthread_cond_wait(&func_changed, &func_lock);
        }
        func_hanging = 0;
    }
    pthread_mutex_unlock(&func_lock);
    if (strcmp(entity->key, "burst") == 0) {
        for (i = 0; i < FUNC_BURST_COMMANDS; i++) {
            args[0] = (uint8_t)i;
            if (host->send(host, OP_FUNC_BURST, args, 1) != HUBUS_MODULE_OK) {
                return HUBUS_MODULE_FAILED;
            }
        }
        return HUBUS_MODULE_OK;
    }
    if (value->type == HUBUS_VALUE_RGB) {
        if (value->as.rgb.r == 1) {
            return HUBUS_MODULE_REJECTED;
        }
        args[0] = value->as.rgb.r;
        args[1] = value->as.rgb.g;
        args[2] = value->as.rgb.b;
        (void)host->set_state(host, entity->key, "ON");
        return host->send(host, OP_FUNC_COLOUR, args, 3);
    }
    args[0] = value->as.on ? 1 : 0;
    (void)host->set_state(host, entity->key, value->as.on ? "ON" : "OFF");
    return host->send(host, OP_FUNC_POWER, args, 1);
}

VARIANT_RULE(func_module, DEV_FUNC, VARIANT_PLAIN);
static const hubus_entity_t func_entities[] = {
    {.key = "lamp", .name = "Lamp", .component = HUBUS_COMPONENT_LIGHT},
    {.key = "fan", .name = "Fan", .component = HUBUS_COMPONENT_SWITCH},
    {.key = "burst", .name = "Burst", .component = HUBUS_COMPONENT_SWITCH},
    {
        .key = "relay",
        .name = "Relay",
        .component = HUBUS_COMPONENT_SWITCH,
        .on = {.used = true, .opcode = 0x10, .n_args = 1, .args = {1}},
        .off = {.used = true, .opcode = 0x10, .n_args = 1, .args = {0}},
    },
    {.key = "first", .name = "First byte", .component = HUBUS_COMPONENT_SENSOR, .decode = {.offset = 0, .len = 1, .scale = 1.0}},
};
#define FUNCTIONS .create = func_create, .destroy = func_destroy, .on_frame = func_on_frame, .on_set = func_on_set
DEVICE_MODULE(func_module, false, FUNCTIONS, ENTITIES(func_entities));

static const hubus_module_t *const test_modules[] = {
    &relay_plain, &relay_state, &relay_blind, &relay_state_blind, &relay_show_only, &relay_no_readback, &no_entities, &switch_plain, &temp_plain, &temp_state, &temp_two, &relay_second, &func_module,
};


typedef struct {
    hubus_slave_core_t core;
    uint8_t mcid;
    uint8_t type;
    uint8_t state;      /* relay: 0/1, switch: pressed, temperature: unused */
    uint16_t raw_temp;
    volatile uint8_t temp_be[2]; /* raw_temp as exposed state (big endian) */
    uint8_t hwid[HUBUS_HWID_SERIAL + 4u];
    unsigned commands;
    unsigned captures;
    uint8_t last_op;
    uint8_t last_args[HUBUS_MAX_PAYLOAD];
    uint8_t last_len;
    uint8_t ops[16];      /* the first opcodes, as a history */
    uint8_t code[64];     /* what PROG_WRITE delivered */
    int blind_phase;      /* 1: command received, 2: blind after its STOP */
    unsigned stop_mark;
    uint64_t blind_end_ms;
} sim_dev_t;

static hubus_sim_t sim;
static int dev_with_state; /* relay and temperature sensor expose a live state (REQUEST_STATE) */
static sim_dev_t dev[8];
static unsigned n_dev;
static hubusd_t *dmn;
static hubusd_config_t cfg;
static uint64_t clock_ms;
static uint8_t dev_force_variant; /* of slave 3; 0: from the other settings */
static uint8_t dev_variant_seven; /* of slave 7; 0: from the other settings */
static int dev_blind_op;          /* the only opcode that makes the blind device blind; -1: every opcode */
static uint8_t dev_nine_type;     /* type of slave 9 in the standard setup */
static uint8_t dev_blind_mcid;  /* this device is blind for 4 ms after a command */

static void dev_capture(void *ctx)
{
    sim_dev_t *v = ctx;
    uint8_t b[2];

    v->captures++;
    if (v->type == DEV_TEMP) {
        b[0] = (uint8_t)(v->raw_temp >> 8);
        b[1] = (uint8_t)v->raw_temp;
        (void)hubus_core_write(&v->core, b, 2, 0);
    } else {
        (void)hubus_core_write(&v->core, &v->state, 1, 0);
    }
}

static void dev_command(void *ctx, uint8_t op, const uint8_t *args, uint8_t len)
{
    sim_dev_t *v = ctx;

    if (v->commands < sizeof v->ops) {
        v->ops[v->commands] = op;
    }
    v->commands++;
    v->last_op = op;
    v->last_len = len;
    if (len) {
        memcpy(v->last_args, args, len);
    }
    if (op == 0x43 && len >= 1 && args[0] + len - 1u <= sizeof v->code) { /* PROG_WRITE of the LED strip */
        memcpy(&v->code[args[0]], &args[1], len - 1u);
    }
    if (v->mcid == dev_blind_mcid && (dev_blind_op < 0 || dev_blind_op == op)) {
        v->blind_phase = 1;
        v->stop_mark = sim.n_stop;
        v->blind_end_ms = clock_ms + 4u;
    }
    if (v->type == DEV_RELAY && op == 0x10 && len >= 1) {
        v->state = args[0] ? 1 : 0;
    }
}

/* The blind device sees nothing from the STOP of a command until its blind section ends, then resynchronizes. A frame
 * the daemon sends it too early is lost, like on real hardware. */
static int dev_blind_filter(hubus_sim_t *s, unsigned slave, uint8_t *clk, uint8_t *data, void *ctx)
{
    sim_dev_t *v = &dev[slave];

    (void)ctx;
    if (v->blind_phase == 1 && s->n_stop > v->stop_mark) {
        v->blind_phase = 2; /* the STOP itself is seen */
        return 1;
    }
    if (v->blind_phase == 2) {
        if (clock_ms < v->blind_end_ms) {
            return 0;
        }
        hubus_core_resync(&v->core, *clk, *data);
        v->blind_phase = 0;
    }
    return 1;
}

static void set_temp(sim_dev_t *v, uint16_t raw)
{
    v->raw_temp = raw;
    v->temp_be[0] = (uint8_t)(raw >> 8);
    v->temp_be[1] = (uint8_t)raw;
}

static uint8_t dev_variant(uint8_t mcid, uint8_t type)
{
    int state = dev_with_state && (type == DEV_RELAY || type == DEV_TEMP);

    if (mcid == 3 && dev_force_variant) {
        return dev_force_variant;
    }
    if (mcid == 7 && dev_variant_seven) {
        return dev_variant_seven;
    }
    if (mcid == dev_blind_mcid) {
        return state ? VARIANT_STATE_BLIND : VARIANT_BLIND;
    }
    return state ? VARIANT_STATE : VARIANT_PLAIN;
}

static sim_dev_t *add_dev(uint8_t mcid, uint8_t type)
{
    sim_dev_t *v = &dev[n_dev++];
    hubus_core_config_t c;

    memset(v, 0, sizeof *v);
    v->mcid = mcid;
    v->type = type;
    v->raw_temp = 298;
    v->hwid[HUBUS_HWID_TYPE] = type;
    v->hwid[HUBUS_HWID_VARIANT] = dev_variant(mcid, type);
    v->hwid[HUBUS_HWID_VERSION] = 1;
    v->hwid[HUBUS_HWID_SERIAL + 3u] = mcid; /* serial: 00 00 00 <mcid> */
    memset(&c, 0, sizeof c);
    c.mcid = mcid;
    c.hwid = v->hwid;
    c.hwid_len = sizeof v->hwid;
    c.on_status_capture = dev_capture;
    c.on_command = dev_command;
    c.speed_class = mcid == 3 ? 4u : 0u;
    c.has_blind_time = mcid == dev_blind_mcid ? 1u : 0u;
    if (dev_with_state && type == DEV_RELAY) {
        c.state = &v->state;
        c.state_len = 1;
    } else if (dev_with_state && type == DEV_TEMP) {
        set_temp(v, 298);
        c.state = v->temp_be;
        c.state_len = 2;
    }
    c.ctx = v;
    TEST_ASSERT_TRUE(hubus_core_init(&v->core, &c, sim.clk, sim.data));
    hubus_sim_add(&sim, &v->core);
    return v;
}

static uint64_t clk_mono(void *ctx)
{
    (void)ctx;
    return clock_ms;
}

static uint64_t clk_wall(void *ctx)
{
    (void)ctx;
    return 1700000000u + clock_ms / 1000u;
}

static void load(const char *yaml)
{
    char err[256];

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hubusd_config_parse(yaml, strlen(yaml), &cfg, err, sizeof err), err);
}

static void start_daemon(void)
{
    hubus_master_hal_t hal = hubus_sim_master_hal(&sim);
    hubusd_mqtt_t mq = {NULL, fake_publish};
    hubusd_clock_t clk = {NULL, clk_mono, clk_wall};

    dmn = malloc(sizeof *dmn);
    TEST_ASSERT_NOT_NULL(dmn);
    hubusd_init(dmn, &cfg, &hal, mq, clk);
    hubusd_set_modules(dmn, test_modules, sizeof test_modules / sizeof test_modules[0]);
}

/* Runs the bus thread until it has nothing to do (bounded). */
static void settle(void)
{
    unsigned i;

    for (i = 0; i < 50; i++) {
        if (!hubusd_step(dmn)) {
            return;
        }
    }
}

static void advance(unsigned seconds)
{
    unsigned i;

    for (i = 0; i < seconds; i++) {
        clock_ms += 1000;
        settle();
    }
}

static const char *base_yaml =
    "mqtt:\n"
    "  stats_interval_s: 10\n"
    "bus:\n"
    "  ping_interval_s: 5\n"
    "  ping_failures: 2\n"
    "  scan_interval_s: 0\n"
    "slaves:\n"
    "  - mcid: 3\n"
    "    name: Garden relay\n"
    "  - mcid: 5\n"
    "    name: Door button\n"
    "  - mcid: 7\n"
    "    name: Cellar\n";

/* Relay 3, button 5, temperature 7, and an unconfigured device 9. */
static void standard_setup(const char *yaml)
{
    load(yaml);
    add_dev(3, DEV_RELAY);
    add_dev(5, DEV_SWITCH);
    add_dev(7, DEV_TEMP);
    add_dev(9, dev_nine_type);
    start_daemon();
    settle();
}

void setUp(void)
{
    hubus_sim_init(&sim);
    hubus_sim_check_timing(&sim, 50000, 50000, 50000);
    n_msgs = 0;
    n_dev = 0;
    dev_with_state = 0;
    dev_blind_mcid = 0;
    dev_blind_op = -1;
    dev_force_variant = 0;
    dev_variant_seven = 0;
    dev_nine_type = DEV_UNKNOWN;
    sim.filter = dev_blind_filter;
    clock_ms = 100000;
    fail_publish = 0;
    dmn = NULL;
    func_live = func_created = func_destroyed = func_frames = 0;
    func_frame_byte = 0;
    func_frame_gap = false;
    func_hang = 0;
    func_create_fails = 0;
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT(0, sim.violations);
    TEST_ASSERT_EQUAL_UINT(0, sim.timing_violations);
    if (dmn) {
        hubusd_destroy(dmn);
        free(dmn);
    }
}

static void say(const char *topic, const char *payload)
{
    hubusd_handle_message(dmn, topic, payload, strlen(payload));
}

/* ---- startup ---------------------------------------------------------------------------- */

static void test_startup_publishes_state_info_availability_and_discovery(void)
{
    standard_setup(base_yaml);

    assert_last("hubus/master/status", "online");
    TEST_ASSERT_TRUE(last_msg("hubus/master/status")->retain);
    assert_contains("hubus/master/state", "\"bus\":\"ok\"");
    assert_contains("hubus/master/state", "\"configured_slaves\":3");
    assert_contains("hubus/master/state", "\"online_slaves\":3");
    assert_last("hubus/slave/3/availability", "online");
    assert_last("hubus/slave/5/availability", "online");
    assert_last("hubus/slave/7/availability", "online");
    TEST_ASSERT_NULL(last("hubus/slave/9/availability")); /* unconfigured slaves are ignored */

    assert_contains("hubus/slave/3/info", "\"device_type\":129");
    assert_contains("hubus/slave/3/info", "\"serial\":\"00000003\"");
    assert_contains("hubus/slave/3/info", "\"entities\":[\"relay\"]");
    assert_contains("hubus/slave/3/info", "\"status_capture\":true");
    assert_contains("hubus/slave/3/info", "\"update\":false");
    assert_contains("hubus/slave/3/info", "\"updater\":false");
    assert_contains("hubus/slave/3/info", "\"module\":\"relay_plain\"");
    assert_contains("hubus/slave/3/info", "\"speed_class\":4");
    assert_contains("hubus/slave/5/info", "\"speed_class\":0");
    TEST_ASSERT_TRUE(last_msg("hubus/slave/3/info")->retain);

    /* initial state is read back from the slaves */
    assert_last("hubus/slave/3/relay/state", "OFF");
    assert_last("hubus/slave/5/button/state", "OFF");
    assert_last("hubus/slave/7/temperature/state", "25");
    TEST_ASSERT_TRUE(last_msg("hubus/slave/3/relay/state")->retain);
}

static void test_unknown_slave_is_reported_in_master_state_only(void)
{
    standard_setup(base_yaml);
    assert_contains("hubus/master/state", "\"unknown_slaves\":[{\"mcid\":9,\"device_type\":151,\"serial\":\"00000009\"}]");
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "unconfigured slave found at MCID 9") >= 1);
    TEST_ASSERT_NULL(last("hubus/slave/9/info"));
    TEST_ASSERT_NULL(last("hubus/slave/9/relay/state"));
}

static void test_discovery_messages(void)
{
    standard_setup(base_yaml);

    assert_contains("homeassistant/switch/hubus_3_relay/config", "\"command_topic\":\"hubus/slave/3/relay/set\"");
    assert_contains("homeassistant/switch/hubus_3_relay/config", "\"state_topic\":\"hubus/slave/3/relay/state\"");
    assert_contains("homeassistant/switch/hubus_3_relay/config", "\"unique_id\":\"hubus_3_relay\"");
    assert_contains("homeassistant/switch/hubus_3_relay/config", "\"name\":null");
    assert_contains("homeassistant/switch/hubus_3_relay/config", "\"availability_mode\":\"all\"");
    assert_contains("homeassistant/switch/hubus_3_relay/config", "{\"topic\":\"hubus/master/status\"}");
    assert_contains("homeassistant/switch/hubus_3_relay/config", "{\"topic\":\"hubus/slave/3/availability\"}");
    assert_contains("homeassistant/switch/hubus_3_relay/config", "\"name\":\"Garden relay\"");
    assert_contains("homeassistant/switch/hubus_3_relay/config", "\"serial_number\":\"00000003\"");
    assert_contains("homeassistant/switch/hubus_3_relay/config", "\"sw_version\":\"1\"");
    TEST_ASSERT_TRUE(last_msg("homeassistant/switch/hubus_3_relay/config")->retain);

    assert_contains("homeassistant/binary_sensor/hubus_5_button/config", "\"payload_on\":\"ON\"");
    TEST_ASSERT_NULL(strstr(last("homeassistant/binary_sensor/hubus_5_button/config"), "command_topic"));

    assert_contains("homeassistant/sensor/hubus_7_temperature/config", "\"device_class\":\"temperature\"");
    assert_contains("homeassistant/sensor/hubus_7_temperature/config", "\"unit_of_measurement\":\"\xC2\xB0" "C\"");
    assert_contains("homeassistant/sensor/hubus_7_temperature/config", "\"state_class\":\"measurement\"");

    /* the master itself */
    assert_contains("homeassistant/sensor/hubusd_hubusd_uptime_s/config", "\"value_template\":\"{{ value_json.uptime_s }}\"");
    assert_contains("homeassistant/sensor/hubusd_hubusd_errors_total/config", "\"entity_category\":\"diagnostic\"");
    TEST_ASSERT_NULL(last("homeassistant/switch/hubus_9_relay/config"));
}

static void test_discovery_can_be_disabled(void)
{
    unsigned i;

    standard_setup("mqtt:\n  discovery: false\nbus:\n  scan_interval_s: 0\nslaves:\n  - mcid: 3\n");
    for (i = 0; i < n_msgs; i++) {
        TEST_ASSERT_NULL_MESSAGE(strstr(msgs[i].topic, "homeassistant/"), msgs[i].topic);
    }
}

static void test_discovery_cleanup_clears_unconfigured_mcids(void)
{
    standard_setup("mqtt:\n  discovery_cleanup: true\nbus:\n  scan_interval_s: 0\nslaves:\n  - mcid: 3\n");
    TEST_ASSERT_EQUAL_UINT(1, count("homeassistant/switch/hubus_20_relay/config")); /* once, though several modules have it */
    assert_last("homeassistant/switch/hubus_20_relay/config", "");
    TEST_ASSERT_EQUAL_UINT(1, count("homeassistant/switch/hubus_20/config")); /* the entry of earlier versions */
    assert_last("homeassistant/switch/hubus_20/config", "");
    assert_last("homeassistant/sensor/hubus_63_temperature/config", "");
    TEST_ASSERT_NOT_EQUAL(0, strlen(last("homeassistant/switch/hubus_3_relay/config")));
}

static void test_custom_base_topic_and_prefix(void)
{
    standard_setup("mqtt:\n  base_topic: home/bus\n  discovery_prefix: ha\nbus:\n  scan_interval_s: 0\nslaves:\n  - mcid: 3\n");
    assert_last("home/bus/master/status", "online");
    assert_last("home/bus/slave/3/availability", "online");
    assert_contains("ha/switch/hubus_3_relay/config", "\"command_topic\":\"home/bus/slave/3/relay/set\"");
    TEST_ASSERT_NULL(last("hubus/master/status"));
}

/* ---- events ------------------------------------------------------------------------------ */

static void test_button_press_is_published(void)
{
    uint8_t pressed = 1, released = 0;

    standard_setup(base_yaml);
    n_msgs = 0;
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&sim, 1, &pressed, 1, 0));
    settle();
    assert_last("hubus/slave/5/button/state", "ON");
    assert_contains("hubus/slave/5/data", "\"data\":\"01\"");
    assert_contains("hubus/slave/5/data", "\"result\":\"normal\"");
    assert_contains("hubus/slave/5/data", "\"mcid\":5");
    TEST_ASSERT_FALSE(last_msg("hubus/slave/5/data")->retain);

    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&sim, 1, &released, 1, 0));
    settle();
    assert_last("hubus/slave/5/button/state", "OFF");
    TEST_ASSERT_EQUAL_UINT(2, count("hubus/slave/5/button/state"));
}

static void test_data_from_an_unconfigured_slave_is_ignored(void)
{
    uint8_t v = 1;

    standard_setup(base_yaml);
    n_msgs = 0;
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&sim, 3, &v, 1, 0)); /* device 9 */
    settle();
    TEST_ASSERT_NULL(last("hubus/slave/9/data"));
    TEST_ASSERT_NULL(last("hubus/slave/9/relay/state"));
}

static void test_overwritten_events_are_flagged(void)
{
    uint8_t a = 1, b = 0, c = 1;

    standard_setup(base_yaml);
    n_msgs = 0;
    hubus_sim_slave_write(&sim, 1, &a, 1, 0);
    hubus_sim_slave_write(&sim, 1, &b, 1, 0);
    hubus_sim_slave_write(&sim, 1, &c, 1, 0);
    settle();
    assert_contains("hubus/slave/5/data", "\"result\":\"gap\"");
    assert_contains("hubus/slave/5/data", "\"lost\":2");
}

/* ---- commands ------------------------------------------------------------------------------ */

static void test_set_switches_the_relay_and_publishes_the_read_back_state(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    say("hubus/slave/3/relay/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
    TEST_ASSERT_EQUAL_UINT(1, dev[0].commands);
    assert_last("hubus/slave/3/relay/state", "ON"); /* read back from the slave, not echoed */

    say("hubus/slave/3/relay/set", "off");
    settle();
    TEST_ASSERT_EQUAL_UINT8(0, dev[0].state);
    assert_last("hubus/slave/3/relay/state", "OFF");
}

static void test_published_state_is_the_real_state_not_the_command(void)
{
    standard_setup(base_yaml);
    dev[0].core.on_command = NULL; /* the relay ignores commands (defective) */
    n_msgs = 0;
    say("hubus/slave/3/relay/set", "ON");
    settle();
    assert_last("hubus/slave/3/relay/state", "OFF");
}

static void test_readback_can_be_disabled(void)
{
    standard_setup("bus:\n  readback: false\n  scan_interval_s: 0\nslaves:\n  - mcid: 3\n");
    n_msgs = 0;
    say("hubus/slave/3/relay/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
    TEST_ASSERT_NULL(last("hubus/slave/3/relay/state"));
    say("hubus/slave/3/capture", "");
    settle();
    assert_last("hubus/slave/3/relay/state", "ON");
}

static void test_cache_readback_reports_the_command_without_contacting_the_slave(void)
{
    standard_setup("bus:\n  readback: cache\n  scan_interval_s: 0\n  ping_interval_s: 0\nslaves:\n  - mcid: 3\n");
    TEST_ASSERT_EQUAL_UINT(1, dev[0].captures); /* the initial state read at startup */
    n_msgs = 0;
    say("hubus/slave/3/relay/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
    TEST_ASSERT_EQUAL_UINT(1, dev[0].captures); /* no STATUS_CAPTURE after the command */
    assert_last("hubus/slave/3/relay/state", "ON");
    TEST_ASSERT_TRUE(last_msg("hubus/slave/3/relay/state")->retain);
    TEST_ASSERT_EQUAL_UINT(0, count("hubus/slave/3/data"));
    say("hubus/slave/3/relay/set", "off");
    settle();
    assert_last("hubus/slave/3/relay/state", "OFF");
    TEST_ASSERT_EQUAL_UINT(1, dev[0].captures);
    /* an explicit capture still asks the slave */
    say("hubus/slave/3/capture", "");
    settle();
    TEST_ASSERT_EQUAL_UINT(2, dev[0].captures);
}

static void test_cache_readback_does_not_know_about_a_defective_slave(void)
{
    standard_setup("bus:\n  readback: cache\n  scan_interval_s: 0\n  ping_interval_s: 0\nslaves:\n  - mcid: 3\n");
    dev[0].core.on_command = NULL; /* the relay ignores commands */
    say("hubus/slave/3/relay/set", "ON");
    settle();
    assert_last("hubus/slave/3/relay/state", "ON"); /* the cache is optimistic; mode `slave` would show OFF */
    TEST_ASSERT_EQUAL_UINT8(0, dev[0].state);
}

static void test_cache_readback_leaves_raw_commands_alone(void)
{
    standard_setup("bus:\n  readback: cache\n  scan_interval_s: 0\n  ping_interval_s: 0\nslaves:\n  - mcid: 3\n");
    n_msgs = 0;
    say("hubus/slave/3/cmd", "0x10 1");
    settle();
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
    TEST_ASSERT_NULL(last("hubus/slave/3/relay/state")); /* the effect of a raw command is not known */
}

static void test_readback_mode_per_slave_overrides_the_bus_default(void)
{
    standard_setup("bus:\n  readback: slave\n  scan_interval_s: 0\n  ping_interval_s: 0\n"
                   "slaves:\n  - mcid: 3\n    readback: cache\n  - mcid: 5\n");
    dev[0].core.on_command = NULL;
    say("hubus/slave/3/relay/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dev[0].captures);
    assert_last("hubus/slave/3/relay/state", "ON");
}

/* A relay with neither a state nor STATUS_CAPTURE (built without both): readback `slave` falls back to the cache. */
static void test_slave_that_cannot_be_read_back_reports_the_cached_state(void)
{
    load("bus:\n  readback: slave\n  scan_interval_s: 0\n  ping_interval_s: 0\nslaves:\n  - mcid: 3\n");
    dev_force_variant = VARIANT_NO_READBACK;
    add_dev(3, DEV_RELAY);
    dev[0].core.on_status_capture = NULL;
    start_daemon();
    settle();
    TEST_ASSERT_NULL(last("hubus/slave/3/relay/state")); /* nothing to read at startup */
    say("hubus/slave/3/relay/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
    assert_last("hubus/slave/3/relay/state", "ON");
    TEST_ASSERT_TRUE(last_msg("hubus/slave/3/relay/state")->retain);
    TEST_ASSERT_EQUAL_UINT(0, dev[0].captures);
    TEST_ASSERT_EQUAL_UINT(0, dmn->master.counters.state_requests);
}

/* The cached state belongs to the entity that was switched; a reconnect publishes it again on that entity's topic. */
static void test_the_cached_state_belongs_to_the_entity_that_was_switched(void)
{
    load("bus:\n  readback: cache\n  scan_interval_s: 0\n  ping_interval_s: 0\nslaves:\n  - mcid: 3\n");
    dev_force_variant = VARIANT_SWITCH_SECOND;
    add_dev(3, DEV_RELAY);
    start_daemon();
    settle();
    say("hubus/slave/3/relay/set", "ON");
    settle();
    assert_last("hubus/slave/3/relay/state", "ON");
    TEST_ASSERT_NULL(last("hubus/slave/3/button/state"));
    n_msgs = 0;
    hubusd_on_connect(dmn);
    settle();
    assert_last("hubus/slave/3/relay/state", "ON");
    TEST_ASSERT_TRUE(last_msg("hubus/slave/3/relay/state")->retain);
    TEST_ASSERT_NULL(last("hubus/slave/3/button/state"));
}

/* A sensor that never announces changes: poll_interval_s reads its state periodically. */
static void test_poll_interval_reads_the_state_periodically(void)
{
    dev_with_state = 1;
    standard_setup("bus:\n  scan_interval_s: 0\n  ping_interval_s: 0\nslaves:\n  - mcid: 3\n  - mcid: 7\n"
                   "    poll_interval_s: 10\n");
    set_temp(&dev[2], 311);
    n_msgs = 0;
    advance(9);
    TEST_ASSERT_NULL(last("hubus/slave/7/temperature/state")); /* not yet due */
    advance(1);
    assert_last("hubus/slave/7/temperature/state", "38");
    TEST_ASSERT_EQUAL_UINT(0, dev[2].captures); /* REQUEST_STATE, no capture */
    set_temp(&dev[2], 301);
    advance(10);
    assert_last("hubus/slave/7/temperature/state", "28");
    TEST_ASSERT_EQUAL_UINT(2, count("hubus/slave/7/temperature/state"));
    TEST_ASSERT_NULL(last("hubus/slave/3/relay/state")); /* only the slave with the option is polled */
}

/* Polling a slave without a state uses STATUS_CAPTURE and a burst. */
static void test_poll_interval_captures_a_slave_without_a_state(void)
{
    standard_setup("bus:\n  scan_interval_s: 0\n  ping_interval_s: 0\nslaves:\n  - mcid: 7\n    poll_interval_s: 5\n");
    TEST_ASSERT_EQUAL_UINT(1, dev[2].captures); /* the initial state read at startup */
    dev[2].raw_temp = 303;
    n_msgs = 0;
    advance(5);
    TEST_ASSERT_EQUAL_UINT(2, dev[2].captures);
    assert_last("hubus/slave/7/temperature/state", "30");
}

static void test_invalid_set_commands_are_rejected(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    say("hubus/slave/3/relay/set", "TOGGLE");
    say("hubus/slave/5/button/set", "ON");  /* a button takes no commands */
    say("hubus/slave/9/relay/set", "ON");  /* not configured */
    say("hubus/slave/4/relay/set", "ON");  /* not configured */
    settle();
    TEST_ASSERT_EQUAL_UINT(4, count_containing("hubus/master/log", "command rejected"));
    TEST_ASSERT_EQUAL_UINT(0, dev[0].commands);
    TEST_ASSERT_EQUAL_UINT(4, dmn->stats.commands_rejected);
}

static void test_malformed_topics_and_payloads_are_rejected(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    say("hubus/slave/0/relay/set", "ON");
    say("hubus/slave/64/relay/set", "ON");
    say("hubus/slave/x/set", "ON");
    say("hubus/slave/3/cmd", "");
    say("hubus/slave/3/cmd", "0x05");           /* reserved opcode */
    say("hubus/slave/3/cmd", "0x10 300");       /* byte out of range */
    say("hubus/slave/3/cmd", "0x10 abc");
    say("hubus/slave/3/cmd", "0x00 1");         /* STATUS_CAPTURE takes no arguments */
    say("hubus/group/64/0/cmd", "0x10");
    say("hubus/group/0/6/cmd", "0x10");         /* group length 6 is exact addressing */
    say("hubus/group/0/0/cmd", "0x05");
    settle();
    TEST_ASSERT_EQUAL_UINT(11, dmn->stats.commands_rejected);
    TEST_ASSERT_EQUAL_UINT(0, dev[0].commands);
}

static void test_unrelated_topics_are_ignored(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    say("other/slave/3/relay/set", "ON");
    say("hubus/slave/3/relay/state", "ON");
    say("hubus/master/status", "online");
    say("hubus", "x");
    settle();
    TEST_ASSERT_EQUAL_UINT(0, dmn->stats.commands_rejected);
    TEST_ASSERT_EQUAL_UINT(0, dev[0].commands);
}

static void test_oversized_payload_is_rejected(void)
{
    char big[2000];

    standard_setup(base_yaml);
    memset(big, 'A', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    say("hubus/slave/3/relay/set", big);
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dmn->stats.commands_rejected);
}

static void test_set_for_an_entity_the_module_does_not_have_is_rejected(void)
{
    dev_nine_type = DEV_NO_ENTITIES;
    standard_setup("bus:\n  scan_interval_s: 0\nslaves:\n  - mcid: 9\n");
    n_msgs = 0;
    say("hubus/slave/9/relay/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dmn->stats.commands_rejected);
    TEST_ASSERT_EQUAL_UINT(1, count_containing("hubus/master/log", "has no entity"));
    say("hubus/slave/9/cmd", "0x10 1"); /* the raw topic still works */
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dmn->stats.commands_rejected);
    TEST_ASSERT_EQUAL_UINT(1, dev[3].commands); /* the raw command reached the slave */
}

/* An unknown HWID is not taken into operation (docs/spec/06-operation.md section 6.10). */
static void test_a_slave_without_a_module_gets_no_commands(void)
{
    standard_setup("bus:\n  scan_interval_s: 0\nslaves:\n  - mcid: 9\n");
    TEST_ASSERT_TRUE(dmn->slave[9].have_info);
    TEST_ASSERT_NULL(dmn->slave[9].module);
    assert_contains("hubus/slave/9/info", "\"module\":\"\"");
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "no module") >= 1u);
    n_msgs = 0;
    say("hubus/slave/9/cmd", "0x10 1");
    settle();
    TEST_ASSERT_EQUAL_UINT(0, dev[3].commands);
    TEST_ASSERT_EQUAL_UINT(1, dmn->stats.commands_rejected);
    TEST_ASSERT_EQUAL_UINT(1, count_containing("hubus/master/log", "has no module"));
}

static void test_raw_command_exact(void)
{
    standard_setup(base_yaml);
    say("hubus/slave/3/cmd", "0x10 1");
    settle();
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
    say("hubus/slave/3/cmd", "16, 0");
    settle();
    TEST_ASSERT_EQUAL_UINT8(0, dev[0].state);
    TEST_ASSERT_EQUAL_UINT(2, dev[0].commands);
}

static void test_raw_command_larger_than_the_slave_accepts_is_rejected(void)
{
    hubus_module_t small;

    standard_setup(base_yaml);
    small = *dmn->slave[3].module;
    small.max_command = 2;
    dmn->slave[3].module = &small;
    say("hubus/slave/3/cmd", "0x10 1 2");
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dmn->stats.commands_rejected);
    TEST_ASSERT_EQUAL_UINT(0, dev[0].commands);
}

static void test_group_and_broadcast_commands(void)
{
    standard_setup(base_yaml);
    say("hubus/group/0/0/cmd", "0x10 1");   /* broadcast */
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dev[0].commands);
    TEST_ASSERT_EQUAL_UINT(1, dev[1].commands);
    TEST_ASSERT_EQUAL_UINT(1, dev[2].commands);
    TEST_ASSERT_EQUAL_UINT(1, dev[3].commands);
    say("hubus/group/0/2/cmd", "0x10 0");   /* 00xxxx: MCIDs 3, 5, 7, 9 */
    settle();
    TEST_ASSERT_EQUAL_UINT(2, dev[0].commands);
    say("hubus/group/4/5/cmd", "0x10 0");   /* 00010x: MCID 4 and 5 */
    settle();
    TEST_ASSERT_EQUAL_UINT(2, dev[0].commands);
    TEST_ASSERT_EQUAL_UINT(3, dev[1].commands);
}

static void test_group_status_capture_collects_states(void)
{
    standard_setup(base_yaml);
    dev[0].state = 1;
    n_msgs = 0;
    say("hubus/group/0/0/cmd", "0");
    settle();
    assert_last("hubus/slave/3/relay/state", "ON");
    assert_last("hubus/slave/7/temperature/state", "25");
}

static void test_capture_topic(void)
{
    standard_setup(base_yaml);
    dev[2].raw_temp = 303;
    n_msgs = 0;
    say("hubus/slave/7/capture", "");
    settle();
    assert_last("hubus/slave/7/temperature/state", "30");
    TEST_ASSERT_EQUAL_UINT(2, dev[2].captures); /* one at startup, one now */
}

static void test_capture_of_a_slave_without_support_is_rejected(void)
{
    hubus_module_t no_capture;

    standard_setup(base_yaml);
    no_capture = *dmn->slave[5].module;
    no_capture.has_status_capture = false;
    dmn->slave[5].module = &no_capture;
    say("hubus/slave/5/capture", "");
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dmn->stats.commands_rejected);
}

static void test_scan_command(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    say("hubus/master/cmd/scan", "");
    settle();
    assert_contains("hubus/master/scan", "{\"mcid\":3,\"state\":\"present\",\"configured\":true,\"device_type\":129}");
    assert_contains("hubus/master/scan", "{\"mcid\":9,\"state\":\"present\",\"configured\":false,\"device_type\":151}");
    TEST_ASSERT_NULL(strstr(last("hubus/master/scan"), "\"mcid\":4"));
}

static void test_queue_overflow_is_counted_and_harmless(void)
{
    unsigned i;

    standard_setup(base_yaml);
    for (i = 0; i < 100; i++) {
        say("hubus/slave/3/relay/set", (i & 1) ? "OFF" : "ON");
    }
    TEST_ASSERT_TRUE(dmn->stats.queue_full >= 100 - HUBUSD_QUEUE_LEN);
    settle();
    settle();
    settle();
    settle();
    settle();
    settle();
    settle();
    settle();
    say("hubus/master/cmd/stats", "");
    settle();
    assert_contains("hubus/master/stats", "\"queue_full\":");
    TEST_ASSERT_NULL(strstr(last("hubus/master/stats"), "\"queue_full\":0,"));
    TEST_ASSERT_EQUAL_UINT(0, dmn->q_count);
}

/* ---- availability ------------------------------------------------------------------------------- */

static void test_slave_goes_offline_and_comes_back(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    hubus_sim_unplug(&sim, 2, 1); /* the temperature sensor is unplugged */
    advance(30);
    assert_last("hubus/slave/7/availability", "offline");
    TEST_ASSERT_NULL(last("hubus/slave/3/availability")); /* unchanged, nothing to report */
    assert_contains("hubus/master/state", "\"online_slaves\":2");
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "slave 7 (Cellar) is offline") == 1);

    hubus_sim_unplug(&sim, 2, 0);
    dev[2].raw_temp = 311;
    advance(30);
    assert_last("hubus/slave/7/availability", "online");
    assert_last("hubus/slave/7/temperature/state", "38"); /* state refreshed after it returned */
    assert_contains("hubus/master/state", "\"online_slaves\":3");
}

static void test_a_single_missed_ping_does_not_mark_the_slave_offline(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    hubus_sim_unplug(&sim, 2, 1);
    advance(6); /* one ping interval */
    hubus_sim_unplug(&sim, 2, 0);
    advance(30);
    TEST_ASSERT_NULL(last("hubus/slave/7/availability"));
}

static void test_configured_slave_missing_at_startup_is_offline(void)
{
    load(base_yaml);
    add_dev(3, DEV_RELAY);
    add_dev(5, DEV_SWITCH); /* no slave 7 */
    start_daemon();
    settle();
    assert_last("hubus/slave/7/availability", "offline");
    assert_last("hubus/slave/3/availability", "online");
    TEST_ASSERT_NULL(last("hubus/slave/7/info"));
    TEST_ASSERT_NULL(last("homeassistant/sensor/hubus_7_temperature/config")); /* the module is unknown until the slave answers */
}

static void test_slave_appearing_later_gets_discovery(void)
{
    load(base_yaml);
    add_dev(3, DEV_RELAY);
    start_daemon();
    settle();
    TEST_ASSERT_NULL(last("homeassistant/sensor/hubus_7_temperature/config"));
    add_dev(7, DEV_TEMP);
    advance(40);
    assert_last("hubus/slave/7/availability", "online");
    assert_contains("homeassistant/sensor/hubus_7_temperature/config", "\"device_class\":\"temperature\"");
    assert_last("hubus/slave/7/temperature/state", "25");
}

/* A module with two entities: both are decoded from the same frame, each has its own topics and discovery entry. */
static void test_a_module_with_two_entities(void)
{
    dev_variant_seven = VARIANT_TWO_ENTITIES;
    standard_setup(base_yaml);
    assert_last("hubus/slave/7/temperature/state", "25");
    assert_last("hubus/slave/7/low-byte/state", "ON"); /* 298 is 0x012A */
    assert_contains("hubus/slave/7/info", "\"entities\":[\"temperature\",\"low-byte\"]");
    assert_contains("homeassistant/sensor/hubus_7_temperature/config", "\"state_topic\":\"hubus/slave/7/temperature/state\"");
    assert_contains("homeassistant/sensor/hubus_7_temperature/config", "\"name\":\"Temperature\"");
    assert_contains("homeassistant/sensor/hubus_7_temperature/config", "\"unique_id\":\"hubus_7_temperature\"");
    assert_contains("homeassistant/binary_sensor/hubus_7_low-byte/config", "\"name\":\"Low byte\"");
    assert_contains("homeassistant/binary_sensor/hubus_7_low-byte/config", "\"state_topic\":\"hubus/slave/7/low-byte/state\"");
    TEST_ASSERT_NULL(strstr(last("homeassistant/binary_sensor/hubus_7_low-byte/config"), "command_topic"));
    assert_contains("homeassistant/sensor/hubus_7_temperature/config", "\"identifiers\":[\"hubus_7\"]"); /* one device */
    assert_contains("homeassistant/binary_sensor/hubus_7_low-byte/config", "\"identifiers\":[\"hubus_7\"]");
    TEST_ASSERT_NULL(last("hubus/slave/7/state"));
}

/* A command goes to an entity: slave/<n>/<key>/set. */
static void test_commands_without_an_entity_are_rejected(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    say("hubus/slave/3/set", "ON"); /* the topic of earlier versions */
    settle();
    TEST_ASSERT_EQUAL_UINT(0, dev[0].commands);
    TEST_ASSERT_EQUAL_UINT(1, dmn->stats.commands_rejected);
    TEST_ASSERT_EQUAL_UINT(1, count_containing("hubus/master/log", "slave/3/<key>/set"));
    say("hubus/slave/3/nothing/set", "ON");
    say("hubus/slave/3/relay/relay/set", "ON"); /* not an entity either */
    say("hubus/slave/3/this-key-is-much-longer-than-an-entity-key-may-be/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT(0, dev[0].commands);
    TEST_ASSERT_EQUAL_UINT(3, dmn->stats.commands_rejected); /* "relay/relay/set" is none of our topics and is ignored */
    say("hubus/slave/3/relay/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dev[0].commands);
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
}

/* When the module of a slave changes, the discovery entries of its old entities are removed. */
static void test_discovery_of_removed_entities_goes_when_the_module_changes(void)
{
    dev_variant_seven = VARIANT_TWO_ENTITIES;
    standard_setup(base_yaml);
    TEST_ASSERT_NOT_EQUAL(0, strlen(last("homeassistant/binary_sensor/hubus_7_low-byte/config")));
    dev[2].hwid[HUBUS_HWID_VARIANT] = VARIANT_PLAIN; /* the firmware of the slave is replaced by one with one entity */
    n_msgs = 0;
    advance(40);
    assert_last("homeassistant/binary_sensor/hubus_7_low-byte/config", "");
    TEST_ASSERT_NOT_EQUAL(0, strlen(last("homeassistant/sensor/hubus_7_temperature/config")));
    assert_contains("homeassistant/sensor/hubus_7_temperature/config", "\"name\":null");
}

/* ---- statistics ------------------------------------------------------------------------------------ */

static void test_statistics_on_request(void)
{
    uint8_t pressed = 1;

    standard_setup(base_yaml);
    hubus_sim_slave_write(&sim, 1, &pressed, 1, 0);
    settle();
    say("hubus/slave/3/relay/set", "ON");
    settle();
    n_msgs = 0;
    say("hubus/master/cmd/stats", "");
    settle();
    assert_contains("hubus/master/stats", "\"bus\":\"ok\"");
    assert_contains("hubus/master/stats", "\"commands_executed\":1");
    assert_contains("hubus/master/stats", "\"commands_rejected\":0");
    TEST_ASSERT_NULL(strstr(last("hubus/master/stats"), "\"bursts\":0,"));
    assert_contains("hubus/slave/5/stats", "\"availability\":\"online\"");
    assert_contains("hubus/slave/3/stats", "\"consecutive_failures\":0");
    TEST_ASSERT_NULL(strstr(last("hubus/slave/5/stats"), "\"frames\":0,"));
}

static void test_statistics_are_published_periodically(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    advance(35);
    TEST_ASSERT_TRUE(count("hubus/master/stats") >= 3);
    TEST_ASSERT_TRUE(count("hubus/slave/3/stats") >= 3);
    TEST_ASSERT_FALSE(last_msg("hubus/master/stats")->retain);
    assert_contains("hubus/master/stats", "\"uptime_s\":");
}

static void test_statistics_can_be_switched_off(void)
{
    standard_setup("mqtt:\n  stats_interval_s: 0\nbus:\n  scan_interval_s: 0\nslaves:\n  - mcid: 3\n");
    n_msgs = 0;
    advance(100);
    TEST_ASSERT_EQUAL_UINT(0, count("hubus/master/stats"));
}

static void test_error_counters_appear_in_the_statistics(void)
{
    standard_setup(base_yaml);
    dmn->master.counters.crc_errors = 4;
    dmn->master.counters.line_errors = 2;
    n_msgs = 0;
    say("hubus/master/cmd/stats", "");
    settle();
    assert_contains("hubus/master/stats", "\"crc_errors\":4");
    assert_contains("hubus/master/stats", "\"errors_total\":6");
}

/* ---- live state (REQUEST_STATE) and individual probes ------------------------------------------- */

static void test_startup_reads_the_state_of_slaves_that_have_one(void)
{
    dev_with_state = 1;
    standard_setup(base_yaml);

    assert_contains("hubus/slave/3/info", "\"state\":true");
    assert_contains("hubus/slave/5/info", "\"state\":false");
    assert_last("hubus/slave/3/relay/state", "OFF");
    assert_last("hubus/slave/7/temperature/state", "25");
    assert_last("hubus/slave/5/button/state", "OFF");
    TEST_ASSERT_EQUAL_UINT(0, dev[0].captures); /* relay and sensor were read directly ... */
    TEST_ASSERT_EQUAL_UINT(0, dev[2].captures);
    TEST_ASSERT_EQUAL_UINT(1, dev[1].captures); /* ... the button has no state and still captures */
    TEST_ASSERT_EQUAL_UINT(2, dmn->master.counters.state_requests);
    assert_contains("hubus/slave/3/data", "\"result\":\"state\"");
}

static void test_readback_after_set_uses_one_state_read(void)
{
    dev_with_state = 1;
    standard_setup(base_yaml);
    n_msgs = 0;
    say("hubus/slave/3/relay/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
    TEST_ASSERT_EQUAL_UINT(0, dev[0].captures); /* no STATUS_CAPTURE, no burst */
    assert_last("hubus/slave/3/relay/state", "ON");
    assert_contains("hubus/slave/3/data", "\"result\":\"state\"");
    TEST_ASSERT_EQUAL_UINT(1, count("hubus/slave/3/data"));
}

static void test_published_state_is_still_the_real_state_with_a_state_read(void)
{
    dev_with_state = 1;
    standard_setup(base_yaml);
    dev[0].core.on_command = NULL; /* the relay ignores commands (defective) */
    n_msgs = 0;
    say("hubus/slave/3/relay/set", "ON");
    settle();
    assert_last("hubus/slave/3/relay/state", "OFF");
}

static void test_read_topic_reads_the_live_state_at_any_time(void)
{
    dev_with_state = 1;
    standard_setup(base_yaml);
    set_temp(&dev[2], 303);
    n_msgs = 0;
    say("hubus/slave/7/read", "");
    settle();
    assert_last("hubus/slave/7/temperature/state", "30");
    TEST_ASSERT_EQUAL_UINT(0, dev[2].captures);
    /* the event buffer is not involved: nothing pending, no burst */
    TEST_ASSERT_EQUAL_UINT(0, count("hubus/slave/5/data"));
}

static void test_read_topic_rejects_slaves_without_a_state_or_configuration(void)
{
    dev_with_state = 1;
    standard_setup(base_yaml);
    n_msgs = 0;
    say("hubus/slave/5/read", ""); /* a button without a state */
    say("hubus/slave/9/read", ""); /* not configured */
    settle();
    TEST_ASSERT_EQUAL_UINT(2, count_containing("hubus/master/log", "command rejected"));
}

static void test_state_read_does_not_disturb_a_pending_event(void)
{
    dev_with_state = 1;
    standard_setup(base_yaml);
    n_msgs = 0;
    dev[0].state = 1;
    (void)hubus_sim_slave_write(&sim, 0, (const uint8_t *)&dev[0].state, 1, 0); /* an event is pending */
    say("hubus/slave/3/read", "");
    settle();
    /* the read came first or the burst did: either way the event is delivered exactly once */
    TEST_ASSERT_FALSE(hubus_core_is_pending(&dev[0].core));
    assert_last("hubus/slave/3/relay/state", "ON");
}

static void test_probe_reports_a_single_mcid(void)
{
    dev_with_state = 1;
    standard_setup(base_yaml);
    n_msgs = 0;
    say("hubus/master/cmd/probe", "3");
    settle();
    assert_contains("hubus/master/probe", "\"mcid\":3");
    assert_contains("hubus/master/probe", "\"state\":\"present\"");
    assert_contains("hubus/master/probe", "\"configured\":true");
    assert_contains("hubus/master/probe", "\"state_capable\":true");

    say("hubus/master/cmd/probe", "9"); /* not configured */
    settle();
    assert_contains("hubus/master/probe", "\"mcid\":9");
    assert_contains("hubus/master/probe", "\"state\":\"present\"");
    assert_contains("hubus/master/probe", "\"configured\":false");

    say("hubus/master/cmd/probe", "20"); /* nobody there */
    settle();
    assert_contains("hubus/master/probe", "\"mcid\":20");
    assert_contains("hubus/master/probe", "\"state\":\"absent\"");
}

static void test_probe_of_a_duplicate_mcid_is_suspect(void)
{
    standard_setup(base_yaml);
    add_dev(11, DEV_UNKNOWN);
    add_dev(11, DEV_UNKNOWN);
    dev[n_dev - 1].hwid[HUBUS_HWID_SERIAL + 3u] = 0x99; /* different serial number: the responses cannot overlap into a valid frame */
    n_msgs = 0;
    say("hubus/master/cmd/probe", "11");
    settle();
    assert_contains("hubus/master/probe", "\"state\":\"suspect\"");
}

static void test_probe_brings_an_offline_slave_back(void)
{
    standard_setup(base_yaml);
    hubus_sim_unplug(&sim, 2, 1); /* the sensor is unplugged */
    advance(20);
    assert_last("hubus/slave/7/availability", "offline");
    hubus_sim_unplug(&sim, 2, 0);
    n_msgs = 0;
    say("hubus/master/cmd/probe", "7");
    settle();
    assert_last("hubus/slave/7/availability", "online");
    assert_contains("hubus/master/probe", "\"state\":\"present\"");
    assert_last("hubus/slave/7/temperature/state", "25"); /* its state is refreshed */
}

static void test_probe_payload_is_validated(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    say("hubus/master/cmd/probe", "0");
    say("hubus/master/cmd/probe", "64");
    say("hubus/master/cmd/probe", "abc");
    say("hubus/master/cmd/probe", "");
    settle();
    TEST_ASSERT_EQUAL_UINT(4, count_containing("hubus/master/log", "command rejected"));
    TEST_ASSERT_NULL(last("hubus/master/probe"));
}

static void test_statistics_count_state_requests(void)
{
    dev_with_state = 1;
    standard_setup(base_yaml);
    n_msgs = 0;
    say("hubus/master/cmd/stats", "");
    settle();
    assert_contains("hubus/master/stats", "\"state_requests\":2");
}

/* ---- faults ----------------------------------------------------------------------------------------- */

static void test_line_fault_is_reported_and_recovers(void)
{
    sim.extra_low = 1; /* DATA stuck low at start */
    hubus_sim_settle(&sim);
    load(base_yaml);
    add_dev(3, DEV_RELAY);
    start_daemon();
    settle();
    assert_contains("hubus/master/state", "\"bus\":\"line_fault\"");
    assert_contains("hubus/master/state", "\"error\":\"line fault");
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "bus fault") >= 1);
    assert_last("hubus/slave/3/availability", "offline"); /* not seen yet */

    /* commands during the fault are rejected instead of queued up */
    say("hubus/slave/3/relay/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dmn->stats.commands_rejected);

    sim.extra_low = 0;
    hubus_sim_settle(&sim);
    advance(5);
    assert_contains("hubus/master/state", "\"bus\":\"line_fault\""); /* retry interval not over yet */
    advance(10);
    assert_contains("hubus/master/state", "\"bus\":\"ok\"");
    assert_last("hubus/slave/3/availability", "online");
}

static void test_fault_during_operation(void)
{
    standard_setup(base_yaml);
    sim.extra_low = 1;
    hubus_sim_settle(&sim);
    say("hubus/slave/3/relay/set", "ON");
    settle();
    assert_contains("hubus/master/state", "\"bus\":\"line_fault\"");
    sim.extra_low = 0;
    hubus_sim_settle(&sim);
    advance(15);
    assert_contains("hubus/master/state", "\"bus\":\"ok\"");
    TEST_ASSERT_TRUE(dmn->stats.startups >= 2);
}

static void test_diagnosis_reports_a_duplicate_mcid(void)
{
    uint8_t v1 = 0x0F, v2 = 0xF0;

    load(base_yaml);
    add_dev(3, DEV_RELAY);
    add_dev(5, DEV_SWITCH);
    add_dev(5, DEV_SWITCH); /* configuration fault: second slave with MCID 5 */
    dev[2].hwid[HUBUS_HWID_SERIAL + 3u] = 0x99;
    start_daemon();
    settle();
    hubus_sim_slave_write(&sim, 1, &v1, 1, 0);
    hubus_sim_slave_write(&sim, 2, &v2, 1, 0);
    advance(2);
    assert_contains("hubus/master/diagnosis", "\"status\":\"suspected_duplicate_mcid\"");
    assert_contains("hubus/master/diagnosis", "\"suspects\":[5]");
    TEST_ASSERT_TRUE(last_msg("hubus/master/diagnosis")->retain);
}

static void test_publish_failures_are_counted(void)
{
    standard_setup(base_yaml);
    fail_publish = 1;
    say("hubus/master/cmd/stats", "");
    settle();
    TEST_ASSERT_TRUE(dmn->stats.publish_errors > 0);
    fail_publish = 0;
    n_msgs = 0;
    say("hubus/master/cmd/stats", "");
    settle();
    assert_contains("hubus/master/stats", "\"publish_errors\":");
    TEST_ASSERT_NULL(strstr(last("hubus/master/stats"), "\"publish_errors\":0"));
}

/* ---- reconnect, reload, shutdown ---------------------------------------------------------------------------- */

static void test_reconnect_republishes_retained_state(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    hubusd_on_connect(dmn);
    settle();
    assert_last("hubus/master/status", "online");
    assert_last("hubus/slave/3/availability", "online");
    assert_last("hubus/slave/3/relay/state", "OFF");
    TEST_ASSERT_NOT_NULL(last("hubus/slave/3/info"));
    TEST_ASSERT_NOT_NULL(last("homeassistant/switch/hubus_3_relay/config"));
    TEST_ASSERT_NOT_NULL(last("hubus/master/diagnosis"));
}

static void test_reload_adds_and_removes_slaves(void)
{
    hubusd_config_t *nc = malloc(sizeof *nc);
    char err[200];
    const char *yaml = "bus:\n  scan_interval_s: 0\n  ping_interval_s: 5\nslaves:\n  - mcid: 3\n    name: Renamed\n  - mcid: 9\n";

    standard_setup(base_yaml);
    TEST_ASSERT_NOT_NULL(nc);
    TEST_ASSERT_EQUAL_INT(0, hubusd_config_parse(yaml, strlen(yaml), nc, err, sizeof err));
    n_msgs = 0;
    hubusd_request_reload(dmn, nc);
    settle();
    advance(10);

    /* slave 5 and 7 are gone */
    assert_last("homeassistant/binary_sensor/hubus_5_button/config", "");
    assert_last("homeassistant/sensor/hubus_7_temperature/config", "");
    assert_last("hubus/slave/5/availability", "");
    assert_last("hubus/slave/7/temperature/state", "");
    /* slave 3 stays with the new name */
    assert_contains("homeassistant/switch/hubus_3_relay/config", "\"name\":\"Renamed\"");
    assert_last("hubus/slave/3/availability", "online");
    /* slave 9 has no entities: online, but only raw topics */
    assert_last("hubus/slave/9/availability", "online");
    TEST_ASSERT_NULL(last("homeassistant/switch/hubus_9_relay/config"));
    assert_contains("hubus/master/state", "\"configured_slaves\":2");
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "configuration reloaded") >= 1);
    TEST_ASSERT_EQUAL_UINT(0, strstr(last("hubus/master/state"), "\"mcid\":9") != NULL); /* no longer unknown */
}

static void test_shutdown_publishes_offline_and_resets_the_bus(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    hubusd_shutdown(dmn);
    assert_last("hubus/master/status", "offline");
    assert_last("hubus/slave/3/availability", "offline");
    assert_last("hubus/slave/7/availability", "offline");
    TEST_ASSERT_EQUAL(0, sim.clk);
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
}

static char sub_topics[9][160];
static unsigned sub_n;

static void collect_sub(void *ctx, const char *filter)
{
    (void)ctx;
    snprintf(sub_topics[sub_n++], sizeof sub_topics[0], "%s", filter);
}

static void test_subscriptions_and_status_topic(void)
{
    char status[160];

    standard_setup(base_yaml);
    sub_n = 0;
    hubusd_subscriptions(dmn, collect_sub, NULL);
    TEST_ASSERT_EQUAL_UINT(9, sub_n);
    TEST_ASSERT_EQUAL_STRING("hubus/master/cmd/+", sub_topics[0]);
    TEST_ASSERT_EQUAL_STRING("hubus/slave/+/+/set", sub_topics[1]);
    TEST_ASSERT_EQUAL_STRING("hubus/slave/+/+/rgb/set", sub_topics[2]);
    TEST_ASSERT_EQUAL_STRING("hubus/slave/+/read", sub_topics[5]);
    TEST_ASSERT_EQUAL_STRING("hubus/slave/+/update", sub_topics[6]);
    TEST_ASSERT_EQUAL_STRING("hubus/slave/+/mcid", sub_topics[7]);
    TEST_ASSERT_EQUAL_STRING("hubus/group/+/+/cmd", sub_topics[8]);
    TEST_ASSERT_EQUAL_UINT(strlen("hubus/master/status"), hubusd_status_topic(dmn, status, sizeof status));
    TEST_ASSERT_EQUAL_STRING("hubus/master/status", status);
}

/* ---- module functions ------------------------------------------------------------------------------------------ */

static const char *func_yaml =
    "mqtt:\n"
    "  stats_interval_s: 0\n"
    "bus:\n"
    "  ping_interval_s: 0\n"
    "  scan_interval_s: 0\n"
    "  readback: none\n"
    "modules:\n"
    "  call_timeout_ms: 100\n"
    "slaves:\n"
    "  - mcid: 4\n"
    "    name: Func\n"
    "  - mcid: 6\n"
    "    name: Second\n";

static sim_dev_t *func_dev;

static void func_setup(void)
{
    load(func_yaml);
    func_dev = add_dev(4, DEV_FUNC);
    add_dev(6, DEV_FUNC);
    start_daemon();
    settle();
}

static void sleep_ms(unsigned ms)
{
    struct timespec t = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};

    nanosleep(&t, NULL);
}

/* The worker runs on its own thread with the real clock: steps until the test sees what it waits for. */
#define PUMP_UNTIL(condition)                                  \
    do {                                                       \
        unsigned pump_;                                        \
        for (pump_ = 0; pump_ < 3000u && !(condition); pump_++) { \
            settle();                                          \
            sleep_ms(1);                                       \
        }                                                      \
        settle();                                              \
        TEST_ASSERT_TRUE_MESSAGE((condition), #condition);     \
    } while (0)

static unsigned func_read(const unsigned *value)
{
    unsigned v;

    pthread_mutex_lock(&func_lock);
    v = *value;
    pthread_mutex_unlock(&func_lock);
    return v;
}

static void func_set_hang(int on)
{
    pthread_mutex_lock(&func_lock);
    func_hang = on;
    pthread_cond_broadcast(&func_changed);
    pthread_mutex_unlock(&func_lock);
}

static void test_functions_get_one_instance_per_slave_and_one_thread_per_module(void)
{
    hubusd_config_t *nc = malloc(sizeof *nc);
    char err[200];
    const char *yaml = "bus:\n  scan_interval_s: 0\n  ping_interval_s: 0\nslaves:\n  - mcid: 6\n";

    func_setup();
    PUMP_UNTIL(func_read(&func_live) == 2u);
    TEST_ASSERT_EQUAL_UINT(2, func_created);

    TEST_ASSERT_NOT_NULL(nc);
    TEST_ASSERT_EQUAL_INT(0, hubusd_config_parse(yaml, strlen(yaml), nc, err, sizeof err));
    hubusd_request_reload(dmn, nc); /* slave 4 goes away, slave 6 keeps its instance */
    settle();
    PUMP_UNTIL(func_read(&func_destroyed) == 1u);
    TEST_ASSERT_EQUAL_UINT(1, func_live);
    TEST_ASSERT_EQUAL_UINT(2, func_created);

    hubusd_shutdown(dmn);
    TEST_ASSERT_EQUAL_UINT(0, func_live);
    TEST_ASSERT_EQUAL_UINT(2, func_destroyed);
}

static void test_light_on_off_and_colour_go_through_on_set(void)
{
    func_setup();
    PUMP_UNTIL(func_read(&func_live) == 2u);
    n_msgs = 0;

    say("hubus/slave/4/lamp/set", "on");
    PUMP_UNTIL(func_dev->last_op == OP_FUNC_POWER);
    TEST_ASSERT_EQUAL_UINT8(1, func_dev->last_args[0]);
    assert_last("hubus/slave/4/lamp/state", "ON");

    say("hubus/slave/4/lamp/rgb/set", " 10, 20 ,30 ");
    PUMP_UNTIL(func_dev->last_op == OP_FUNC_COLOUR);
    TEST_ASSERT_EQUAL_UINT8(3, func_dev->last_len);
    TEST_ASSERT_EQUAL_UINT8(10, func_dev->last_args[0]);
    TEST_ASSERT_EQUAL_UINT8(20, func_dev->last_args[1]);
    TEST_ASSERT_EQUAL_UINT8(30, func_dev->last_args[2]);

    say("hubus/slave/4/fan/set", "OFF");
    PUMP_UNTIL(func_dev->last_op == OP_FUNC_POWER);
    TEST_ASSERT_EQUAL_UINT8(0, func_dev->last_args[0]);
    assert_last("hubus/slave/4/fan/state", "OFF");
}

static void test_values_that_cannot_be_used_are_rejected(void)
{
    func_setup();
    PUMP_UNTIL(func_read(&func_live) == 2u);
    n_msgs = 0;

    say("hubus/slave/4/lamp/rgb/set", "red");
    say("hubus/slave/4/lamp/rgb/set", "1,2");
    say("hubus/slave/4/lamp/rgb/set", "256,0,0");
    say("hubus/slave/4/lamp/set", "bright");
    say("hubus/slave/4/fan/rgb/set", "1,2,3");
    say("hubus/slave/4/first/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT(6, dmn->stats.commands_rejected);
    TEST_ASSERT_EQUAL_UINT(0, func_dev->commands);

    say("hubus/slave/4/lamp/rgb/set", "1,2,3"); /* the function does not take it */
    PUMP_UNTIL(dmn->stats.commands_rejected == 7u);
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "rejected the value") >= 1);
}

static void test_a_fixed_command_needs_no_function(void)
{
    func_setup();
    PUMP_UNTIL(func_read(&func_live) == 2u);
    say("hubus/slave/4/relay/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_HEX8(0x10, func_dev->last_op);
}

static void test_frames_reach_on_frame_with_kind_and_gap(void)
{
    uint8_t b = 0x5A;

    func_setup();
    PUMP_UNTIL(func_read(&func_live) == 2u);
    TEST_ASSERT_EQUAL_INT(HUBUS_WRITE_OK, hubus_sim_slave_write(&sim, 0, &b, 1, 0));
    PUMP_UNTIL(func_read(&func_frame_byte) == 0x5Au);
    TEST_ASSERT_EQUAL(HUBUS_FRAME_EVENT, func_frame_kind);
    TEST_ASSERT_FALSE(func_frame_gap);
    TEST_ASSERT_NOT_NULL(last("hubus/slave/4/first/state")); /* the data of a module is decoded as ever */
}

static void test_a_long_series_of_commands_is_not_lost(void)
{
    func_setup();
    PUMP_UNTIL(func_read(&func_live) == 2u);
    say("hubus/slave/4/burst/set", "ON");
    PUMP_UNTIL(func_dev->commands == FUNC_BURST_COMMANDS);
    TEST_ASSERT_EQUAL_UINT8(FUNC_BURST_COMMANDS - 1u, func_dev->last_args[0]);
    TEST_ASSERT_EQUAL_UINT(0, dmn->stats.queue_full);
}

static void test_a_function_that_hangs_stops_only_its_module(void)
{
    func_setup();
    PUMP_UNTIL(func_read(&func_live) == 2u);
    func_set_hang(1);
    say("hubus/slave/4/lamp/set", "ON");
    PUMP_UNTIL(func_read(&func_hanging) == 1u);
    clock_ms += 50;
    settle();
    TEST_ASSERT_EQUAL_UINT(0, count_containing("hubus/slave/4/info", "\"module_failed\":true"));
    clock_ms += 100;
    settle();
    assert_contains("hubus/slave/4/info", "\"module_failed\":true");
    assert_contains("hubus/slave/6/info", "\"module_failed\":true");
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "did not return within 100 ms") >= 1);

    /* no more calls, but everything that is data still works */
    n_msgs = 0;
    say("hubus/slave/4/lamp/set", "OFF");
    settle();
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "has failed") >= 1);
    say("hubus/slave/4/relay/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_HEX8(0x10, func_dev->last_op);

    func_set_hang(0);
    hubusd_shutdown(dmn);
    TEST_ASSERT_EQUAL_UINT(0, func_destroyed); /* a failed module gets no more calls */
}

static void test_create_that_fails_is_logged_and_set_is_rejected(void)
{
    func_create_fails = 1;
    func_setup();
    PUMP_UNTIL(count_containing("hubus/master/log", "could not create") >= 2u);
    say("hubus/slave/4/lamp/set", "ON");
    PUMP_UNTIL(count_containing("hubus/master/log", "has no instance") >= 1u);
    TEST_ASSERT_EQUAL_UINT(0, func_dev->commands);
}

static void test_discovery_of_lights_and_switches_without_commands(void)
{
    func_setup();
    assert_contains("homeassistant/light/hubus_4_lamp/config", "\"command_topic\":\"hubus/slave/4/lamp/set\"");
    assert_contains("homeassistant/light/hubus_4_lamp/config", "\"rgb_command_topic\":\"hubus/slave/4/lamp/rgb/set\"");
    assert_contains("homeassistant/light/hubus_4_lamp/config", "\"state_topic\":\"hubus/slave/4/lamp/state\"");
    TEST_ASSERT_NULL(strstr(last("homeassistant/light/hubus_4_lamp/config"), "state_on"));
    assert_contains("homeassistant/switch/hubus_4_fan/config", "\"command_topic\":\"hubus/slave/4/fan/set\"");
    assert_contains("hubus/slave/4/info", "\"module_failed\":false");
}

/* ---- the module of the LED strip -------------------------------------------------------------------------------- */

#define OP_FILL 0x41u
#define OP_SHOW 0x42u
#define OP_PROG_WRITE 0x43u
#define OP_PROG_RUN 0x44u

static sim_dev_t *strip;

static void strip_setup(void)
{
    static const hubus_module_t *const modules[] = {&hubusd_ledstrip_module};

    load("mqtt:\n  stats_interval_s: 0\nbus:\n  ping_interval_s: 0\n  scan_interval_s: 0\n  readback: none\nslaves:\n  - mcid: 3\n");
    dev_force_variant = 1;
    strip = add_dev(3, 0x84);
    start_daemon();
    hubusd_set_modules(dmn, modules, 1);
    settle();
    PUMP_UNTIL(last("hubus/slave/3/info") != NULL);
}

/* Steps until the slave has got `n` commands. A SHOW makes it blind for 4 ms, so the clock has to go on. */
static void strip_until_commands(unsigned n)
{
    unsigned k;

    for (k = 0; k < 2000u && strip->commands < n; k++) {
        clock_ms += 5;
        settle();
        sleep_ms(1);
    }
    TEST_ASSERT_EQUAL_UINT(n, strip->commands);
}

static void test_the_strip_offers_a_lamp_an_animation_and_a_button(void)
{
    strip_setup();
    assert_contains("homeassistant/light/hubus_3_lamp/config", "\"rgb_command_topic\":\"hubus/slave/3/lamp/rgb/set\"");
    assert_contains("homeassistant/text/hubus_3_animation/config", "\"command_topic\":\"hubus/slave/3/animation/set\"");
    assert_contains("homeassistant/text/hubus_3_animation/config", "\"max\":95");
    assert_contains("homeassistant/binary_sensor/hubus_3_button/config", "\"state_topic\":\"hubus/slave/3/button/state\"");
}

static void test_the_lamp_fills_the_strip_with_its_colour(void)
{
    strip_setup();
    say("hubus/slave/3/lamp/rgb/set", "255,10,20");
    strip_until_commands(2);
    TEST_ASSERT_EQUAL_HEX8(OP_FILL, strip->ops[0]);
    TEST_ASSERT_EQUAL_HEX8(OP_SHOW, strip->ops[1]);
    TEST_ASSERT_EQUAL_UINT8(1, strip->last_args[0]); /* seq */
    PUMP_UNTIL(last("hubus/slave/3/lamp/state") != NULL);
    assert_last("hubus/slave/3/lamp/state", "ON");

    say("hubus/slave/3/lamp/set", "ON"); /* Home Assistant sends this after the colour */
    clock_ms += 5;
    settle();
    sleep_ms(5);
    settle();
    TEST_ASSERT_EQUAL_UINT(2, strip->commands);

    say("hubus/slave/3/lamp/set", "OFF");
    strip_until_commands(4);
    TEST_ASSERT_EQUAL_UINT8(2, strip->last_args[0]);
    PUMP_UNTIL(strcmp(last("hubus/slave/3/lamp/state"), "OFF") == 0);
}

static void test_a_formula_is_compiled_and_sent_as_a_program(void)
{
    static const char *formula = "hsv(i * 4 + (t >> 3), 255, sin8((t >> 2) + i * 16))";
    uint8_t code[LEDVM_CODE_MAX];
    uint8_t len;

    strip_setup();
    say("hubus/slave/3/animation/set", formula);
    strip_until_commands(2);
    len = hubusd_ledc_compile(formula, code, NULL, 0);
    TEST_ASSERT_EQUAL_HEX8(OP_PROG_WRITE, strip->ops[0]);
    TEST_ASSERT_EQUAL_HEX8(OP_PROG_RUN, strip->ops[1]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(code, strip->code, len);
    TEST_ASSERT_EQUAL_UINT8(4, strip->last_len);
    TEST_ASSERT_EQUAL_UINT8(len, strip->last_args[0]);
    TEST_ASSERT_EQUAL_UINT8(hubus_crc8(code, len), strip->last_args[1]);
    TEST_ASSERT_EQUAL_UINT8(LEDVM_LAYOUT_GRB, strip->last_args[2]);
    TEST_ASSERT_TRUE(strip->last_args[3] > 0u);
    PUMP_UNTIL(last("hubus/slave/3/animation/state") != NULL);
    assert_last("hubus/slave/3/animation/state", formula);
    assert_last("hubus/slave/3/lamp/state", "ON");
}

static void test_a_long_program_goes_in_pieces(void)
{
    /* more than the 29 bytes one PROG_WRITE takes */
    static const char *formula = "v0 = v0 + 1; v1 = v1 + 2; v2 = v2 + 3; v3 = v3 + 4; rgb(v0 + v1 + v2, v3 + i, t + n)";
    uint8_t code[LEDVM_CODE_MAX];
    uint8_t len = hubusd_ledc_compile(formula, code, NULL, 0);

    TEST_ASSERT_TRUE(len > 29u);
    strip_setup();
    say("hubus/slave/3/animation/set", formula);
    strip_until_commands(3);
    TEST_ASSERT_EQUAL_HEX8(OP_PROG_WRITE, strip->ops[0]);
    TEST_ASSERT_EQUAL_HEX8(OP_PROG_WRITE, strip->ops[1]);
    TEST_ASSERT_EQUAL_HEX8(OP_PROG_RUN, strip->ops[2]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(code, strip->code, len);
}

static void test_a_formula_that_is_wrong_is_rejected_with_the_reason(void)
{
    strip_setup();
    n_msgs = 0;
    say("hubus/slave/3/animation/set", "rgb(1, 2, foo)");
    PUMP_UNTIL(count_containing("hubus/master/log", "unknown name 'foo'") >= 1u);
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "rejected the value") >= 1);
    TEST_ASSERT_EQUAL_UINT(0, strip->commands);
    TEST_ASSERT_NULL(last("hubus/slave/3/animation/state"));
}

static void test_the_lamp_ends_an_animation_and_none_returns_to_the_lamp(void)
{
    strip_setup();
    say("hubus/slave/3/lamp/rgb/set", "1,2,3");
    strip_until_commands(2);
    say("hubus/slave/3/animation/set", "rgb(i, 0, 0)");
    strip_until_commands(4);
    TEST_ASSERT_EQUAL_HEX8(OP_PROG_RUN, strip->last_op);

    say("hubus/slave/3/lamp/set", "ON"); /* the same colour, but the strip shows the animation */
    strip_until_commands(6);
    TEST_ASSERT_EQUAL_HEX8(OP_SHOW, strip->last_op);
    PUMP_UNTIL(strcmp(last("hubus/slave/3/animation/state"), "none") == 0);

    say("hubus/slave/3/animation/set", "rgb(i, 0, 0)");
    strip_until_commands(8);
    say("hubus/slave/3/animation/set", "");
    strip_until_commands(10);
    TEST_ASSERT_EQUAL_HEX8(OP_SHOW, strip->last_op);
}

static void test_after_a_power_up_the_strip_gets_back_what_it_showed(void)
{
    uint8_t report[3] = {0, 0, 0}; /* no SHOW, no program: what the slave reports after a power-up */

    strip_setup();
    say("hubus/slave/3/animation/set", "rgb(i, 0, 0)");
    strip_until_commands(2);
    TEST_ASSERT_EQUAL_INT(HUBUS_WRITE_OK, hubus_sim_slave_write(&sim, 0, report, 3, 0));
    strip_until_commands(4);
    TEST_ASSERT_EQUAL_HEX8(OP_PROG_RUN, strip->last_op);

    say("hubus/slave/3/lamp/rgb/set", "1,2,3");
    strip_until_commands(6);
    TEST_ASSERT_EQUAL_INT(HUBUS_WRITE_OK, hubus_sim_slave_write(&sim, 0, report, 3, 0));
    strip_until_commands(8);
    TEST_ASSERT_EQUAL_HEX8(OP_SHOW, strip->last_op);
}

static void test_a_refused_program_is_reported(void)
{
    uint8_t report[3] = {0, 0, 2}; /* PROG_REFUSED */

    strip_setup();
    say("hubus/slave/3/animation/set", "rgb(i, 0, 0)");
    strip_until_commands(2);
    TEST_ASSERT_EQUAL_INT(HUBUS_WRITE_OK, hubus_sim_slave_write(&sim, 0, report, 3, 0));
    PUMP_UNTIL(strcmp(last("hubus/slave/3/animation/state"), "refused by the slave") == 0);
}

/* ---- replacing the modules (SIGHUP) ------------------------------------------------------------------------------ */

VARIANT_RULE(func_next, DEV_FUNC, VARIANT_PLAIN);
static const hubus_entity_t func_next_entities[] = {{.key = "lamp", .name = "Lamp", .component = HUBUS_COMPONENT_LIGHT}};
DEVICE_MODULE(func_next, false, FUNCTIONS, ENTITIES(func_next_entities));

static void test_replacing_a_module_makes_new_instances_and_entities(void)
{
    static const hubus_module_t *const next[] = {&func_next};

    func_setup();
    PUMP_UNTIL(func_read(&func_live) == 2u);
    assert_contains("homeassistant/switch/hubus_4_fan/config", "\"name\"");
    say("hubus/slave/4/lamp/set", "ON");
    PUMP_UNTIL(last("hubus/slave/4/lamp/state") != NULL);
    n_msgs = 0;

    TEST_ASSERT_NULL(hubusd_modules_missing(dmn, &cfg, next, 1));
    hubusd_replace_modules(dmn, next, 1);
    TEST_ASSERT_EQUAL_UINT(2, func_read(&func_destroyed)); /* the thread of the old module has ended */
    PUMP_UNTIL(func_read(&func_created) == 4u);
    TEST_ASSERT_EQUAL_UINT(2, func_read(&func_live));

    TEST_ASSERT_EQUAL_PTR(&func_next, dmn->slave[4].module);
    assert_contains("hubus/slave/4/info", "\"module\":\"func_next\"");
    assert_last("homeassistant/switch/hubus_4_fan/config", ""); /* gone with the old module */
    assert_last("hubus/slave/4/fan/state", "");
    assert_contains("homeassistant/light/hubus_4_lamp/config", "\"model\":\"func_next\"");
    TEST_ASSERT_EQUAL_STRING("", dmn->slave[4].last_state[0]);

    say("hubus/slave/4/lamp/set", "OFF");
    PUMP_UNTIL(func_dev->last_op == OP_FUNC_POWER && func_dev->last_args[0] == 0u);
}

static void test_replacing_with_the_same_module_changes_nothing(void)
{
    func_setup();
    PUMP_UNTIL(func_read(&func_live) == 2u);
    n_msgs = 0;
    hubusd_replace_modules(dmn, test_modules, sizeof test_modules / sizeof test_modules[0]);
    settle();
    sleep_ms(5);
    settle();
    TEST_ASSERT_EQUAL_UINT(2, func_read(&func_created));
    TEST_ASSERT_EQUAL_UINT(0, func_read(&func_destroyed));
    TEST_ASSERT_NULL(last("hubus/slave/4/info"));
}

static void test_modules_a_slave_needs_are_not_given_up(void)
{
    static const hubus_module_t *const others[] = {&relay_plain};
    hubusd_config_t none;

    func_setup();
    PUMP_UNTIL(func_read(&func_live) == 2u);
    TEST_ASSERT_EQUAL_STRING("func_module", hubusd_modules_missing(dmn, &cfg, others, 1));
    TEST_ASSERT_NULL(hubusd_modules_missing(dmn, &cfg, test_modules, sizeof test_modules / sizeof test_modules[0]));
    /* if the slave is not in the new configuration, nothing needs its module */
    none = cfg;
    none.n_slaves = 0;
    TEST_ASSERT_NULL(hubusd_modules_missing(dmn, &none, others, 1));
}

/* A new file with the content of the plugin under this name, as `install` and `mv` make it. */
static void install_plugin(const char *from, const char *to)
{
    static char buf[1 << 20];
    char tmp[300];
    size_t n;
    FILE *in = fopen(from, "rb");
    FILE *out;

    TEST_ASSERT_NOT_NULL(in);
    snprintf(tmp, sizeof tmp, "%s.new", to);
    out = fopen(tmp, "wb");
    TEST_ASSERT_NOT_NULL(out);
    n = fread(buf, 1, sizeof buf, in);
    TEST_ASSERT_EQUAL_size_t(n, fwrite(buf, 1, n, out));
    fclose(in);
    fclose(out);
    TEST_ASSERT_EQUAL_INT(0, chmod(tmp, 0755));
    TEST_ASSERT_EQUAL_INT(0, rename(tmp, to));
}

static void test_a_reload_reads_the_plugins_anew(void)
{
    char dir[] = "/tmp/hubus_reload_XXXXXX";
    char path[200], yaml[300];
    hubus_module_set_t set;
    const hubus_module_t *first, *second;

    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/good.so", dir);
    install_plugin(HUBUS_TEST_PLUGIN_DIR "/a_good.so", path);
    snprintf(yaml, sizeof yaml,
             "mqtt:\n  stats_interval_s: 0\nbus:\n  ping_interval_s: 0\n  scan_interval_s: 0\nmodules:\n  dir: %s\nslaves:\n  - mcid: 3\n", dir);
    load(yaml);
    hubus_module_set_init(&set);
    TEST_ASSERT_EQUAL_UINT(1, hubusd_load_modules(&cfg, &set, NULL, NULL));
    dev_force_variant = 1;
    add_dev(3, 0x90);
    start_daemon();
    hubusd_set_modules(dmn, set.module, set.n);
    settle();
    assert_contains("hubus/slave/3/info", "\"module\":\"plugin_good\"");
    first = dmn->slave[3].module;
    TEST_ASSERT_NOT_NULL(first);

    /* the file is still the same: so is the module */
    n_msgs = 0;
    TEST_ASSERT_EQUAL_INT(1, hubusd_reload_modules(dmn, &set, &cfg, NULL, NULL));
    TEST_ASSERT_EQUAL_PTR(first, dmn->slave[3].module);
    TEST_ASSERT_NULL(last("hubus/slave/3/info"));

    /* a new file: a new module under the old name */
    install_plugin(HUBUS_TEST_PLUGIN_DIR "/a_good.so", path);
    TEST_ASSERT_EQUAL_INT(1, hubusd_reload_modules(dmn, &set, &cfg, NULL, NULL));
    second = dmn->slave[3].module;
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_TRUE(second != first);
    assert_contains("hubus/slave/3/info", "\"module\":\"plugin_good\"");
    assert_contains("homeassistant/sensor/hubus_3_temperature/config", "\"model\":\"plugin_good\"");
    TEST_ASSERT_EQUAL_STRING("plugin_good", second->name);

    /* the file is gone but the slave needs it: everything stays */
    n_msgs = 0;
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    TEST_ASSERT_EQUAL_INT(0, hubusd_reload_modules(dmn, &set, &cfg, NULL, NULL));
    TEST_ASSERT_EQUAL_PTR(second, dmn->slave[3].module);
    TEST_ASSERT_EQUAL_STRING("plugin_good", dmn->slave[3].module->name);
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "modules not reloaded") >= 1);

    hubusd_destroy(dmn);
    free(dmn);
    dmn = NULL;
    hubusd_unload_modules(&set);
    TEST_ASSERT_EQUAL_INT(0, rmdir(dir));
}

static void test_empty_payloads_arrive_as_null(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    hubusd_handle_message(dmn, "hubus/slave/7/capture", NULL, 0);
    hubusd_handle_message(dmn, "hubus/master/cmd/scan", NULL, 0);
    hubusd_handle_message(dmn, "hubus/master/cmd/stats", NULL, 0);
    hubusd_handle_message(dmn, "hubus/slave/3/relay/set", NULL, 0); /* empty command: rejected */
    settle();
    TEST_ASSERT_NOT_NULL(last("hubus/master/stats"));
    TEST_ASSERT_NOT_NULL(last("hubus/master/scan"));
    TEST_ASSERT_EQUAL_UINT(1, dmn->stats.commands_rejected);
    TEST_ASSERT_EQUAL_UINT(2, dev[2].captures);
}

/* ---- threading ------------------------------------------------------------------------------------------------------ */

static void *hammer(void *arg)
{
    unsigned i;

    (void)arg;
    for (i = 0; i < 2000; i++) {
        say("hubus/slave/3/relay/set", (i & 1) ? "OFF" : "ON");
        say("hubus/slave/3/cmd", "0x10 1");
        say("hubus/master/cmd/stats", "");
        if (i % 100 == 0) {
            hubusd_on_connect(dmn);
        }
    }
    return NULL;
}

static void test_messages_from_another_thread(void)
{
    pthread_t th;
    unsigned i;

    standard_setup(base_yaml);
    pthread_create(&th, NULL, hammer, NULL);
    for (i = 0; i < 200; i++) {
        hubusd_step(dmn);
    }
    pthread_join(th, NULL);
    for (i = 0; i < 100; i++) {
        hubusd_step(dmn);
    }
    TEST_ASSERT_TRUE(dmn->stats.commands_executed > 0);
    TEST_ASSERT_TRUE(dmn->stats.commands_executed + dmn->stats.queue_full + dmn->stats.commands_rejected > 0);
}


/* ---- blind time after a command ------------------------------------------------------------------------------------ */

/* The module states the blind time per opcode; the slave keeps quiet after every command, so even an opcode without a
 * blind time needs the RESET or transaction that ends this phase. */
static void test_the_blind_time_depends_on_the_opcode(void)
{
    dev_with_state = 1;
    dev_blind_mcid = 3;
    dev_blind_op = OP_BLIND_ONLY;
    dev_force_variant = VARIANT_BLIND_SHOW_ONLY;
    standard_setup(base_yaml);

    say("hubus/slave/3/cmd", "0x10 1");
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dev[0].commands);
    TEST_ASSERT_EQUAL_UINT64(clock_ms + 1u, dmn->slave[3].blind_until_ms); /* not blind, but quiet: one tick */
    clock_ms += 2u;
    settle();
    TEST_ASSERT_EQUAL_UINT64(0, dmn->slave[3].blind_until_ms);

    say("hubus/slave/3/cmd", "0x42");
    settle();
    TEST_ASSERT_EQUAL_UINT(2, dev[0].commands);
    TEST_ASSERT_EQUAL_UINT64(clock_ms + 5u, dmn->slave[3].blind_until_ms); /* 4 ms and one tick */
    TEST_ASSERT_EQUAL_UINT8(2, dev[0].blind_phase);
    clock_ms += 5u;
    settle();
    TEST_ASSERT_EQUAL_UINT64(0, dmn->slave[3].blind_until_ms);
}


static void test_readback_waits_for_the_blind_time_and_a_reset_ends_it(void)
{
    unsigned resets;

    dev_with_state = 1;
    dev_blind_mcid = 3;
    standard_setup(base_yaml);
    assert_contains("hubus/slave/3/info", "\"blind\":true");
    assert_contains("hubus/slave/5/info", "\"blind\":false");
    n_msgs = 0;
    say("hubus/slave/3/relay/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
    TEST_ASSERT_EQUAL_UINT8(2, dev[0].blind_phase);
    TEST_ASSERT_NULL(last("hubus/slave/3/relay/state")); /* the readback waits */
    TEST_ASSERT_TRUE(dmn->slave[3].blind_until_ms > clock_ms);

    say("hubus/slave/3/relay/set", "OFF"); /* a second command waits as well */
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dev[0].commands);
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);

    resets = sim.n_reset;
    clock_ms += 6; /* class 5: 4 ms, plus rounding and one tick */
    settle();
    TEST_ASSERT_TRUE(sim.n_reset > resets);
    TEST_ASSERT_EQUAL_UINT(1, count_containing("hubus/slave/3/relay/state", "ON")); /* read back after the blind time */
    TEST_ASSERT_EQUAL_UINT(2, dev[0].commands);                               /* then the queued command */
    TEST_ASSERT_EQUAL_UINT8(0, dev[0].state);

    clock_ms += 6;
    settle();
    assert_last("hubus/slave/3/relay/state", "OFF");
    TEST_ASSERT_EQUAL_UINT(0, dmn->slave[3].blind_until_ms);
    TEST_ASSERT_EQUAL_UINT(1, dmn->slave[3].online);
}

static void test_other_slaves_are_served_while_one_is_blind(void)
{
    dev_with_state = 1;
    dev_blind_mcid = 3;
    standard_setup(base_yaml);
    n_msgs = 0;
    say("hubus/slave/3/relay/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT8(2, dev[0].blind_phase);

    dev[1].state = 1; /* the button is pressed */
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&sim, 1, &dev[1].state, 1, 0));
    say("hubus/slave/7/read", "");
    settle();
    assert_last("hubus/slave/5/button/state", "ON");
    TEST_ASSERT_NOT_NULL(last("hubus/slave/7/temperature/state"));
    TEST_ASSERT_EQUAL_UINT8(2, dev[0].blind_phase); /* all of it while the relay was still blind */

    clock_ms += 6;
    settle();
    assert_last("hubus/slave/3/relay/state", "ON");
}

static void test_pings_and_scans_skip_a_blind_slave(void)
{
    dev_blind_mcid = 3;
    standard_setup("bus:\n"
                   "  ping_interval_s: 1\n"
                   "  ping_failures: 1\n"
                   "  scan_interval_s: 1\n"
                   "slaves:\n"
                   "  - mcid: 3\n"
                   "    name: Relay\n"
                   "    readback: none\n"); /* a capture as readback would be a command again */
    clock_ms += 6; /* the status capture at startup is a command too */
    settle();
    say("hubus/slave/3/cmd", "0x10 1");
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dev[0].commands);
    TEST_ASSERT_EQUAL_UINT8(2, dev[0].blind_phase);
    dmn->slave[3].next_ping_ms = clock_ms; /* ping and scan are due, but the relay is blind */
    dmn->next_scan_ms = clock_ms;
    settle();
    TEST_ASSERT_EQUAL_INT(1, dmn->slave[3].online); /* a ping now would have gone unanswered */
    TEST_ASSERT_EQUAL_UINT64(clock_ms, dmn->slave[3].next_ping_ms);
    TEST_ASSERT_EQUAL_UINT64(clock_ms, dmn->next_scan_ms);
    clock_ms += 6;
    settle();
    TEST_ASSERT_EQUAL_UINT64(0, dmn->slave[3].blind_until_ms);
    TEST_ASSERT_TRUE(dmn->slave[3].next_ping_ms > clock_ms); /* pinged and scanned after the blind time */
    TEST_ASSERT_TRUE(dmn->next_scan_ms > clock_ms);
    TEST_ASSERT_EQUAL_INT(1, dmn->slave[3].online);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_startup_publishes_state_info_availability_and_discovery);
    RUN_TEST(test_unknown_slave_is_reported_in_master_state_only);
    RUN_TEST(test_discovery_messages);
    RUN_TEST(test_discovery_can_be_disabled);
    RUN_TEST(test_discovery_cleanup_clears_unconfigured_mcids);
    RUN_TEST(test_custom_base_topic_and_prefix);
    RUN_TEST(test_button_press_is_published);
    RUN_TEST(test_data_from_an_unconfigured_slave_is_ignored);
    RUN_TEST(test_overwritten_events_are_flagged);
    RUN_TEST(test_set_switches_the_relay_and_publishes_the_read_back_state);
    RUN_TEST(test_published_state_is_the_real_state_not_the_command);
    RUN_TEST(test_readback_can_be_disabled);
    RUN_TEST(test_cache_readback_reports_the_command_without_contacting_the_slave);
    RUN_TEST(test_cache_readback_does_not_know_about_a_defective_slave);
    RUN_TEST(test_cache_readback_leaves_raw_commands_alone);
    RUN_TEST(test_slave_that_cannot_be_read_back_reports_the_cached_state);
    RUN_TEST(test_the_cached_state_belongs_to_the_entity_that_was_switched);
    RUN_TEST(test_poll_interval_reads_the_state_periodically);
    RUN_TEST(test_poll_interval_captures_a_slave_without_a_state);
    RUN_TEST(test_readback_mode_per_slave_overrides_the_bus_default);
    RUN_TEST(test_invalid_set_commands_are_rejected);
    RUN_TEST(test_malformed_topics_and_payloads_are_rejected);
    RUN_TEST(test_unrelated_topics_are_ignored);
    RUN_TEST(test_oversized_payload_is_rejected);
    RUN_TEST(test_set_for_an_entity_the_module_does_not_have_is_rejected);
    RUN_TEST(test_a_module_with_two_entities);
    RUN_TEST(test_commands_without_an_entity_are_rejected);
    RUN_TEST(test_discovery_of_removed_entities_goes_when_the_module_changes);
    RUN_TEST(test_a_slave_without_a_module_gets_no_commands);
    RUN_TEST(test_raw_command_exact);
    RUN_TEST(test_the_blind_time_depends_on_the_opcode);
    RUN_TEST(test_raw_command_larger_than_the_slave_accepts_is_rejected);
    RUN_TEST(test_group_and_broadcast_commands);
    RUN_TEST(test_group_status_capture_collects_states);
    RUN_TEST(test_capture_topic);
    RUN_TEST(test_capture_of_a_slave_without_support_is_rejected);
    RUN_TEST(test_scan_command);
    RUN_TEST(test_startup_reads_the_state_of_slaves_that_have_one);
    RUN_TEST(test_readback_after_set_uses_one_state_read);
    RUN_TEST(test_readback_waits_for_the_blind_time_and_a_reset_ends_it);
    RUN_TEST(test_other_slaves_are_served_while_one_is_blind);
    RUN_TEST(test_pings_and_scans_skip_a_blind_slave);
    RUN_TEST(test_published_state_is_still_the_real_state_with_a_state_read);
    RUN_TEST(test_read_topic_reads_the_live_state_at_any_time);
    RUN_TEST(test_read_topic_rejects_slaves_without_a_state_or_configuration);
    RUN_TEST(test_state_read_does_not_disturb_a_pending_event);
    RUN_TEST(test_probe_reports_a_single_mcid);
    RUN_TEST(test_probe_of_a_duplicate_mcid_is_suspect);
    RUN_TEST(test_probe_brings_an_offline_slave_back);
    RUN_TEST(test_probe_payload_is_validated);
    RUN_TEST(test_statistics_count_state_requests);
    RUN_TEST(test_queue_overflow_is_counted_and_harmless);
    RUN_TEST(test_slave_goes_offline_and_comes_back);
    RUN_TEST(test_a_single_missed_ping_does_not_mark_the_slave_offline);
    RUN_TEST(test_configured_slave_missing_at_startup_is_offline);
    RUN_TEST(test_slave_appearing_later_gets_discovery);
    RUN_TEST(test_statistics_on_request);
    RUN_TEST(test_statistics_are_published_periodically);
    RUN_TEST(test_statistics_can_be_switched_off);
    RUN_TEST(test_error_counters_appear_in_the_statistics);
    RUN_TEST(test_line_fault_is_reported_and_recovers);
    RUN_TEST(test_fault_during_operation);
    RUN_TEST(test_diagnosis_reports_a_duplicate_mcid);
    RUN_TEST(test_publish_failures_are_counted);
    RUN_TEST(test_reconnect_republishes_retained_state);
    RUN_TEST(test_reload_adds_and_removes_slaves);
    RUN_TEST(test_shutdown_publishes_offline_and_resets_the_bus);
    RUN_TEST(test_subscriptions_and_status_topic);
    RUN_TEST(test_functions_get_one_instance_per_slave_and_one_thread_per_module);
    RUN_TEST(test_light_on_off_and_colour_go_through_on_set);
    RUN_TEST(test_values_that_cannot_be_used_are_rejected);
    RUN_TEST(test_a_fixed_command_needs_no_function);
    RUN_TEST(test_frames_reach_on_frame_with_kind_and_gap);
    RUN_TEST(test_a_long_series_of_commands_is_not_lost);
    RUN_TEST(test_a_function_that_hangs_stops_only_its_module);
    RUN_TEST(test_create_that_fails_is_logged_and_set_is_rejected);
    RUN_TEST(test_discovery_of_lights_and_switches_without_commands);
    RUN_TEST(test_the_strip_offers_a_lamp_an_animation_and_a_button);
    RUN_TEST(test_the_lamp_fills_the_strip_with_its_colour);
    RUN_TEST(test_a_formula_is_compiled_and_sent_as_a_program);
    RUN_TEST(test_a_long_program_goes_in_pieces);
    RUN_TEST(test_a_formula_that_is_wrong_is_rejected_with_the_reason);
    RUN_TEST(test_the_lamp_ends_an_animation_and_none_returns_to_the_lamp);
    RUN_TEST(test_after_a_power_up_the_strip_gets_back_what_it_showed);
    RUN_TEST(test_a_refused_program_is_reported);
    RUN_TEST(test_replacing_a_module_makes_new_instances_and_entities);
    RUN_TEST(test_replacing_with_the_same_module_changes_nothing);
    RUN_TEST(test_modules_a_slave_needs_are_not_given_up);
    RUN_TEST(test_a_reload_reads_the_plugins_anew);
    RUN_TEST(test_empty_payloads_arrive_as_null);
    RUN_TEST(test_messages_from_another_thread);
    return UNITY_END();
}
