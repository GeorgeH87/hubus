/*
 * hubusd core against the bus simulation and a fake MQTT client: startup,
 * events, commands, availability, statistics, discovery, faults, reload.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "hubus_sim.h"
#include "hubusd_core.h"
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

enum { DEV_RELAY = 1, DEV_SWITCH = 2, DEV_TEMP = 3, DEV_UNKNOWN = 0x77 };

typedef struct {
    hubus_slave_core_t core;
    uint8_t mcid;
    uint8_t type;
    uint8_t state;      /* relay: 0/1, switch: pressed, temperature: unused */
    uint16_t raw_temp;
    volatile uint8_t temp_be[2]; /* raw_temp as exposed state (big endian) */
    uint8_t info[6];
    unsigned commands;
    unsigned captures;
} sim_dev_t;

static hubus_sim_t sim;
static int dev_with_state; /* relay and temperature sensor expose a live state (REQUEST_STATE) */
static sim_dev_t dev[8];
static unsigned n_dev;
static hubusd_t *dmn;
static hubusd_config_t cfg;
static uint64_t clock_ms;

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

    v->commands++;
    if (v->type == DEV_RELAY && op == 0x10 && len >= 1) {
        v->state = args[0] ? 1 : 0;
    }
}

static void set_temp(sim_dev_t *v, uint16_t raw)
{
    v->raw_temp = raw;
    v->temp_be[0] = (uint8_t)(raw >> 8);
    v->temp_be[1] = (uint8_t)raw;
}

static sim_dev_t *add_dev(uint8_t mcid, uint8_t type)
{
    sim_dev_t *v = &dev[n_dev++];
    hubus_core_config_t c;

    memset(v, 0, sizeof *v);
    v->mcid = mcid;
    v->type = type;
    v->raw_temp = 298;
    v->info[0] = type;
    v->info[1] = 1;
    v->info[5] = mcid; /* serial: 00 00 00 <mcid> */
    memset(&c, 0, sizeof c);
    c.mcid = mcid;
    c.info = v->info;
    c.info_len = sizeof v->info;
    c.on_status_capture = dev_capture;
    c.on_command = dev_command;
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
    add_dev(9, DEV_UNKNOWN);
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
    clock_ms = 100000;
    fail_publish = 0;
    dmn = NULL;
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

    assert_contains("hubus/slave/3/info", "\"device_type\":1");
    assert_contains("hubus/slave/3/info", "\"serial\":\"00000003\"");
    assert_contains("hubus/slave/3/info", "\"profile\":\"relay\"");
    assert_contains("hubus/slave/3/info", "\"status_capture\":true");
    TEST_ASSERT_TRUE(last_msg("hubus/slave/3/info")->retain);

    /* initial state is read back from the slaves */
    assert_last("hubus/slave/3/state", "OFF");
    assert_last("hubus/slave/5/state", "OFF");
    assert_last("hubus/slave/7/state", "25");
    TEST_ASSERT_TRUE(last_msg("hubus/slave/3/state")->retain);
}

static void test_unknown_slave_is_reported_in_master_state_only(void)
{
    standard_setup(base_yaml);
    assert_contains("hubus/master/state", "\"unknown_slaves\":[{\"mcid\":9,\"device_type\":119,\"serial\":\"00000009\"}]");
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "unconfigured slave found at MCID 9") >= 1);
    TEST_ASSERT_NULL(last("hubus/slave/9/info"));
    TEST_ASSERT_NULL(last("hubus/slave/9/state"));
}

