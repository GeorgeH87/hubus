/*
 * Buffer sizes are compile-time options. This test is built with
 * HUBUS_RX_BUF_SIZE=4 and HUBUS_TX_BUF_SIZE=6 (see test/CMakeLists.txt).
 */
#include <string.h>

#include "hubus_sim.h"
#include "rawmaster.h"
#include "unity.h"

static hubus_sim_t sim;
static raw_t rm;
static hubus_slave_core_t core;
static unsigned commands;
static unsigned captures;
static uint8_t last_len;
static const uint8_t status[3] = {1, 2, 3};

static void on_command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    (void)ctx;
    (void)opcode;
    (void)args;
    commands++;
    last_len = len;
}

static void on_capture(void *ctx)
{
    (void)ctx;
    captures++;
    (void)hubus_core_write(&core, status, sizeof status, 0);
}

void setUp(void)
{
    hubus_core_config_t cfg;

    hubus_sim_init(&sim);
    raw_init(&rm, &sim);
    commands = 0;
    captures = 0;
    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = 9;
    cfg.on_command = on_command;
    cfg.on_status_capture = on_capture;
    TEST_ASSERT_TRUE(hubus_core_init(&core, &cfg, sim.clk, sim.data));
    hubus_sim_add(&sim, &core);
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT(0, sim.violations);
}

static void send_cmd(const uint8_t *payload, uint8_t len)
{
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_SEND_COMMAND, HUBUS_TARGET_LEN_EXACT, 9, payload, len);
    raw_stop(&rm, 0);
}

static void test_sizes_are_configured(void)
{
    TEST_ASSERT_EQUAL_INT(4, HUBUS_RX_BUF_SIZE);
    TEST_ASSERT_EQUAL_INT(6, HUBUS_TX_BUF_SIZE);
}

static void test_info_announces_the_accepted_command_size(void)
{
    uint8_t buf[40];

    TEST_ASSERT_TRUE(raw_request(&rm, HUBUS_M_REQUEST_INFO, 9, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT8(4, buf[2 + HUBUS_INFO_MAX_CMD]);
}

static void test_command_that_fits_is_executed(void)
{
    const uint8_t cmd[4] = {0x10, 1, 2, 3};

    send_cmd(cmd, 4);
    TEST_ASSERT_EQUAL_UINT(1, commands);
    TEST_ASSERT_EQUAL_UINT8(3, last_len);
}

static void test_oversized_command_is_ignored_and_the_slave_stays_in_sync(void)
{
    const uint8_t big[5] = {0x10, 1, 2, 3, 4};
    const uint8_t ok[2] = {0x10, 1};
    const uint8_t op = HUBUS_OP_STATUS_CAPTURE;
    unsigned len;

    for (len = 5; len <= HUBUS_MAX_PAYLOAD; len += 13) {
        uint8_t large[HUBUS_MAX_PAYLOAD] = {0x10};
        send_cmd(large, (uint8_t)len);
    }
    send_cmd(big, 5);
    TEST_ASSERT_EQUAL_UINT(0, commands);
    send_cmd(ok, 2);
    TEST_ASSERT_EQUAL_UINT(1, commands);

    /* an oversized frame received to the end also leaves the following frames intact within one session */
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_SEND_COMMAND, HUBUS_TARGET_LEN_EXACT, 9, big, 5);
    raw_stop(&rm, 1);
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_SEND_COMMAND, HUBUS_TARGET_LEN_EXACT, 9, &op, 1);
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT(1, captures);
}

static void test_transmit_buffer_limit(void)
{
    uint8_t seven[7] = {0};
    uint8_t six[6] = {1, 2, 3, 4, 5, 6};
    uint8_t buf[40];

    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_TOO_LONG, hubus_sim_slave_write(&sim, 0, seven, 7, 0));
    TEST_ASSERT_FALSE(hubus_core_is_pending(&core));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&sim, 0, six, 6, 0));
    TEST_ASSERT_EQUAL_UINT(9, raw_request(&rm, HUBUS_M_REQUEST_DATA, 9, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_MEMORY(six, buf + 2, 6);
}

static volatile uint8_t live[2] = {1, 2};

/* A build without REQUEST_STATE: a slave configured with a state does not answer and does not announce it. */
static void test_state_support_is_compiled_out(void)
{
    hubus_core_config_t cfg;
    hubus_slave_core_t other;
    uint8_t buf[40];

    TEST_ASSERT_EQUAL_INT(0, HUBUS_ENABLE_STATE);
    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = 10;
    cfg.state = live;
    cfg.state_len = sizeof live;
    TEST_ASSERT_TRUE(hubus_core_init(&other, &cfg, sim.clk, sim.data));
    hubus_sim_add(&sim, &other);
    TEST_ASSERT_EQUAL_UINT(0, raw_request(&rm, HUBUS_M_REQUEST_STATE, 10, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_TRUE(raw_request(&rm, HUBUS_M_REQUEST_INFO, 10, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT8(0, buf[2 + HUBUS_INFO_CAPS] & HUBUS_CAP_STATE);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sizes_are_configured);
    RUN_TEST(test_info_announces_the_accepted_command_size);
    RUN_TEST(test_command_that_fits_is_executed);
    RUN_TEST(test_oversized_command_is_ignored_and_the_slave_stays_in_sync);
    RUN_TEST(test_transmit_buffer_limit);
    RUN_TEST(test_state_support_is_compiled_out);
    return UNITY_END();
}
