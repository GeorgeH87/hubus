/*
 * Firmware updates from a module: hubusd on the simulated bus against the real updater logic with a flash array and an
 * application core with update support. The device model is the one of test_update.c: UPDATE_ENTER jumps from the application
 * to the updater, UPDATE_LEAVE resets, and the reset starts the application if its info page checks out.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hubus_sim.h"
#include "hubus_updater.h"
#include "hubusd_core.h"
#include "hubusd_config.h"
#include "unity.h"
#include "update_app_core.h"

#define MCID 9u
#define FLASH_SIZE 8192u
#define PAGE HUBUS_UPD_PAGE_SIZE
#define OLD_VERSION 7u
#define NEW_VERSION 8u

static hubus_sim_t sim;
static hubus_master_hal_t sim_hal;
static unsigned dev;
static uint8_t flash[FLASH_SIZE];
static uint8_t eeprom_mcid;
static uint8_t reset_requested;
static uint8_t in_updater;
static unsigned flash_writes;
static uint8_t app_hwid[] = {0xF4, 0x01, OLD_VERSION, 0x00, 0x09};
static int cut_after_writes; /* a power cut: from this many writes on the page writes get lost; 0: never */

uint8_t hubus_upd_flash_read(uint16_t addr)
{
    return flash[addr];
}

void hubus_upd_flash_write(uint16_t addr, const uint8_t *page)
{
    TEST_ASSERT_TRUE(addr % PAGE == 0 && addr + PAGE <= HUBUS_UPD_ADDR);
    flash_writes++;
    if (cut_after_writes && flash_writes >= (unsigned)cut_after_writes) {
        return;
    }
    memcpy(&flash[addr], page, PAGE);
}

void hubus_upd_reset(void)
{
    reset_requested = 1;
}

static void app_pins(hubus_slave_core_t *core, uint8_t clk, uint8_t data)
{
    (void)core;
    app_feed(clk, data);
}

static void run_updater(uint8_t mcid)
{
    in_updater = 1;
    TEST_ASSERT_TRUE(hubus_updater_init(mcid, sim.clk, sim.data));
    sim.slave[dev] = &hubus_updater_core;
    sim.pins[dev] = NULL;
    hubus_sim_settle(&sim);
}

static void run_application(void)
{
    in_updater = 0;
    TEST_ASSERT_TRUE(app_start(eeprom_mcid, app_hwid, sizeof app_hwid, sim.clk, sim.data));
    sim.slave[dev] = (hubus_slave_core_t *)app_core();
    sim.pins[dev] = app_pins;
    hubus_sim_settle(&sim);
}

static void boot(void)
{
    uint16_t entry;

    if (hubus_updater_app_valid(&entry)) {
        run_application();
    } else {
        run_updater(eeprom_mcid);
    }
}

static void device_step(void)
{
    if (in_updater) {
        hubus_updater_poll();
        if (reset_requested) {
            reset_requested = 0;
            boot();
        }
    } else if (app_take_update_request()) {
        eeprom_mcid = app_mcid();
        run_updater(eeprom_mcid);
    }
}

static void hal_set_clk(void *ctx, uint8_t high)
{
    (void)ctx;
    sim_hal.set_clk(sim_hal.ctx, high);
}

static void hal_set_data_low(void *ctx, uint8_t low)
{
    (void)ctx;
    sim_hal.set_data_low(sim_hal.ctx, low);
}

static uint8_t hal_read_data(void *ctx)
{
    (void)ctx;
    return sim_hal.read_data(sim_hal.ctx);
}

static void hal_delay_ns(void *ctx, uint32_t ns)
{
    (void)ctx;
    sim_hal.delay_ns(sim_hal.ctx, ns);
    device_step();
}

/* ---- fake MQTT and clock ---------------------------------------------------------------------------------------- */

#define MSG_MAX 2048

typedef struct {
    char topic[200];
    char payload[1400];
} msg_t;

static msg_t msgs[MSG_MAX];
static unsigned n_msgs;

static int fake_publish(void *ctx, const char *topic, const char *payload, size_t len, int retain)
{
    (void)ctx;
    (void)retain;
    TEST_ASSERT_TRUE(n_msgs < MSG_MAX);
    TEST_ASSERT_TRUE(len < sizeof msgs[0].payload);
    snprintf(msgs[n_msgs].topic, sizeof msgs[0].topic, "%s", topic);
    memcpy(msgs[n_msgs].payload, payload, len);
    msgs[n_msgs].payload[len] = '\0';
    n_msgs++;
    return 0;
}

