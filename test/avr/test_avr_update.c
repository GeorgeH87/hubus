/*
 * Firmware update on a simulated ATtiny85 (docs/spec/06-operation.md section 6.8): the updater as it is built for the
 * device (updater/hubus_updater_avr.c), with self-programming (SPM), EEPROM, and the watchdog reset, against
 * hubus_master_update(). The application is the switch example with update support (ex_switch_update), loaded from
 * its HEX file as the master example does.
 *
 * Covers what test_update cannot: the reset vector rule and the jump to the updater in the generated code, the page
 * writes with SPM, the CRC check of the info page at boot, the hand-over from the application (UPDATE_ENTER inside its
 * interrupt handler), the MCID in EEPROM, and UPDATE_LEAVE by the watchdog. The MCID in the last EEPROM byte as
 * the programmer writes it, and its change over the bus (UPDATE_SET_MCID), with the switch taking it from there.
 * The LED strip with update support (ex_ledstrip_update) as well: nearly the whole application area, replaced while
 * its animation runs in an interruptible section.
 */
#include <stdio.h>
#include <string.h>

#include "avr_ioport.h"
#include "avr_slave.h"
#include "fixture.h"
#include "hubus_crc.h"
#include "hubus_update.h"
#include "ledvm.h"
#include "sim_avr.h"
#include "sim_io.h"

#define MCU "attiny85"
#define F_CPU_HZ 8000000u
#define HEX(name) HUBUS_AVR_FW_DIR "/" name ".hex"
#define MS 1000000u

#define FLASH_SIZE 8192u
#define UPDATER_ADDR 0x1800u
#define INFO_ADDR (UPDATER_ADDR - 64u)
#define BOOT_NS (50u * MS) /* updater start, CRC of the application (about 30 ms), C runtime of the application */

#define SWITCH 5u
#define UPDATER_DEFAULT_MCID 63u
#define SWITCH_TYPE 0x82u
#define SWITCH_UPDATE_VARIANT 0x02u
#define BUTTON_PIN 1 /* PB1 */

static const uint8_t attiny85_signature[3] = {0x1E, 0x93, 0x0B};

static fx_t fx;
static avr_bus_t bus;
static uint8_t updater[FLASH_SIZE];
static uint8_t app[FLASH_SIZE];
static uint32_t app_len;

void setUp(void)
{
    fx_init(&fx);
    avr_bus_init(&bus, &fx.sim);
    memset(app, 0xFF, sizeof app);
    app_len = avr_read_hex(HEX("ex_switch_update"), app, sizeof app);
}

void tearDown(void)
{
    fx_check(&fx);
    avr_bus_free(&bus);
}

/* A device as it comes from the programmer: only the updater image (the updater and the jump at address 0). */
static avr_slave_t *add_fresh(const char *updater_hex)
{
    avr_slave_t *d;

    memset(updater, 0xFF, sizeof updater);
    (void)avr_read_hex(updater_hex, updater, sizeof updater);
    d = avr_bus_add_flash(&bus, updater, sizeof updater, MCU, F_CPU_HZ);
    avr_raise_irq(avr_io_getirq(d->avr, AVR_IOCTL_IOPORT_GETIRQ('B'), BUTTON_PIN), 1); /* button released */
    return d;
}

static void boot(void)
{
    avr_bus_idle(&bus, BOOT_NS);
    fx_startup(&fx);
}

static hubus_update_image_t image_of(const uint8_t *data, uint32_t len)
{
    hubus_update_image_t img;

    memset(&img, 0, sizeof img);
    img.data = data;
    img.len = len;
    memcpy(img.signature, attiny85_signature, 3);
    return img;
}

static hubus_update_image_t image(void)
{
    return image_of(app, app_len);
}

/* HWID type the slave `mcid` announces, -1 if it does not answer. */
static int hwid_type(uint8_t mcid, uint8_t *variant)
{
    hubus_slave_frame_t f;

    if (hubus_master_request_info(&fx.master, mcid, &f) != HUBUS_OK || f.len <= HUBUS_INFO_HWID + HUBUS_HWID_VARIANT) {
        return -1;
    }
    if (variant) {
        *variant = f.payload[HUBUS_INFO_HWID + HUBUS_HWID_VARIANT];
    }
    return f.payload[HUBUS_INFO_HWID + HUBUS_HWID_TYPE];
}

