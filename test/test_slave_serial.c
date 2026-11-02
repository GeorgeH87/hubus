/*
 * HUBUS_ENABLE_SERIAL: the serial number comes from RAM and follows the constant part of the HWID in SLAVE_INFO.
 */
#include <string.h>

#include "hubus_frame.h"
#include "hubus_sim.h"
#include "rawmaster.h"
#include "unity.h"

static hubus_sim_t sim;
static raw_t rm;
static hubus_slave_core_t core;
static const uint8_t hwid[] = {0xF3, 0x03, 0x01, 0x00};
static uint8_t serial[4];

void setUp(void)
{
    hubus_sim_init(&sim);
    raw_init(&rm, &sim);
    memset(serial, 0, sizeof serial);
}

void tearDown(void) {}

static hubus_core_config_t config(void)
{
    hubus_core_config_t cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = 9;
    cfg.hwid = hwid;
    cfg.hwid_len = sizeof hwid;
    cfg.serial = serial;
    cfg.serial_len = sizeof serial;
    return cfg;
}

static void test_the_serial_number_follows_the_hwid(void)
{
    hubus_core_config_t cfg = config();
    uint8_t buf[40];
    size_t n;

    serial[0] = 0x00;
    serial[1] = 0x00;
    serial[2] = 0x2A;
    serial[3] = 0x17;
    TEST_ASSERT_TRUE(hubus_core_init(&core, &cfg, sim.clk, sim.data));
    hubus_sim_add(&sim, &core);
    n = raw_request(&rm, HUBUS_M_REQUEST_INFO, 9, buf);
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT(3 + HUBUS_INFO_HWID + sizeof hwid + sizeof serial, n);
    TEST_ASSERT_TRUE(hubus_frame_decode(buf, n, NULL));
    TEST_ASSERT_EQUAL_MEMORY(hwid, &buf[2 + HUBUS_INFO_HWID], sizeof hwid);
    TEST_ASSERT_EQUAL_MEMORY(serial, &buf[2 + HUBUS_INFO_HWID + sizeof hwid], sizeof serial);
}

static void test_without_a_serial_number_only_the_hwid_is_sent(void)
{
    hubus_core_config_t cfg = config();
    uint8_t buf[40];

    cfg.serial = NULL;
    cfg.serial_len = 0;
    TEST_ASSERT_TRUE(hubus_core_init(&core, &cfg, sim.clk, sim.data));
    hubus_sim_add(&sim, &core);
    TEST_ASSERT_EQUAL_UINT(3 + HUBUS_INFO_HWID + sizeof hwid, raw_request(&rm, HUBUS_M_REQUEST_INFO, 9, buf));
    raw_stop(&rm, 0);
}

static void test_hwid_and_serial_number_must_fit_together(void)
{
    hubus_core_config_t cfg = config();

    cfg.serial_len = (uint8_t)(HUBUS_HWID_MAX_LEN - sizeof hwid);
    TEST_ASSERT_TRUE(hubus_core_init(&core, &cfg, sim.clk, sim.data));
    cfg.serial_len++;
    TEST_ASSERT_FALSE(hubus_core_init(&core, &cfg, sim.clk, sim.data));
    cfg.serial_len = 1;
    cfg.serial = NULL;
    TEST_ASSERT_FALSE(hubus_core_init(&core, &cfg, sim.clk, sim.data));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_serial_number_follows_the_hwid);
    RUN_TEST(test_without_a_serial_number_only_the_hwid_is_sent);
    RUN_TEST(test_hwid_and_serial_number_must_fit_together);
    return UNITY_END();
}
