/*
 * Firmware update end to end on the simulated bus (docs/spec/06-operation.md section 6.8): the master library
 * against the real updater logic with a flash array, and an application core with HUBUS_ENABLE_UPDATE. A small
 * device model switches between them like the ATtiny85 does: UPDATE_ENTER jumps from the application to the
 * updater, UPDATE_LEAVE resets, and the reset starts the application if its info page checks out.
 */
#include <stdlib.h>
#include <string.h>

#include "hubus_crc.h"
#include "hubus_master.h"
#include "hubus_sim.h"
#include "hubus_update.h"
#include "hubus_updater.h"
#include "unity.h"
#include "update_app_core.h"

#define MCID 9u
#define FLASH_SIZE 8192u
#define PAGE HUBUS_UPD_PAGE_SIZE

static hubus_sim_t sim;
static hubus_master_t master;
static hubus_master_hal_t sim_hal;
static unsigned dev;
static uint8_t flash[FLASH_SIZE];
static uint8_t eeprom_mcid;
static uint8_t reset_requested;
static uint8_t in_updater;
static uint16_t booted_entry;
static unsigned flash_writes;
static const uint8_t app_info[] = {0x02, 0x07, 0x00, 0x09};

/* ---- platform of the updater ------------------------------------------------------------------------------------ */

uint8_t hubus_upd_flash_read(uint16_t addr)
{
    return flash[addr];
}

void hubus_upd_flash_write(uint16_t addr, const uint8_t *page)
{
    TEST_ASSERT_TRUE(addr % PAGE == 0 && addr + PAGE <= HUBUS_UPD_ADDR);
    memcpy(&flash[addr], page, PAGE);
    flash_writes++;
}

void hubus_upd_reset(void)
{
    reset_requested = 1;
}

/* ---- device model ----------------------------------------------------------------------------------------------- */

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

/* Reset: the updater starts a valid application, otherwise it stays with the MCID from EEPROM. */
static void boot(void)
{
    uint16_t entry;

    if (hubus_updater_app_valid(&entry)) {
        in_updater = 0;
        booted_entry = entry;
        TEST_ASSERT_TRUE(app_start(eeprom_mcid, app_info, sizeof app_info, sim.clk, sim.data));
        sim.slave[dev] = (hubus_slave_core_t *)app_core();
        sim.pins[dev] = app_pins;
        hubus_sim_settle(&sim);
    } else {
        run_updater(eeprom_mcid);
    }
}

/* What the device does between pin changes: the updater's main loop, the application's hand-over. */
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

/* ---- images ----------------------------------------------------------------------------------------------------- */

static uint8_t image_a[1000];
static uint8_t image_b[1500];

/* An application image: RJMP to word `entry` at address 0, then pseudo-random bytes. */
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

static hubus_update_image_t image(const uint8_t *data, uint32_t len)
{
    hubus_update_image_t img;

    memset(&img, 0, sizeof img);
    img.data = data;
    img.len = len;
    return img;
}

static void assert_flash_holds(const uint8_t *img, size_t len, uint16_t entry)
{
    TEST_ASSERT_EQUAL_HEX8(HUBUS_UPD_RESET_RJMP & 0xFF, flash[0]);
    TEST_ASSERT_EQUAL_HEX8(HUBUS_UPD_RESET_RJMP >> 8, flash[1]);
    TEST_ASSERT_EQUAL_MEMORY(img + 2, flash + 2, len - 2);
    TEST_ASSERT_EQUAL_HEX8(HUBUS_UPD_MAGIC0, flash[HUBUS_UPD_INFO_ADDR]);
    TEST_ASSERT_FALSE(in_updater);
    TEST_ASSERT_EQUAL_UINT16(entry, booted_entry);
}

/* ---- fault injection -------------------------------------------------------------------------------------------- */

/* The device sees DATA inverted during the high phase of its rising edge number `flip_at` (0 = never): one bit
 * error, received by the device or seen by it while it transmits. */
static unsigned flip_at;
static unsigned rises;
static uint8_t last_clk;

static int flip_filter(hubus_sim_t *s, unsigned slave, uint8_t *clk, uint8_t *data, void *ctx)
{
    (void)s;
    (void)ctx;
    if (slave != dev) {
        return 1;
    }
    if (*clk && !last_clk) {
        rises++;
    }
    last_clk = *clk;
    if (*clk && rises == flip_at) {
        *data = (uint8_t)!*data;
    }
    return 1;
}