static const char *last(const char *topic)
{
    unsigned i;

    for (i = n_msgs; i-- > 0;) {
        if (strcmp(msgs[i].topic, topic) == 0) {
            return msgs[i].payload;
        }
    }
    return NULL;
}

static unsigned count_containing(const char *topic, const char *needle)
{
    unsigned i, n = 0;

    for (i = 0; i < n_msgs; i++) {
        n += strcmp(msgs[i].topic, topic) == 0 && strstr(msgs[i].payload, needle) != NULL;
    }
    return n;
}

static void assert_contains(const char *topic, const char *needle)
{
    const char *p = last(topic);

    TEST_ASSERT_NOT_NULL_MESSAGE(p, topic);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(p, needle), p);
}

static uint64_t clock_ms;

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

/* ---- modules ---------------------------------------------------------------------------------------------------- */

static uint8_t image[1000];

static const hubus_module_rule_t app_rules[] = {{.pattern = {0xF4, 0x01}, .mask = {0xFF, 0xFF}, .len = 2}};
static const hubus_module_rule_t updater_rules[] = {{.pattern = {HUBUS_HWID_TYPE_UPDATER}, .mask = {0xFF}, .len = 1}};
static const hubus_entity_t app_entities[] = {
    {.key = "first", .name = "First", .component = HUBUS_COMPONENT_SENSOR, .decode = {.offset = 0, .len = 1, .scale = 1.0}},
};

static unsigned live_instances, created_instances;
static int instance;

static hubus_module_instance_t *app_create(const hubus_module_host_t *host)
{
    (void)host;
    __atomic_add_fetch(&live_instances, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&created_instances, 1, __ATOMIC_SEQ_CST);
    return (hubus_module_instance_t *)&instance;
}

static void app_destroy(hubus_module_instance_t *self)
{
    (void)self;
    __atomic_sub_fetch(&live_instances, 1, __ATOMIC_SEQ_CST);
}

static const hubus_module_t updater_module = {
    HUBUS_MODULE_INIT, .name = "updater", .rules = updater_rules, .n_rules = 1, .max_command = HUBUS_MAX_PAYLOAD, .has_state = true,
};
static hubus_module_t app_module = {
    HUBUS_MODULE_INIT,
    .name = "app",
    .rules = app_rules,
    .n_rules = 1,
    .max_command = HUBUS_MAX_PAYLOAD,
    .has_state = true,
    .has_update = true,
    .entities = app_entities,
    .n_entities = 1,
    .firmware = image,
    .firmware_len = sizeof image,
    .firmware_version = NEW_VERSION,
    .create = app_create,
    .destroy = app_destroy,
};
static const hubus_module_t *const modules[] = {&app_module, &updater_module};

/* ---- setup ------------------------------------------------------------------------------------------------------ */

static hubusd_t *dmn;
static hubusd_config_t cfg;

/* What an image of the updater looks like: RJMP at address 0, then pseudo-random bytes. */
static void make_image(uint8_t *img, size_t len, uint16_t entry, unsigned seed)
{
    size_t i;

    for (i = 0; i < len; i++) {
        seed = seed * 1103515245u + 12345u;
        img[i] = (uint8_t)(seed >> 16);
    }
    img[0] = (uint8_t)(entry - 1u);
    img[1] = (uint8_t)(0xC0u | ((entry - 1u) >> 8));
}

static void settle(void)
{
    unsigned i;

    for (i = 0; i < 200; i++) {
        if (!hubusd_step(dmn)) {
            return;
        }
    }
}

static void say(const char *topic, const char *payload)
{
    hubusd_handle_message(dmn, topic, payload, strlen(payload));
}

/* Starts hubusd with the device, which runs its old application, or waits in its updater with an empty flash. */
static void start(int with_application)
{
    static const char *yaml =
        "mqtt:\n  stats_interval_s: 0\nbus:\n  ping_interval_s: 5\n  ping_failures: 3\n  scan_interval_s: 0\nslaves:\n  - mcid: 9\n";
    hubus_master_hal_t hal;
    hubusd_mqtt_t mq = {NULL, fake_publish};
    hubusd_clock_t clk = {NULL, clk_mono, clk_wall};
    char err[200];

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hubusd_config_parse(yaml, strlen(yaml), &cfg, err, sizeof err), err);
    hubus_sim_init(&sim);
    sim_hal = hubus_sim_master_hal(&sim);
    hal.ctx = NULL;
    hal.set_clk = hal_set_clk;
    hal.set_data_low = hal_set_data_low;
    hal.read_data = hal_read_data;
    hal.delay_ns = hal_delay_ns;
    TEST_ASSERT_TRUE(hubus_updater_init(MCID, sim.clk, sim.data));
    dev = hubus_sim_add(&sim, &hubus_updater_core);
    if (with_application) {
        run_application();
    } else {
        run_updater(eeprom_mcid);
    }
    dmn = malloc(sizeof *dmn);
    TEST_ASSERT_NOT_NULL(dmn);
    hubusd_init(dmn, &cfg, &hal, mq, clk);
    hubusd_set_modules(dmn, modules, sizeof modules / sizeof modules[0]);
    settle();
}

