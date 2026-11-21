/*
 * Records bus scenarios as value change dumps for the sigrok decoder test (test/tools/sigrok.sh): the real master and
 * slave cores on the simulated bus, one VCD file per scenario in the directory given as the first argument. What the
 * decoder has to find in each file is in test/sigrok/<scenario>.txt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture.h"

static fx_t fx;
static const char *out_dir = ".";

static void record(const char *name)
{
    char path[512];

    snprintf(path, sizeof path, "%s/%s.vcd", out_dir, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hubus_sim_vcd_open(&fx.sim, path), path);
}

void setUp(void)
{
    fx_init(&fx);
}

void tearDown(void)
{
    hubus_sim_vcd_close(&fx.sim);
    fx_check(&fx);
}

/* Forces DATA low during the clock pulse with this number of rising edges (counted from the start of the dump): a bit
 * error on the line. */
static unsigned flip_rise;

static void flip_bit(hubus_sim_t *sim, uint64_t until_ns, void *ctx)
{
    (void)until_ns;
    (void)ctx;
    if (!sim->clk && sim->clk_rises + 1u == flip_rise && !sim->extra_low) {
        sim->extra_low = 1;
        hubus_sim_settle(sim);
    } else if (!sim->clk && sim->clk_rises == flip_rise && sim->extra_low) {
        sim->extra_low = 0;
        hubus_sim_settle(sim);
    }
}

static void test_reset(void)
{
    fx_add(&fx, 3);
    record("reset");
    fx_startup(&fx);
}

static void test_info(void)
{
    hubus_slave_frame_t f;

    fx_add(&fx, 3);
    fx_startup(&fx);
    record("info");
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_info(&fx.master, 3, &f));
}

static void test_state(void)
{
    hubus_slave_frame_t f;

    fx_add_full(&fx, 5, 1, 5, 2);
    fx_startup(&fx);
    record("state");
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_state(&fx.master, 5, &f));
}

static void test_no_response(void)
{
    hubus_slave_frame_t f;

    fx_add(&fx, 3);
    fx_startup(&fx);
    record("no_response");
    TEST_ASSERT_EQUAL(HUBUS_ERR_NO_RESPONSE, hubus_master_request_info(&fx.master, 7, &f));
}

static void test_commands(void)
{
    const uint8_t on = 0x01;

    fx_add(&fx, 3);
    fx_startup(&fx);
    record("commands");
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, HUBUS_TARGET_LEN_EXACT, 3, 0x10, &on, 1));
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 0, 0, HUBUS_OP_STATUS_CAPTURE, NULL, 0));
}

/* The burst of section 5.6 of the specification: four pending slaves, served in descending MCID order. */
static void test_burst(void)
{
    const uint8_t mcids[] = {15, 25, 40, 42};
    hubus_burst_stats_t st;
    unsigned i;

    for (i = 0; i < sizeof mcids; i++) {
        fx_add(&fx, mcids[i]);
    }
    fx_startup(&fx);
    record("burst");
    for (i = 0; i < sizeof mcids; i++) {
        fx_write(&fx, i, (uint8_t)(0xA0 + mcids[i]));
    }
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(4, st.served);
}

static void test_fragment(void)
{
    const uint8_t part[2] = {0x11, 0x22};
    hubus_burst_stats_t st;
    unsigned s = fx_add(&fx, 9);

    fx_startup(&fx);
    record("fragment");
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&fx.sim, s, part, sizeof part, 1));
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
}

/* A bit of the response of slave 3 is pulled low on the line: the master and the decoder see a wrong CRC. */
static void test_bit_error(void)
{
    hubus_slave_frame_t f;

    fx_add(&fx, 3);
    fx_startup(&fx);
    fx.sim.clk_rises = 0;
    record("bit_error");
    flip_rise = 1u + 24u + 1u + 16u + 4u; /* START, master frame, turnaround, slave header, bit 4 of byte 0 (0x00) */
    fx.sim.advance = flip_bit;
    TEST_ASSERT_NOT_EQUAL(HUBUS_OK, hubus_master_request_info(&fx.master, 3, &f));
    fx.sim.advance = NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        out_dir = argv[1];
    }
    UNITY_BEGIN();
    RUN_TEST(test_reset);
    RUN_TEST(test_info);
    RUN_TEST(test_state);
    RUN_TEST(test_no_response);
    RUN_TEST(test_commands);
    RUN_TEST(test_burst);
    RUN_TEST(test_fragment);
    RUN_TEST(test_bit_error);
    return UNITY_END();
}