/* ---- setup ------------------------------------------------------------------------------------------------------ */

void setUp(void)
{
    hubus_master_hal_t hal;
    hubus_master_cfg_t cfg;

    memset(flash, 0xFF, sizeof flash);
    eeprom_mcid = MCID;
    reset_requested = 0;
    flash_writes = 0;
    flip_at = 0;
    rises = 0;
    hubus_sim_init(&sim);
    sim_hal = hubus_sim_master_hal(&sim);
    hal.ctx = NULL;
    hal.set_clk = hal_set_clk;
    hal.set_data_low = hal_set_data_low;
    hal.read_data = hal_read_data;
    hal.delay_ns = hal_delay_ns;
    hubus_master_cfg_default(&cfg);
    hubus_master_init(&master, &hal, &cfg);
    TEST_ASSERT_TRUE(hubus_updater_init(MCID, sim.clk, sim.data));
    dev = hubus_sim_add(&sim, &hubus_updater_core);
    boot(); /* empty flash: the updater stays */
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_startup(&master));
    make_image(image_a, sizeof image_a, 0x0F, 1);
    make_image(image_b, sizeof image_b, 0x12, 2);
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT(0, sim.violations);
}

/* ---- tests ------------------------------------------------------------------------------------------------------ */

static void test_updater_announces_itself(void)
{
    hubus_slave_frame_t f;
    const uint8_t *p = &f.payload[HUBUS_INFO_APP];

    TEST_ASSERT_TRUE(in_updater);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_info(&master, MCID, &f));
    TEST_ASSERT_EQUAL_HEX8(HUBUS_CAP_UPDATER | HUBUS_CAP_STATE, f.payload[HUBUS_INFO_CAPS]);
    TEST_ASSERT_EQUAL_UINT(HUBUS_INFO_APP + HUBUS_UPD_INFO_LEN, f.len);
    TEST_ASSERT_EQUAL_HEX8(0x1E, p[HUBUS_UPD_INFO_SIGNATURE]);
    TEST_ASSERT_EQUAL_UINT8(PAGE, p[HUBUS_UPD_INFO_PAGE]);
    TEST_ASSERT_EQUAL_HEX8(0x18, p[HUBUS_UPD_INFO_APP_SIZE]); /* 0x1800 */
    TEST_ASSERT_EQUAL_HEX8(HUBUS_UPD_FLAG_RESET_VECTOR, p[HUBUS_UPD_INFO_FLAGS]);
    TEST_ASSERT_EQUAL_HEX8(0x18, p[HUBUS_UPD_INFO_UPDATER]);
}

static void test_update_from_updater_then_from_application(void)
{
    hubus_update_image_t a = image(image_a, sizeof image_a);
    hubus_update_image_t b = image(image_b, sizeof image_b);
    hubus_update_result_t res;
    hubus_slave_frame_t f;

    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_update(&master, MCID, &a, NULL, NULL, &res));
    assert_flash_holds(image_a, sizeof image_a, 0x0F);
    /* invalidation, 16 pages, info page */
    TEST_ASSERT_EQUAL_UINT16(18, res.pages);
    TEST_ASSERT_EQUAL_UINT16(0, res.retries);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_info(&master, MCID, &f));
    TEST_ASSERT_EQUAL_HEX8(HUBUS_CAP_UPDATE | HUBUS_CAP_STATE, f.payload[HUBUS_INFO_CAPS]);

    /* The application switches to its updater on UPDATE_ENTER. */
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_update(&master, MCID, &b, NULL, NULL, &res));
    assert_flash_holds(image_b, sizeof image_b, 0x12);
    TEST_ASSERT_EQUAL_UINT8(MCID, eeprom_mcid);
}