void setUp(void)
{
    memset(flash, 0xFF, sizeof flash);
    eeprom_mcid = MCID;
    reset_requested = 0;
    flash_writes = 0;
    cut_after_writes = 0;
    n_msgs = 0;
    clock_ms = 100000;
    app_hwid[HUBUS_HWID_VERSION] = OLD_VERSION;
    live_instances = created_instances = 0;
    make_image(image, sizeof image, 0x0F, 1);
    dmn = NULL;
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT(0, sim.violations);
    if (dmn) {
        hubusd_destroy(dmn);
        free(dmn);
    }
}

/* The next start of the application reports the version of the image it came from. */
static void after_update_new_version(void)
{
    app_hwid[HUBUS_HWID_VERSION] = NEW_VERSION;
}

static void wait_for_live_instances(unsigned n)
{
    unsigned k;

    for (k = 0; k < 2000u && __atomic_load_n(&live_instances, __ATOMIC_SEQ_CST) != n; k++) {
        settle();
        hubus_sim_settle(&sim);
    }
    TEST_ASSERT_EQUAL_UINT(n, __atomic_load_n(&live_instances, __ATOMIC_SEQ_CST));
}

/* ---- tests ------------------------------------------------------------------------------------------------------ */

static void test_the_module_offers_its_image_to_home_assistant(void)
{
    start(1);
    assert_contains("homeassistant/update/hubus_9_firmware/config", "\"command_topic\":\"hubus/slave/9/update\"");
    assert_contains("homeassistant/update/hubus_9_firmware/config", "\"state_topic\":\"hubus/slave/9/firmware\"");
    assert_contains("homeassistant/update/hubus_9_firmware/config", "\"payload_install\":\"app\"");
    assert_contains("homeassistant/update/hubus_9_firmware/config", "\"identifiers\":[\"hubus_9\"]");
    assert_contains("hubus/slave/9/firmware", "\"installed_version\":\"7\"");
    assert_contains("hubus/slave/9/firmware", "\"latest_version\":\"8\"");
    assert_contains("hubus/slave/9/firmware", "\"in_progress\":false");
}

static void test_a_trigger_loads_the_image_of_the_module(void)
{
    start(1);
    wait_for_live_instances(1);
    n_msgs = 0;
    after_update_new_version();
    say("hubus/slave/9/update", "");
    settle();

    TEST_ASSERT_FALSE(in_updater);
    TEST_ASSERT_EQUAL_MEMORY(image + 2, flash + 2, sizeof image - 2);
    TEST_ASSERT_TRUE(flash_writes > 0u);
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "done") >= 1u);
    TEST_ASSERT_TRUE(count_containing("hubus/slave/9/firmware", "\"in_progress\":true") >= 2u);
    TEST_ASSERT_TRUE(count_containing("hubus/slave/9/firmware", "\"update_percentage\":100") >= 1u);
    assert_contains("hubus/slave/9/firmware", "\"installed_version\":\"8\"");
    assert_contains("hubus/slave/9/firmware", "\"in_progress\":false");
    assert_contains("hubus/slave/9/firmware", "\"update_percentage\":null");
    assert_contains("hubus/slave/9/info", "\"firmware\":8");
    TEST_ASSERT_EQUAL_PTR(&app_module, dmn->slave[MCID].module);
    TEST_ASSERT_NULL(dmn->slave[MCID].update_module);
    wait_for_live_instances(1);
    TEST_ASSERT_EQUAL_UINT(2, __atomic_load_n(&created_instances, __ATOMIC_SEQ_CST)); /* a new one for the restarted slave */

    /* the bus goes on: the state of the new application is read and the slave stays online */
    clock_ms += 6000;
    settle();
    TEST_ASSERT_EQUAL_INT(1, dmn->slave[MCID].online);
    TEST_ASSERT_EQUAL_INT(HUBUSD_BUS_OK, dmn->bus);
}

static void test_the_payload_of_home_assistant_names_the_module(void)
{
    start(1);
    after_update_new_version();
    say("hubus/slave/9/update", "app");
    settle();
    assert_contains("hubus/slave/9/firmware", "\"installed_version\":\"8\"");
}