static void test_discovery_messages(void)
{
    standard_setup(base_yaml);

    assert_contains("homeassistant/switch/hubus_3/config", "\"command_topic\":\"hubus/slave/3/set\"");
    assert_contains("homeassistant/switch/hubus_3/config", "\"state_topic\":\"hubus/slave/3/state\"");
    assert_contains("homeassistant/switch/hubus_3/config", "\"unique_id\":\"hubus_3\"");
    assert_contains("homeassistant/switch/hubus_3/config", "\"name\":null");
    assert_contains("homeassistant/switch/hubus_3/config", "\"availability_mode\":\"all\"");
    assert_contains("homeassistant/switch/hubus_3/config", "{\"topic\":\"hubus/master/status\"}");
    assert_contains("homeassistant/switch/hubus_3/config", "{\"topic\":\"hubus/slave/3/availability\"}");
    assert_contains("homeassistant/switch/hubus_3/config", "\"name\":\"Garden relay\"");
    assert_contains("homeassistant/switch/hubus_3/config", "\"serial_number\":\"00000003\"");
    assert_contains("homeassistant/switch/hubus_3/config", "\"sw_version\":\"1\"");
    TEST_ASSERT_TRUE(last_msg("homeassistant/switch/hubus_3/config")->retain);

    assert_contains("homeassistant/binary_sensor/hubus_5/config", "\"payload_on\":\"ON\"");
    TEST_ASSERT_NULL(strstr(last("homeassistant/binary_sensor/hubus_5/config"), "command_topic"));

    assert_contains("homeassistant/sensor/hubus_7/config", "\"device_class\":\"temperature\"");
    assert_contains("homeassistant/sensor/hubus_7/config", "\"unit_of_measurement\":\"\xC2\xB0" "C\"");
    assert_contains("homeassistant/sensor/hubus_7/config", "\"state_class\":\"measurement\"");

    /* the master itself */
    assert_contains("homeassistant/sensor/hubusd_hubusd_uptime_s/config", "\"value_template\":\"{{ value_json.uptime_s }}\"");
    assert_contains("homeassistant/sensor/hubusd_hubusd_errors_total/config", "\"entity_category\":\"diagnostic\"");
    TEST_ASSERT_NULL(last("homeassistant/switch/hubus_9/config"));
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
    TEST_ASSERT_EQUAL_UINT(1, count("homeassistant/switch/hubus_20/config"));
    assert_last("homeassistant/switch/hubus_20/config", "");
    assert_last("homeassistant/sensor/hubus_63/config", "");
    TEST_ASSERT_NOT_EQUAL(0, strlen(last("homeassistant/switch/hubus_3/config")));
}

static void test_custom_base_topic_and_prefix(void)
{
    standard_setup("mqtt:\n  base_topic: home/bus\n  discovery_prefix: ha\nbus:\n  scan_interval_s: 0\nslaves:\n  - mcid: 3\n");
    assert_last("home/bus/master/status", "online");
    assert_last("home/bus/slave/3/availability", "online");
    assert_contains("ha/switch/hubus_3/config", "\"command_topic\":\"home/bus/slave/3/set\"");
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
    assert_last("hubus/slave/5/state", "ON");
    assert_contains("hubus/slave/5/data", "\"data\":\"01\"");
    assert_contains("hubus/slave/5/data", "\"result\":\"normal\"");
    assert_contains("hubus/slave/5/data", "\"mcid\":5");
    TEST_ASSERT_FALSE(last_msg("hubus/slave/5/data")->retain);

    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&sim, 1, &released, 1, 0));
    settle();
    assert_last("hubus/slave/5/state", "OFF");
    TEST_ASSERT_EQUAL_UINT(2, count("hubus/slave/5/state"));
}

static void test_data_from_an_unconfigured_slave_is_ignored(void)
{
    uint8_t v = 1;

    standard_setup(base_yaml);
    n_msgs = 0;
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&sim, 3, &v, 1, 0)); /* device 9 */
    settle();
    TEST_ASSERT_NULL(last("hubus/slave/9/data"));
    TEST_ASSERT_NULL(last("hubus/slave/9/state"));
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
    say("hubus/slave/3/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
    TEST_ASSERT_EQUAL_UINT(1, dev[0].commands);
    assert_last("hubus/slave/3/state", "ON"); /* read back from the slave, not echoed */

    say("hubus/slave/3/set", "off");
    settle();
    TEST_ASSERT_EQUAL_UINT8(0, dev[0].state);
    assert_last("hubus/slave/3/state", "OFF");
}

static void test_published_state_is_the_real_state_not_the_command(void)
{
    standard_setup(base_yaml);
    dev[0].core.on_command = NULL; /* the relay ignores commands (defective) */
    n_msgs = 0;
    say("hubus/slave/3/set", "ON");
    settle();
    assert_last("hubus/slave/3/state", "OFF");
}