static void expect_switch(uint8_t mcid)
{
    uint8_t variant = 0;

    TEST_ASSERT_EQUAL_INT(SWITCH_TYPE, hwid_type(mcid, &variant));
    TEST_ASSERT_EQUAL_HEX8(SWITCH_UPDATE_VARIANT, variant);
}

/* The flash of the device holds the image, with the jump to the updater at address 0 in place of its reset vector,
 * and the updater untouched. */
static void expect_flash_of(const avr_slave_t *d, const uint8_t *data, uint32_t len)
{
    uint16_t w0 = (uint16_t)(d->avr->flash[0] | d->avr->flash[1] << 8);

    TEST_ASSERT_EQUAL_HEX16((uint16_t)(updater[0] | updater[1] << 8), w0);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&data[2], &d->avr->flash[2], len - 2u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&updater[UPDATER_ADDR], &d->avr->flash[UPDATER_ADDR], FLASH_SIZE - UPDATER_ADDR);
}

static void expect_flash(const avr_slave_t *d)
{
    expect_flash_of(d, app, app_len);
}

static void update_image_ok(uint8_t mcid, hubus_update_image_t img, hubus_update_result_t *res)
{
    hubus_status_t r = hubus_master_update(&fx.master, mcid, &img, NULL, NULL, res);

    if (r != HUBUS_OK) {
        char msg[96];

        snprintf(msg, sizeof msg, "update: status %d, %s, page 0x%04x", (int)r, hubus_update_reason_str(res->reason),
                 (unsigned)res->page);
        TEST_FAIL_MESSAGE(msg);
    }
}

static void update_ok(uint8_t mcid, hubus_update_result_t *res)
{
    update_image_ok(mcid, image(), res);
}

/* ---- tests ------------------------------------------------------------------------------------------------------- */

/* The freshly programmed device waits in its updater; the master loads the switch, which then starts at every reset. */
static void test_fresh_device_gets_its_application(void)
{
    hubus_update_result_t res;
    avr_slave_t *d = add_fresh(HEX("fw_updater_5"));

    boot();
    TEST_ASSERT_EQUAL_HEX8(0xFF, avr_slave_eeprom_read(d, -1)); /* no MCID from an application yet */
    TEST_ASSERT_EQUAL_INT(HUBUS_HWID_TYPE_UPDATER, hwid_type(SWITCH, NULL));
    update_ok(SWITCH, &res);
    printf("update of %u bytes: %u pages, %u repeated, %.1f s on the bus\n", (unsigned)app_len, res.pages,
           res.retries, (double)fx.sim.time_ns / 1e9);
    TEST_ASSERT_EQUAL_UINT(0, res.retries);
    expect_switch(SWITCH);
    expect_flash(d);

    avr_slave_power_cycle(d);
    avr_bus_idle(&bus, BOOT_NS);
    expect_switch(SWITCH);
}

/* The running application hands over to the updater (UPDATE_ENTER), which keeps its MCID in EEPROM; the new image is
 * written and started. */
static void test_application_is_replaced(void)
{
    hubus_update_result_t res;
    avr_slave_t *d = add_fresh(HEX("fw_updater_5"));

    boot();
    update_ok(SWITCH, &res);
    expect_switch(SWITCH);
    TEST_ASSERT_EQUAL_HEX8(0xFF, avr_slave_eeprom_read(d, -1)); /* the updater ran without UPDATE_ENTER */

    update_ok(SWITCH, &res);
    TEST_ASSERT_EQUAL_UINT((app_len + 63u) / 64u + 2u, res.pages); /* the invalidated info page, the image, the info page */
    expect_switch(SWITCH);
    expect_flash(d);
    TEST_ASSERT_EQUAL_HEX8(SWITCH, avr_slave_eeprom_read(d, -1));
}

/* The bus breaks off in the middle of the update and the device loses power: the old application was invalidated
 * first, so the device starts its updater, under the MCID it got from the application, and the update can be repeated. */
static avr_slave_t *cut_dev;
static uint16_t cut_at;

static void cut_progress(void *ctx, uint16_t done, uint16_t total)
{
    (void)ctx;
    (void)total;
    if (done == cut_at) {
        hubus_sim_unplug(&fx.sim, cut_dev->idx, 1);
    }
}