static void test_a_slave_on_the_version_of_the_image_gets_no_update(void)
{
    start(1);
    app_module.firmware_version = OLD_VERSION;
    say("hubus/slave/9/update", "");
    settle();
    app_module.firmware_version = NEW_VERSION;
    TEST_ASSERT_EQUAL_UINT(0, flash_writes);
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "already runs version 7") >= 1u);
}

static void test_an_older_image_can_be_loaded_on_purpose(void)
{
    start(1);
    app_module.firmware_version = 3;
    app_hwid[HUBUS_HWID_VERSION] = 3;
    say("hubus/slave/9/update", "");
    settle();
    app_module.firmware_version = NEW_VERSION;
    TEST_ASSERT_TRUE(flash_writes > 0u);
    assert_contains("hubus/slave/9/firmware", "\"installed_version\":\"3\"");
}

static void test_a_trigger_cannot_load_the_image_of_another_module(void)
{
    start(1);
    say("hubus/slave/9/update", "updater");
    settle();
    TEST_ASSERT_EQUAL_UINT(0, flash_writes);
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "uses module app, not updater") >= 1u);

    say("hubus/slave/9/update", "nothing");
    settle();
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "no module 'nothing'") >= 1u);
    TEST_ASSERT_EQUAL_UINT(0, flash_writes);
}

static void test_a_slave_in_its_updater_needs_the_name_of_the_module(void)
{
    start(0); /* an update that broke off before this start of hubusd */
    TEST_ASSERT_TRUE(in_updater);
    assert_contains("hubus/slave/9/info", "\"updater\":true");

    say("hubus/slave/9/update", "");
    settle();
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "name the module") >= 1u);
    TEST_ASSERT_EQUAL_UINT(0, flash_writes);

    say("hubus/slave/9/update", "updater");
    settle();
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "module updater has no firmware image") >= 1u);

    after_update_new_version();
    say("hubus/slave/9/update", "app");
    settle();
    TEST_ASSERT_FALSE(in_updater);
    assert_contains("hubus/slave/9/info", "\"module\":\"app\"");
    assert_contains("hubus/slave/9/firmware", "\"installed_version\":\"8\"");
    assert_contains("homeassistant/update/hubus_9_firmware/config", "\"payload_install\":\"app\"");
}

static void test_a_failed_update_can_be_repeated_without_a_name(void)
{
    start(1);
    cut_after_writes = 3; /* the writes do not arrive: the updater reports it, and the master gives up */
    after_update_new_version();
    say("hubus/slave/9/update", "");
    settle();
    TEST_ASSERT_TRUE(in_updater);
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "failed") >= 1u);
    assert_contains("hubus/slave/9/info", "\"module\":\"updater\"");
    assert_contains("hubus/slave/9/firmware", "\"latest_version\":\"8\"");
    assert_contains("hubus/slave/9/firmware", "\"in_progress\":false");
    TEST_ASSERT_EQUAL_PTR(&app_module, dmn->slave[MCID].update_module);

    cut_after_writes = 0;
    say("hubus/slave/9/update", "");
    settle();
    TEST_ASSERT_FALSE(in_updater);
    TEST_ASSERT_NULL(dmn->slave[MCID].update_module);
    assert_contains("hubus/slave/9/firmware", "\"installed_version\":\"8\"");
}

static void test_a_module_without_an_image_offers_no_update(void)
{
    const uint8_t *saved = app_module.firmware;

    app_module.firmware = NULL;
    app_module.firmware_len = 0;
    start(1);
    TEST_ASSERT_NULL(last("homeassistant/update/hubus_9_firmware/config"));
    TEST_ASSERT_NULL(last("hubus/slave/9/firmware"));
    say("hubus/slave/9/update", "");
    settle();
    app_module.firmware = saved;
    app_module.firmware_len = sizeof image;
    TEST_ASSERT_TRUE(count_containing("hubus/master/log", "has no firmware image") >= 1u);
    TEST_ASSERT_EQUAL_UINT(0, flash_writes);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_module_offers_its_image_to_home_assistant);
    RUN_TEST(test_a_trigger_loads_the_image_of_the_module);
    RUN_TEST(test_the_payload_of_home_assistant_names_the_module);
    RUN_TEST(test_a_slave_on_the_version_of_the_image_gets_no_update);
    RUN_TEST(test_an_older_image_can_be_loaded_on_purpose);
    RUN_TEST(test_a_trigger_cannot_load_the_image_of_another_module);
    RUN_TEST(test_a_slave_in_its_updater_needs_the_name_of_the_module);
    RUN_TEST(test_a_failed_update_can_be_repeated_without_a_name);
    RUN_TEST(test_a_module_without_an_image_offers_no_update);
    return UNITY_END();
}