static void test_enter_needs_key_and_exact_addressing(void)
{
    static const uint8_t key[2] = {HUBUS_UPDATE_KEY0, HUBUS_UPDATE_KEY1};
    static const uint8_t wrong[2] = {HUBUS_UPDATE_KEY0, 0x00};
    hubus_update_image_t a = image(image_a, sizeof image_a);

    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_update(&master, MCID, &a, NULL, NULL, NULL));
    TEST_ASSERT_FALSE(in_updater);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&master, 0, 0, HUBUS_OP_UPDATE_ENTER, key, 2));
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&master, 3, MCID, HUBUS_OP_UPDATE_ENTER, key, 2));
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&master, 6, MCID, HUBUS_OP_UPDATE_ENTER, wrong, 2));
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&master, 6, MCID, HUBUS_OP_UPDATE_ENTER, key, 1));
    TEST_ASSERT_FALSE(in_updater);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&master, 6, MCID, HUBUS_OP_UPDATE_ENTER, key, 2));
    TEST_ASSERT_TRUE(in_updater);
}

static void test_image_checks(void)
{
    hubus_update_image_t img = image(image_a, sizeof image_a);
    hubus_update_result_t res;
    static uint8_t big[0x1800];
    uint8_t first;
    unsigned writes;

    img.signature[0] = 0x1E;
    img.signature[1] = 0x93;
    img.signature[2] = 0x06; /* ATtiny45 */
    TEST_ASSERT_EQUAL(HUBUS_ERR_UPDATE, hubus_master_update(&master, MCID, &img, NULL, NULL, &res));
    TEST_ASSERT_EQUAL(HUBUS_UPD_REASON_SIGNATURE, res.reason);

    make_image(big, sizeof big, 0x0F, 3);
    img = image(big, 0x1800 - PAGE + 1); /* one byte into the info page */
    TEST_ASSERT_EQUAL(HUBUS_ERR_UPDATE, hubus_master_update(&master, MCID, &img, NULL, NULL, &res));
    TEST_ASSERT_EQUAL(HUBUS_UPD_REASON_TOO_LARGE, res.reason);
    img = image(big, 0x1800 - PAGE); /* the largest application fits */
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_update(&master, MCID, &img, NULL, NULL, &res));

    writes = flash_writes;
    first = image_a[1];
    image_a[1] = 0x94; /* JMP instead of RJMP */
    img = image(image_a, sizeof image_a);
    TEST_ASSERT_EQUAL(HUBUS_ERR_UPDATE, hubus_master_update(&master, MCID, &img, NULL, NULL, &res));
    TEST_ASSERT_EQUAL(HUBUS_UPD_REASON_BAD_IMAGE, res.reason);
    image_a[1] = first;
    TEST_ASSERT_EQUAL_UINT(writes, flash_writes); /* nothing written for a refused image */
}

static void test_bit_errors_are_retried(void)
{
    hubus_update_image_t a = image(image_a, sizeof image_a);
    hubus_update_result_t res;
    unsigned at, total = 0, retried = 0;

    /* One flipped bit at every 97th clock of a whole update: in a LOAD, a WRITE, a state read, an info request, or
     * UPDATE_LEAVE. Every update still succeeds, most of them with a repeated page. */
    for (at = 1;; at += 97) {
        memset(flash, 0xFF, sizeof flash);
        boot();
        rises = 0;
        last_clk = sim.clk;
        flip_at = at;
        sim.filter = flip_filter;
        TEST_ASSERT_EQUAL_MESSAGE(HUBUS_OK, hubus_master_update(&master, MCID, &a, NULL, NULL, &res),
                                  hubus_update_reason_str(res.reason));
        sim.filter = NULL;
        assert_flash_holds(image_a, sizeof image_a, 0x0F);
        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_startup(&master));
        if (rises < at) {
            break; /* the update ended before this clock */
        }
        total++;
        retried += res.retries ? 1u : 0u;
    }
    TEST_ASSERT_GREATER_THAN_UINT(50, total);
    TEST_ASSERT_GREATER_THAN_UINT(total / 2, retried);
}

static void test_interrupted_update_leaves_the_updater(void)
{
    hubus_update_image_t a = image(image_a, sizeof image_a);
    hubus_update_image_t b = image(image_b, sizeof image_b);
    hubus_update_result_t res;

    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_update(&master, MCID, &a, NULL, NULL, &res));
    /* Power fails while image B is written: after the invalidation and a few pages. */
    flash[HUBUS_UPD_INFO_ADDR] = 0xFF;
    memcpy(flash + 3 * PAGE, image_b + 3 * PAGE, PAGE);
    boot();
    TEST_ASSERT_TRUE(in_updater);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_update(&master, MCID, &b, NULL, NULL, &res));
    assert_flash_holds(image_b, sizeof image_b, 0x12);
}

