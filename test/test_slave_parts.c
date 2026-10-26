/*
 * Slaves built without the event buffer (HUBUS_ENABLE_EVENTS=0). This test is built twice (see test/CMakeLists.txt):
 * as a passive slave that is only read (HUBUS_ENABLE_STATE=1, for example a sensor) and as a slave that only executes
 * commands (HUBUS_ENABLE_STATE=0, for example a relay whose state the master keeps in a cache).
 */
#include <string.h>

#include "hubus_sim.h"
#include "rawmaster.h"
#include "unity.h"

#if HUBUS_ENABLE_EVENTS
#error "test_slave_parts is built without the event buffer"
#endif

static hubus_sim_t sim;
static raw_t rm;
static hubus_slave_core_t core[3];
static unsigned commands[3];
static unsigned captures;
static volatile uint8_t live[3][2];
static const uint8_t info_bytes[] = {0x03, 0x01};

static void on_command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    unsigned i = (unsigned)(uintptr_t)ctx;

    (void)opcode;
    (void)args;
    (void)len;
    commands[i]++;
}

static void on_capture(void *ctx)
{
    (void)ctx;
    captures++;
}

static void add(unsigned i, uint8_t mcid)
{
    hubus_core_config_t cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = mcid;
    cfg.info = info_bytes;
    cfg.info_len = sizeof info_bytes;
    cfg.state = live[i];
    cfg.state_len = sizeof live[i];
    cfg.on_status_capture = on_capture; /* ignored without the event buffer */
    cfg.on_command = on_command;
    cfg.ctx = (void *)(uintptr_t)i;
    live[i][0] = mcid;
    live[i][1] = (uint8_t)(mcid + 100u);
    TEST_ASSERT_TRUE(hubus_core_init(&core[i], &cfg, sim.clk, sim.data));
    hubus_sim_add(&sim, &core[i]);
}

void setUp(void)
{
    hubus_sim_init(&sim);
    raw_init(&rm, &sim);
    memset(commands, 0, sizeof commands);
    captures = 0;
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT(0, sim.violations);
}

static void send_cmd(uint8_t tlen, uint8_t prefix, uint8_t opcode)
{
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_SEND_COMMAND, tlen, prefix, &opcode, 1);
    raw_stop(&rm, 0);
}

static void test_info_announces_neither_events_nor_capture(void)
{
    uint8_t buf[40];

    add(0, 9);
    TEST_ASSERT_EQUAL_UINT(3 + HUBUS_INFO_APP + sizeof info_bytes, raw_request(&rm, HUBUS_M_REQUEST_INFO, 9, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_TRUE(hubus_frame_decode(buf, 3 + HUBUS_INFO_APP + sizeof info_bytes, NULL));
    TEST_ASSERT_EQUAL_UINT8(0, buf[2 + HUBUS_INFO_CAPS] & (HUBUS_CAP_EVENTS | HUBUS_CAP_STATUS_CAPTURE));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_ENABLE_STATE ? HUBUS_CAP_STATE : 0u, buf[2 + HUBUS_INFO_CAPS] & HUBUS_CAP_STATE);
    TEST_ASSERT_EQUAL_MEMORY(info_bytes, buf + 2 + HUBUS_INFO_APP, sizeof info_bytes);
}

static void test_request_data_is_never_answered(void)
{
    uint8_t buf[40];

    add(0, 9);
    TEST_ASSERT_EQUAL_UINT(0, raw_request(&rm, HUBUS_M_REQUEST_DATA, 9, buf));
    raw_stop(&rm, 0);
    /* nor does the slave contend in a group request (burst) */
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_REQUEST_DATA, 0, 0, NULL, 0);
    raw_turnaround(&rm);
    TEST_ASSERT_EQUAL_HEX16(0, raw_recv_arb(&rm, 0));
    TEST_ASSERT_EQUAL_UINT(0, raw_recv_frame(&rm, buf));
    raw_stop(&rm, 0);
}

static void test_status_capture_is_ignored_and_the_slave_never_signals(void)
{
    unsigned i;

    add(0, 9);
    send_cmd(HUBUS_TARGET_LEN_EXACT, 9, HUBUS_OP_STATUS_CAPTURE);
    TEST_ASSERT_EQUAL_UINT(0, captures);
    /* idle clock pulses after the STOP: a slave with pending data would pull DATA low here */
    for (i = 0; i < 4; i++) {
        raw_tick(&rm);
        TEST_ASSERT_EQUAL_UINT8(1, sim.data);
    }
    TEST_ASSERT_EQUAL_UINT(HUBUS_ST_IDLE, core[0].state);
}

static void test_commands_are_executed_exact_and_by_group(void)
{
    add(0, 9);
    add(1, 10);
    send_cmd(HUBUS_TARGET_LEN_EXACT, 9, 0x10);
    TEST_ASSERT_EQUAL_UINT(1, commands[0]);
    TEST_ASSERT_EQUAL_UINT(0, commands[1]);
    send_cmd(0, 0, 0x10); /* broadcast */
    TEST_ASSERT_EQUAL_UINT(2, commands[0]);
    TEST_ASSERT_EQUAL_UINT(1, commands[1]);
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
}

#if HUBUS_ENABLE_STATE
static void test_state_is_read_exactly(void)
{
    uint8_t buf[40];
    hubus_header_t h;

    add(0, 9);
    live[0][1] = 77;
    TEST_ASSERT_EQUAL_UINT(3 + 2, raw_request(&rm, HUBUS_M_REQUEST_STATE, 9, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_TRUE(hubus_frame_decode(buf, 5, &h));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_S_DATA, h.type);
    TEST_ASSERT_EQUAL_UINT8(0, h.field); /* MORE = 0, GEN = 00 */
    TEST_ASSERT_EQUAL_UINT8(9, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(77, buf[3]);
}

/* Group REQUEST_STATE: the highest MCID wins, the loser drives its handoff slot and keeps quiet. */
static void test_state_is_read_by_group(void)
{
    uint8_t buf[40];
    uint16_t slots;

    add(0, 9);  /* 001001 */
    add(1, 12); /* 001100 */
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_REQUEST_STATE, 0, 0, NULL, 0);
    raw_turnaround(&rm);
    slots = raw_recv_arb(&rm, 0);
    TEST_ASSERT_EQUAL_UINT(3 + 2, raw_recv_frame(&rm, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_TRUE(hubus_frame_decode(buf, 5, NULL));
    TEST_ASSERT_EQUAL_UINT8(12, buf[2]);
    TEST_ASSERT_EQUAL_HEX16((1u << 4) | (1u << 6) | (1u << 7), slots); /* 9 lost at bit 2 and drove its handoff slot */
}
#else
static void test_state_is_not_answered(void)
{
    uint8_t buf[40];

    add(0, 9);
    TEST_ASSERT_EQUAL_UINT(0, raw_request(&rm, HUBUS_M_REQUEST_STATE, 9, buf));
    raw_stop(&rm, 0);
}
#endif

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_info_announces_neither_events_nor_capture);
    RUN_TEST(test_request_data_is_never_answered);
    RUN_TEST(test_status_capture_is_ignored_and_the_slave_never_signals);
    RUN_TEST(test_commands_are_executed_exact_and_by_group);
#if HUBUS_ENABLE_STATE
    RUN_TEST(test_state_is_read_exactly);
    RUN_TEST(test_state_is_read_by_group);
#else
    RUN_TEST(test_state_is_not_answered);
#endif
    return UNITY_END();
}