static void test_interrupted_update_is_repeated(void)
{
    hubus_update_result_t res;
    hubus_update_image_t img = image();
    avr_slave_t *d = add_fresh(HEX("fw_updater"));

    /* First install under the default MCID of the updater: the switch starts, but under its own MCID, so the master
     * cannot see it start (docs/slave-library.md, firmware update). */
    boot();
    TEST_ASSERT_EQUAL_INT(HUBUS_HWID_TYPE_UPDATER, hwid_type(UPDATER_DEFAULT_MCID, NULL));
    TEST_ASSERT_EQUAL(HUBUS_ERR_UPDATE, hubus_master_update(&fx.master, UPDATER_DEFAULT_MCID, &img, NULL, NULL, &res));
    TEST_ASSERT_EQUAL(HUBUS_UPD_REASON_NOT_STARTED, res.reason);
    expect_switch(SWITCH);

    cut_dev = d;
    cut_at = 20;
    TEST_ASSERT_NOT_EQUAL(HUBUS_OK, hubus_master_update(&fx.master, SWITCH, &img, cut_progress, NULL, &res));
    hubus_sim_unplug(&fx.sim, d->idx, 0);
    avr_slave_power_cycle(d);
    avr_bus_idle(&bus, BOOT_NS);
    TEST_ASSERT_EQUAL_INT(HUBUS_HWID_TYPE_UPDATER, hwid_type(SWITCH, NULL));
    TEST_ASSERT_EQUAL_HEX8(SWITCH, avr_slave_eeprom_read(d, -1));

    update_ok(SWITCH, &res);
    expect_switch(SWITCH);
    expect_flash(d);
}

/* ---- MCID -------------------------------------------------------------------------------------------------------- */

#define FLASHED_MCID 14u
#define NEW_MCID 20u

static void set_mcid_ok(uint8_t mcid, uint8_t new_mcid)
{
    hubus_update_result_t res;
    hubus_status_t r = hubus_master_set_mcid(&fx.master, mcid, new_mcid, NULL, 0, &res);

    if (r != HUBUS_OK) {
        char msg[96];

        snprintf(msg, sizeof msg, "set_mcid: status %d, %s", (int)r, hubus_update_reason_str(res.reason));
        TEST_FAIL_MESSAGE(msg);
    }
}

/* The recommended way: one updater image for all devices, the MCID in the last EEPROM byte from the programmer. The
 * updater answers to it, and so does the switch it loads. */
static void test_mcid_from_the_programmer(void)
{
    hubus_update_result_t res;
    avr_slave_t *d = add_fresh(HEX("fw_updater"));

    avr_slave_eeprom_write(d, -1, FLASHED_MCID);
    boot();
    TEST_ASSERT_EQUAL_INT(HUBUS_HWID_TYPE_UPDATER, hwid_type(FLASHED_MCID, NULL));
    TEST_ASSERT_EQUAL_INT(-1, hwid_type(UPDATER_DEFAULT_MCID, NULL));
    update_ok(FLASHED_MCID, &res);
    expect_switch(FLASHED_MCID);
    TEST_ASSERT_EQUAL_INT(-1, hwid_type(SWITCH, NULL));
}

/* A fresh device under the default MCID of the updater gets its MCID over the bus first, then its application, which
 * starts under it. */
static void test_fresh_device_gets_its_mcid_over_the_bus(void)
{
    hubus_update_result_t res;
    avr_slave_t *d = add_fresh(HEX("fw_updater"));

    boot();
    TEST_ASSERT_EQUAL_INT(HUBUS_HWID_TYPE_UPDATER, hwid_type(UPDATER_DEFAULT_MCID, NULL));
    set_mcid_ok(UPDATER_DEFAULT_MCID, NEW_MCID);
    TEST_ASSERT_EQUAL_INT(HUBUS_HWID_TYPE_UPDATER, hwid_type(NEW_MCID, NULL));
    TEST_ASSERT_EQUAL_INT(-1, hwid_type(UPDATER_DEFAULT_MCID, NULL));
    avr_bus_idle(&bus, 10u * MS); /* the EEPROM write */
    TEST_ASSERT_EQUAL_HEX8(NEW_MCID, avr_slave_eeprom_read(d, -1));

    update_ok(NEW_MCID, &res);
    expect_switch(NEW_MCID);
}