static void test_updater_refuses_bad_pages(void)
{
    uint8_t args[HUBUS_MAX_PAYLOAD];
    hubus_slave_frame_t f;
    uint16_t crc;
    unsigned off;
    uint8_t page[PAGE];

    memset(page, 0x11, sizeof page);
    for (off = 0; off < PAGE; off += 16) {
        args[0] = (uint8_t)off;
        memcpy(&args[1], &page[off], 16);
        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&master, 6, MCID, HUBUS_OP_UPDATE_LOAD, args, 17));
    }
    crc = hubus_crc16(page, PAGE);

    /* the updater itself, a misaligned address, the first page without RJMP, a wrong CRC */
    {
        static const uint16_t addrs[] = {HUBUS_UPD_ADDR, 0x0041, 0x0000, 0x0040};
        static const uint8_t results[] = {HUBUS_UPD_ERR_ADDR, HUBUS_UPD_ERR_ADDR, HUBUS_UPD_ERR_VECTOR, HUBUS_UPD_ERR_CRC};
        unsigned i;

        for (i = 0; i < 4; i++) {
            uint16_t c = i == 3 ? (uint16_t)(crc ^ 1u) : crc;

            args[0] = (uint8_t)(addrs[i] >> 8);
            args[1] = (uint8_t)addrs[i];
            args[2] = (uint8_t)(c >> 8);
            args[3] = (uint8_t)c;
            TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&master, 6, MCID, HUBUS_OP_UPDATE_WRITE, args, 4));
            TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_state(&master, MCID, &f));
            TEST_ASSERT_EQUAL_UINT8(results[i], f.payload[HUBUS_UPD_STATE_RESULT]);
            TEST_ASSERT_EQUAL_UINT8(i + 1, f.payload[HUBUS_UPD_STATE_SEQ]);
        }
    }
    TEST_ASSERT_EQUAL_UINT(0, flash_writes);

    /* UPDATE_LEAVE without a valid application: the updater resets and stays */
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&master, 6, MCID, HUBUS_OP_UPDATE_LEAVE, NULL, 0));
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_info(&master, MCID, &f));
    TEST_ASSERT_TRUE(in_updater);
}

static void test_no_slave(void)
{
    hubus_update_image_t a = image(image_a, sizeof image_a);
    hubus_update_result_t res;

    hubus_sim_unplug(&sim, dev, 1);
    TEST_ASSERT_EQUAL(HUBUS_ERR_UPDATE, hubus_master_update(&master, MCID, &a, NULL, NULL, &res));
    TEST_ASSERT_EQUAL(HUBUS_UPD_REASON_NO_UPDATER, res.reason);
}

static void test_ihex(void)
{
    static const char hex[] = ":020000020000FC\n"
                              ":0400000001020304F2\r\n"
                              ":02000600AABB93\n"
                              ":00000001FF\n";
    uint8_t out[16];
    uint32_t len;

    TEST_ASSERT_EQUAL_INT(0, hubus_ihex_parse(hex, sizeof hex - 1, out, sizeof out, &len));
    TEST_ASSERT_EQUAL_UINT32(8, len);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x04, out[3]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[4]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, out[6]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, out[7]);
    TEST_ASSERT_EQUAL_INT(-1, hubus_ihex_parse(":0400000001020304F3\n", 20, out, sizeof out, &len)); /* checksum */
    TEST_ASSERT_EQUAL_INT(-1, hubus_ihex_parse(":02000F000102EC\n", 16, out, sizeof out, &len));    /* too far */
    TEST_ASSERT_EQUAL_INT(-1, hubus_ihex_parse("0400000001020304F2\n", 19, out, sizeof out, &len)); /* no colon */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_updater_announces_itself);
    RUN_TEST(test_update_from_updater_then_from_application);
    RUN_TEST(test_enter_needs_key_and_exact_addressing);
    RUN_TEST(test_image_checks);
    RUN_TEST(test_bit_errors_are_retried);
    RUN_TEST(test_interrupted_update_leaves_the_updater);
    RUN_TEST(test_updater_refuses_bad_pages);
    RUN_TEST(test_no_slave);
    RUN_TEST(test_ihex);
    return UNITY_END();
}