static void test_readback_can_be_disabled(void)
{
    standard_setup("bus:\n  readback: false\n  scan_interval_s: 0\nslaves:\n  - mcid: 3\n");
    n_msgs = 0;
    say("hubus/slave/3/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
    TEST_ASSERT_NULL(last("hubus/slave/3/state"));
    say("hubus/slave/3/capture", "");
    settle();
    assert_last("hubus/slave/3/state", "ON");
}

static void test_cache_readback_reports_the_command_without_contacting_the_slave(void)
{
    standard_setup("bus:\n  readback: cache\n  scan_interval_s: 0\n  ping_interval_s: 0\nslaves:\n  - mcid: 3\n");
    TEST_ASSERT_EQUAL_UINT(1, dev[0].captures); /* the initial state read at startup */
    n_msgs = 0;
    say("hubus/slave/3/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
    TEST_ASSERT_EQUAL_UINT(1, dev[0].captures); /* no STATUS_CAPTURE after the command */
    assert_last("hubus/slave/3/state", "ON");
    TEST_ASSERT_TRUE(last_msg("hubus/slave/3/state")->retain);
    TEST_ASSERT_EQUAL_UINT(0, count("hubus/slave/3/data"));
    say("hubus/slave/3/set", "off");
    settle();
    assert_last("hubus/slave/3/state", "OFF");
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
    say("hubus/slave/3/set", "ON");
    settle();
    assert_last("hubus/slave/3/state", "ON"); /* the cache is optimistic; mode `slave` would show OFF */
    TEST_ASSERT_EQUAL_UINT8(0, dev[0].state);
}

static void test_cache_readback_leaves_raw_commands_alone(void)
{
    standard_setup("bus:\n  readback: cache\n  scan_interval_s: 0\n  ping_interval_s: 0\nslaves:\n  - mcid: 3\n");
    n_msgs = 0;
    say("hubus/slave/3/cmd", "0x10 1");
    settle();
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
    TEST_ASSERT_NULL(last("hubus/slave/3/state")); /* the effect of a raw command is not known */
}

static void test_readback_mode_per_slave_overrides_the_bus_default(void)
{
    standard_setup("bus:\n  readback: slave\n  scan_interval_s: 0\n  ping_interval_s: 0\n"
                   "slaves:\n  - mcid: 3\n    readback: cache\n  - mcid: 5\n");
    dev[0].core.on_command = NULL;
    say("hubus/slave/3/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dev[0].captures);
    assert_last("hubus/slave/3/state", "ON");
}

/* A relay with neither a state nor STATUS_CAPTURE (built without both): readback `slave` falls back to the cache. */
static void test_slave_that_cannot_be_read_back_reports_the_cached_state(void)
{
    load("bus:\n  readback: slave\n  scan_interval_s: 0\n  ping_interval_s: 0\nslaves:\n  - mcid: 3\n");
    add_dev(3, DEV_RELAY);
    dev[0].core.on_status_capture = NULL;
    start_daemon();
    settle();
    TEST_ASSERT_NULL(last("hubus/slave/3/state")); /* nothing to read at startup */
    say("hubus/slave/3/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
    assert_last("hubus/slave/3/state", "ON");
    TEST_ASSERT_TRUE(last_msg("hubus/slave/3/state")->retain);
    TEST_ASSERT_EQUAL_UINT(0, dev[0].captures);
    TEST_ASSERT_EQUAL_UINT(0, dmn->master.counters.state_requests);
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
    TEST_ASSERT_NULL(last("hubus/slave/7/state")); /* not yet due */
    advance(1);
    assert_last("hubus/slave/7/state", "38");
    TEST_ASSERT_EQUAL_UINT(0, dev[2].captures); /* REQUEST_STATE, no capture */
    set_temp(&dev[2], 301);
    advance(10);
    assert_last("hubus/slave/7/state", "28");
    TEST_ASSERT_EQUAL_UINT(2, count("hubus/slave/7/state"));
    TEST_ASSERT_NULL(last("hubus/slave/3/state")); /* only the slave with the option is polled */
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
    assert_last("hubus/slave/7/state", "30");
}

static void test_invalid_set_commands_are_rejected(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    say("hubus/slave/3/set", "TOGGLE");
    say("hubus/slave/5/set", "ON");  /* a button takes no commands */
    say("hubus/slave/9/set", "ON");  /* not configured */
    say("hubus/slave/4/set", "ON");  /* not configured */
    settle();
    TEST_ASSERT_EQUAL_UINT(4, count_containing("hubus/master/log", "command rejected"));
    TEST_ASSERT_EQUAL_UINT(0, dev[0].commands);
    TEST_ASSERT_EQUAL_UINT(4, dmn->stats.commands_rejected);
}

static void test_malformed_topics_and_payloads_are_rejected(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    say("hubus/slave/0/set", "ON");
    say("hubus/slave/64/set", "ON");
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
    say("other/slave/3/set", "ON");
    say("hubus/slave/3/state", "ON");
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
    say("hubus/slave/3/set", big);
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dmn->stats.commands_rejected);
}

static void test_set_for_a_slave_without_a_profile_is_rejected(void)
{
    standard_setup("bus:\n  scan_interval_s: 0\nslaves:\n  - mcid: 9\n"); /* device type 0x77: no profile */
    n_msgs = 0;
    say("hubus/slave/9/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dmn->stats.commands_rejected);
    TEST_ASSERT_EQUAL_UINT(1, count_containing("hubus/master/log", "has no profile"));
    say("hubus/slave/9/cmd", "0x10 1"); /* the raw topic still works */
    settle();
    TEST_ASSERT_EQUAL_UINT(1, dmn->stats.commands_rejected);
    TEST_ASSERT_EQUAL_UINT(1, dev[3].commands); /* the raw command reached the slave */
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
    standard_setup(base_yaml);
    dmn->slave[3].info.payload[HUBUS_INFO_MAX_CMD] = 2; /* announced in SLAVE_INFO */
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
    assert_last("hubus/slave/3/state", "ON");
    assert_last("hubus/slave/7/state", "25");
}

static void test_capture_topic(void)
{
    standard_setup(base_yaml);
    dev[2].raw_temp = 303;
    n_msgs = 0;
    say("hubus/slave/7/capture", "");
    settle();
    assert_last("hubus/slave/7/state", "30");
    TEST_ASSERT_EQUAL_UINT(2, dev[2].captures); /* one at startup, one now */
}

static void test_capture_of_a_slave_without_support_is_rejected(void)
{
    standard_setup(base_yaml);
    dmn->slave[5].info.payload[HUBUS_INFO_CAPS] = 0;
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
    assert_contains("hubus/master/scan", "{\"mcid\":3,\"state\":\"present\",\"configured\":true,\"device_type\":1}");
    assert_contains("hubus/master/scan", "{\"mcid\":9,\"state\":\"present\",\"configured\":false,\"device_type\":119}");
    TEST_ASSERT_NULL(strstr(last("hubus/master/scan"), "\"mcid\":4"));
}

static void test_queue_overflow_is_counted_and_harmless(void)
{
    unsigned i;

    standard_setup(base_yaml);
    for (i = 0; i < 100; i++) {
        say("hubus/slave/3/set", (i & 1) ? "OFF" : "ON");
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
    assert_last("hubus/slave/7/state", "38"); /* state refreshed after it returned */
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
    TEST_ASSERT_NULL(last("homeassistant/sensor/hubus_7/config")); /* profile unknown until the slave answers */
}

static void test_slave_appearing_later_gets_discovery(void)
{
    load(base_yaml);
    add_dev(3, DEV_RELAY);
    start_daemon();
    settle();
    TEST_ASSERT_NULL(last("homeassistant/sensor/hubus_7/config"));
    add_dev(7, DEV_TEMP);
    advance(40);
    assert_last("hubus/slave/7/availability", "online");
    assert_contains("homeassistant/sensor/hubus_7/config", "\"device_class\":\"temperature\"");
    assert_last("hubus/slave/7/state", "25");
}

/* ---- statistics ------------------------------------------------------------------------------------ */

static void test_statistics_on_request(void)
{
    uint8_t pressed = 1;

    standard_setup(base_yaml);
    hubus_sim_slave_write(&sim, 1, &pressed, 1, 0);
    settle();
    say("hubus/slave/3/set", "ON");
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
    assert_last("hubus/slave/3/state", "OFF");
    assert_last("hubus/slave/7/state", "25");
    assert_last("hubus/slave/5/state", "OFF");
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
    say("hubus/slave/3/set", "ON");
    settle();
    TEST_ASSERT_EQUAL_UINT8(1, dev[0].state);
    TEST_ASSERT_EQUAL_UINT(0, dev[0].captures); /* no STATUS_CAPTURE, no burst */
    assert_last("hubus/slave/3/state", "ON");
    assert_contains("hubus/slave/3/data", "\"result\":\"state\"");
    TEST_ASSERT_EQUAL_UINT(1, count("hubus/slave/3/data"));
}

static void test_published_state_is_still_the_real_state_with_a_state_read(void)
{
    dev_with_state = 1;
    standard_setup(base_yaml);
    dev[0].core.on_command = NULL; /* the relay ignores commands (defective) */
    n_msgs = 0;
    say("hubus/slave/3/set", "ON");
    settle();
    assert_last("hubus/slave/3/state", "OFF");
}

static void test_read_topic_reads_the_live_state_at_any_time(void)
{
    dev_with_state = 1;
    standard_setup(base_yaml);
    set_temp(&dev[2], 303);
    n_msgs = 0;
    say("hubus/slave/7/read", "");
    settle();
    assert_last("hubus/slave/7/state", "30");
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
    assert_last("hubus/slave/3/state", "ON");
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
    dev[n_dev - 1].info[5] = 0x99; /* different serial number: the responses cannot overlap into a valid frame */
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
    assert_last("hubus/slave/7/state", "25"); /* its state is refreshed */
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
    say("hubus/slave/3/set", "ON");
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
    say("hubus/slave/3/set", "ON");
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
    dev[2].info[5] = 0x99;
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
    assert_last("hubus/slave/3/state", "OFF");
    TEST_ASSERT_NOT_NULL(last("hubus/slave/3/info"));
    TEST_ASSERT_NOT_NULL(last("homeassistant/switch/hubus_3/config"));
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
    assert_last("homeassistant/binary_sensor/hubus_5/config", "");
    assert_last("homeassistant/sensor/hubus_7/config", "");
    assert_last("hubus/slave/5/availability", "");
    assert_last("hubus/slave/7/state", "");
    /* slave 3 stays with the new name */
    assert_contains("homeassistant/switch/hubus_3/config", "\"name\":\"Renamed\"");
    assert_last("hubus/slave/3/availability", "online");
    /* slave 9 has no profile for device type 0x77: online, but only raw topics */
    assert_last("hubus/slave/9/availability", "online");
    TEST_ASSERT_NULL(last("homeassistant/switch/hubus_9/config"));
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

static char sub_topics[8][160];
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
    TEST_ASSERT_EQUAL_UINT(6, sub_n);
    TEST_ASSERT_EQUAL_STRING("hubus/master/cmd/+", sub_topics[0]);
    TEST_ASSERT_EQUAL_STRING("hubus/slave/+/set", sub_topics[1]);
    TEST_ASSERT_EQUAL_STRING("hubus/slave/+/read", sub_topics[4]);
    TEST_ASSERT_EQUAL_STRING("hubus/group/+/+/cmd", sub_topics[5]);
    TEST_ASSERT_EQUAL_UINT(strlen("hubus/master/status"), hubusd_status_topic(dmn, status, sizeof status));
    TEST_ASSERT_EQUAL_STRING("hubus/master/status", status);
}

static void test_empty_payloads_arrive_as_null(void)
{
    standard_setup(base_yaml);
    n_msgs = 0;
    hubusd_handle_message(dmn, "hubus/slave/7/capture", NULL, 0);
    hubusd_handle_message(dmn, "hubus/master/cmd/scan", NULL, 0);
    hubusd_handle_message(dmn, "hubus/master/cmd/stats", NULL, 0);
    hubusd_handle_message(dmn, "hubus/slave/3/set", NULL, 0); /* empty command: rejected */
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
        say("hubus/slave/3/set", (i & 1) ? "OFF" : "ON");
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
    RUN_TEST(test_poll_interval_reads_the_state_periodically);
    RUN_TEST(test_poll_interval_captures_a_slave_without_a_state);
    RUN_TEST(test_readback_mode_per_slave_overrides_the_bus_default);
    RUN_TEST(test_invalid_set_commands_are_rejected);
    RUN_TEST(test_malformed_topics_and_payloads_are_rejected);
    RUN_TEST(test_unrelated_topics_are_ignored);
    RUN_TEST(test_oversized_payload_is_rejected);
    RUN_TEST(test_set_for_a_slave_without_a_profile_is_rejected);
    RUN_TEST(test_raw_command_exact);
    RUN_TEST(test_raw_command_larger_than_the_slave_accepts_is_rejected);
    RUN_TEST(test_group_and_broadcast_commands);
    RUN_TEST(test_group_status_capture_collects_states);
    RUN_TEST(test_capture_topic);
    RUN_TEST(test_capture_of_a_slave_without_support_is_rejected);
    RUN_TEST(test_scan_command);
    RUN_TEST(test_startup_reads_the_state_of_slaves_that_have_one);
    RUN_TEST(test_readback_after_set_uses_one_state_read);
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
    RUN_TEST(test_empty_payloads_arrive_as_null);
    RUN_TEST(test_messages_from_another_thread);
    return UNITY_END();
}