/* The running switch moves to another MCID and keeps it after a power cycle; a firmware update keeps it as well. */
static void test_application_gets_another_mcid(void)
{
    hubus_update_result_t res;
    avr_slave_t *d = add_fresh(HEX("fw_updater_5"));

    boot();
    update_ok(SWITCH, &res);
    expect_switch(SWITCH);
    set_mcid_ok(SWITCH, NEW_MCID);
    expect_switch(NEW_MCID);
    TEST_ASSERT_EQUAL_INT(-1, hwid_type(SWITCH, NULL));
    TEST_ASSERT_EQUAL_HEX8(NEW_MCID, avr_slave_eeprom_read(d, -1));

    avr_slave_power_cycle(d);
    avr_bus_idle(&bus, BOOT_NS);
    expect_switch(NEW_MCID);

    update_ok(NEW_MCID, &res);
    expect_switch(NEW_MCID);
    expect_flash(d);
}

/* ---- LED strip --------------------------------------------------------------------------------------------------- */

#define STRIP 11u
#define STRIP_TYPE 0x84u
#define STRIP_UPDATE_VARIANT 0x02u
#define OP_PROG_WRITE 0x43
#define OP_PROG_RUN 0x44
#define STRIP_PROG_RUNNING 1u

static uint8_t strip_app[FLASH_SIZE];

static void expect_strip(uint8_t mcid)
{
    uint8_t variant = 0;

    TEST_ASSERT_EQUAL_INT(STRIP_TYPE, hwid_type(mcid, &variant));
    TEST_ASSERT_EQUAL_HEX8(STRIP_UPDATE_VARIANT, variant);
}

/* Byte 2 of the state of the strip: its program. */
static uint8_t strip_program(void)
{
    hubus_slave_frame_t f;

    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_state(&fx.master, STRIP, &f));
    TEST_ASSERT_EQUAL_UINT8(3, f.len);
    return f.payload[2];
}

/* The strip as shipped with update support: loaded into a fresh device with its MCID from the programmer, then replaced
 * while an animation runs, so the hand-over (UPDATE_ENTER) comes in during an interruptible section. The new strip
 * starts dark, without a program. */
static void test_led_strip_is_replaced_while_it_animates(void)
{
    static const uint8_t prog[] = {LEDVM_I, LEDVM_K8, 10, LEDVM_ADD, LEDVM_K8, 0x20, LEDVM_K8, 0x30, LEDVM_RGB};
    uint8_t args[1 + sizeof prog];
    uint8_t run[4];
    hubus_update_result_t res;
    uint32_t len;
    avr_slave_t *d = add_fresh(HEX("fw_updater"));

    memset(strip_app, 0xFF, sizeof strip_app);
    len = avr_read_hex(HEX("ex_ledstrip_update"), strip_app, sizeof strip_app);
    TEST_ASSERT_TRUE(len > 0u && len <= INFO_ADDR);
    avr_slave_eeprom_write(d, -1, STRIP);
    boot();
    update_image_ok(STRIP, image_of(strip_app, len), &res);
    printf("LED strip: %u bytes, %u pages, %.1f s on the bus\n", (unsigned)len, res.pages, (double)fx.sim.time_ns / 1e9);
    expect_strip(STRIP);
    expect_flash_of(d, strip_app, len);

    args[0] = 0;
    memcpy(&args[1], prog, sizeof prog);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, HUBUS_TARGET_LEN_EXACT, STRIP, OP_PROG_WRITE,
                                                          args, sizeof args));
    run[0] = sizeof prog;
    run[1] = hubus_crc8(prog, sizeof prog);
    run[2] = LEDVM_LAYOUT_GRB;
    run[3] = 1; /* a frame every millisecond: the strip is sending most of the time */
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, HUBUS_TARGET_LEN_EXACT, STRIP, OP_PROG_RUN, run,
                                                          sizeof run));
    avr_bus_idle(&bus, 20u * MS);
    TEST_ASSERT_EQUAL_UINT8(STRIP_PROG_RUNNING, strip_program());

    update_image_ok(STRIP, image_of(strip_app, len), &res);
    expect_strip(STRIP);
    expect_flash_of(d, strip_app, len);
    TEST_ASSERT_EQUAL_HEX8(STRIP, avr_slave_eeprom_read(d, -1));
    TEST_ASSERT_EQUAL_UINT8(0, strip_program());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fresh_device_gets_its_application);
    RUN_TEST(test_application_is_replaced);
    RUN_TEST(test_interrupted_update_is_repeated);
    RUN_TEST(test_mcid_from_the_programmer);
    RUN_TEST(test_fresh_device_gets_its_mcid_over_the_bus);
    RUN_TEST(test_application_gets_another_mcid);
    RUN_TEST(test_led_strip_is_replaced_while_it_animates);
    return UNITY_END();
}
