/*
 * Blind sections (docs/spec/07-slave.md section 7.8): a slave that sees no pin events for a while after a command,
 * then resynchronizes. Whatever the bus did meanwhile and wherever the section ends, the slave must stay passive until
 * the next START and must not disturb the other slaves.
 */
#include <string.h>

#include "fixture.h"

static fx_t fx;

void setUp(void)
{
    fx_init(&fx);
}

void tearDown(void)
{
    fx_check(&fx);
}

/* ---- blind section of one slave, driven from the slave's view of the bus ------------------------------------------ */

typedef struct {
    unsigned victim;
    unsigned begin_after_stop; /* the section begins right after the STOP with this number (sim.n_stop) */
    unsigned skip;             /* line changes the slave does not see */
    int phase;                 /* 0 waiting, 1 blind, 2 resynchronized */
    unsigned skipped;
    unsigned start_at_resync;  /* sim.n_start when the section ended */
    unsigned stop_at_resync;
    unsigned early_rx;         /* receiving before a START after the section: misread frame */
    unsigned early_drive;      /* DATA pulled low before a START after the section */
} blind_t;

static blind_t bl;

static int blind_filter(hubus_sim_t *sim, unsigned slave, uint8_t *clk, uint8_t *data, void *ctx)
{
    blind_t *b = ctx;

    if (slave != b->victim) {
        return 1;
    }
    if (b->phase == 0) {
        if (sim->n_stop >= b->begin_after_stop) {
            TEST_ASSERT_TRUE(hubus_core_can_blind(sim->slave[slave]));
            b->phase = 1; /* this delivery is the STOP itself; everything after it goes unseen */
        }
        return 1;
    }
    if (b->phase == 1) {
        if (b->skipped < b->skip) {
            b->skipped++;
            return 0;
        }
        hubus_core_resync(sim->slave[slave], *clk, *data);
        b->phase = 2;
        b->start_at_resync = sim->n_start;
        b->stop_at_resync = sim->n_stop;
    }
    return 1;
}

/* Checks the victim after every pin event it sees. */
static void victim_pins(hubus_slave_core_t *c, uint8_t clk, uint8_t data)
{
    hubus_core_pins(c, clk, data);
    if (bl.phase == 2 && fx.sim.n_start == bl.start_at_resync) {
        if (c->state == HUBUS_ST_RX) {
            bl.early_rx++;
        }
        if (hubus_core_data_low(c)) {
            bl.early_drive++;
        }
    }
}

static void blind_arm(unsigned victim, unsigned skip)
{
    memset(&bl, 0, sizeof bl);
    bl.victim = victim;
    bl.begin_after_stop = fx.sim.n_stop + 1u; /* the STOP of the next transaction */
    bl.skip = skip;
    fx.sim.filter = blind_filter;
    fx.sim.filter_ctx = &bl;
    fx.sim.pins[victim] = victim_pins;
}

/* Ends a section that is still running (with the current levels) and removes the hooks. */
static void blind_disarm(void)
{
    if (bl.phase == 1) {
        hubus_core_resync(fx.sim.slave[bl.victim], fx.sim.clk, fx.sim.data);
        bl.phase = 2;
        hubus_sim_settle(&fx.sim);
    }
    fx.sim.filter = NULL;
    fx.sim.pins[bl.victim] = NULL;
}

static void counting_pins(hubus_slave_core_t *c, uint8_t clk, uint8_t data)
{
    bl.skipped++;
    hubus_core_pins(c, clk, data);
}

/*
 * Victim 33 gets a command and goes blind right after its STOP, before the master lowers CLK: the levels it saw last are
 * CLK and DATA high, the worst case for stale levels. Meanwhile the master serves the others: a burst with arbitration
 * and handoff slots (40 and 12 pending), a state read, and a command. Returns the number of line changes of that
 * traffic.
 */
static unsigned traffic_while_blind(unsigned skip, int count_only)
{
    const uint8_t op_victim[] = {0x21, 7};
    const uint8_t op_other = 0x30;
    hubus_burst_stats_t st;
    hubus_slave_frame_t f;
    unsigned victim, b, c, n;

    fx_init(&fx);
    victim = fx_add_full(&fx, 33, 0, 33, 0);
    b = fx_add(&fx, 40);
    c = fx_add_full(&fx, 12, 1, 12, 2);
    fx_startup(&fx);
    fx_write(&fx, b, 0x40);
    fx_write(&fx, c, 0x12);

    if (count_only) {
        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 33, op_victim[0], &op_victim[1], 1));
        memset(&bl, 0, sizeof bl);
        bl.victim = victim;
        fx.sim.pins[victim] = counting_pins;
    } else {
        blind_arm(victim, skip);
        fx.master.hold = 1; /* STOP without lowering CLK, as a master does before the next transaction */
        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 33, op_victim[0], &op_victim[1], 1));
        TEST_ASSERT_EQUAL_UINT8(1, fx.sim.clk);
        fx.master.hold = 0;
    }

    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(2, st.served);
    TEST_ASSERT_EQUAL_UINT8(40, fx.log[0].mcid);
    TEST_ASSERT_EQUAL_UINT8(12, fx.log[1].mcid);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_state(&fx.master, 12, &f));
    TEST_ASSERT_EQUAL_UINT8(2, f.len);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 40, op_other, NULL, 0));
    TEST_ASSERT_EQUAL_UINT(1, fx.app[b].commands);
    TEST_ASSERT_EQUAL_UINT(0, fx.app[c].commands);

    n = bl.skipped;
    if (count_only) {
        fx.sim.pins[victim] = NULL;
    } else {
        blind_disarm();
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, bl.early_rx, "frame misread after the blind section");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, bl.early_drive, "DATA driven after the blind section");
    }
    /* the victim executed its command once and nothing else, and is fully back */
    TEST_ASSERT_EQUAL_UINT(1, fx.app[victim].commands);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 33, 0x22, NULL, 0));
    TEST_ASSERT_EQUAL_UINT(2, fx.app[victim].commands);
    TEST_ASSERT_EQUAL_UINT8(0x22, fx.app[victim].last_op);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_info(&fx.master, 33, &f));
    fx_check(&fx);
    return n;
}

static void test_blind_section_ending_anywhere_in_foreign_traffic(void)
{
    unsigned total = traffic_while_blind(0, 1);
    unsigned skip;

    TEST_ASSERT_TRUE(total > 200u);
    for (skip = 0; skip < total; skip++) {
        traffic_while_blind(skip, 0);
    }
}

/* Without the resync the stale levels turn the first DATA change into a START: the danger the function exists for. */
/* Without the resync the stale levels turn the first DATA change into a START: the danger the function exists for. */
static void test_stale_levels_after_a_blind_section_misread_a_frame(void)
{
    hubus_slave_core_t c;
    hubus_core_config_t cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = 33;
    TEST_ASSERT_TRUE(hubus_core_init(&c, &cfg, 1, 1)); /* last seen: the STOP, CLK and DATA high */
    /* back in the middle of a frame, CLK high with a 1 bit on DATA */
    hubus_core_pins(&c, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_ST_RX, c.state); /* taken for a START */

    TEST_ASSERT_TRUE(hubus_core_init(&c, &cfg, 1, 1));
    hubus_core_resync(&c, 1, 0);
    hubus_core_pins(&c, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_ST_IGNORE, c.state); /* with the resync nothing changed */
    hubus_core_pins(&c, 0, 0);
    hubus_core_pins(&c, 0, 1);
    hubus_core_pins(&c, 1, 1);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_ST_IGNORE, c.state); /* further bits are no frame either */
    hubus_core_pins(&c, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_ST_RX, c.state); /* a real START brings it back */

    /* blind from the idle bus (CLK low) to a moment with CLK high: the START that follows must not be missed */
    TEST_ASSERT_TRUE(hubus_core_init(&c, &cfg, 0, 1));
    hubus_core_resync(&c, 1, 1);
    hubus_core_pins(&c, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_ST_RX, c.state);
}

/* A slave that became pending while blind does not announce itself on the idle bus afterwards: it waits for a STOP or
 * RESET. The master's RESET at the end of the blind time brings the announcement back. */
static void test_pending_slave_announces_after_the_reset_that_ends_the_blind_time(void)
{
    hubus_burst_stats_t st;
    unsigned victim = fx_add_full(&fx, 33, 0, 33, 0);

    fx_startup(&fx);
    blind_arm(victim, 1u); /* blind from the STOP of the command; the falling CLK edge after it goes unseen */
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 33, 0x21, NULL, 0));
    TEST_ASSERT_EQUAL_UINT8(0, fx.sim.clk);
    fx_write(&fx, victim, 0x34); /* the application writes while blind */
    blind_disarm();
    TEST_ASSERT_EQUAL_UINT8(HUBUS_ST_IGNORE, fx.core[victim].state);
    TEST_ASSERT_EQUAL_UINT8(1, fx.sim.data); /* no announcement on the idle bus */
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_service(&fx.master, fx_collect, &fx, &st));
    TEST_ASSERT_EQUAL_UINT(0, st.served);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[victim]));

    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_startup(&fx.master)); /* RESET */
    fx.n_log = 0;
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_service(&fx.master, fx_collect, &fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, st.served);
    TEST_ASSERT_EQUAL_UINT8(0x34, fx.log[0].first);
}

static void test_no_blind_section_while_the_slave_drives_data(void)
{
    unsigned victim = fx_add_full(&fx, 33, 0, 33, 0);
    hubus_slave_core_t *c = &fx.core[victim];

    fx_startup(&fx);
    TEST_ASSERT_TRUE(hubus_core_can_blind(c)); /* idle */
    fx_write(&fx, victim, 0x33);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_ST_EVENT, c->state);
    TEST_ASSERT_FALSE(hubus_core_can_blind(c)); /* signaling an event */
    c->state = HUBUS_ST_TURN;
    TEST_ASSERT_FALSE(hubus_core_can_blind(c)); /* owes an answer */
    c->state = HUBUS_ST_ARB;
    TEST_ASSERT_FALSE(hubus_core_can_blind(c));
    c->state = HUBUS_ST_TX;
    TEST_ASSERT_FALSE(hubus_core_can_blind(c));
    c->state = HUBUS_ST_RX;
    TEST_ASSERT_TRUE(hubus_core_can_blind(c)); /* DATA is released while receiving */
    c->state = HUBUS_ST_IGNORE;
    TEST_ASSERT_TRUE(hubus_core_can_blind(c));
    hubus_core_resync(c, fx.sim.clk, fx.sim.data);
    TEST_ASSERT_EQUAL_UINT8(0, hubus_core_data_low(c));
    hubus_sim_settle(&fx.sim);
}

static void test_resync_of_a_disabled_core_does_nothing(void)
{
    hubus_slave_core_t c;
    hubus_core_config_t cfg;

    memset(&cfg, 0, sizeof cfg);
    TEST_ASSERT_FALSE(hubus_core_init(&c, &cfg, 0, 1));
    hubus_core_resync(&c, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(0, c.clk);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_ST_IDLE, c.state);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_blind_section_ending_anywhere_in_foreign_traffic);
    RUN_TEST(test_stale_levels_after_a_blind_section_misread_a_frame);
    RUN_TEST(test_pending_slave_announces_after_the_reset_that_ends_the_blind_time);
    RUN_TEST(test_no_blind_section_while_the_slave_drives_data);
    RUN_TEST(test_resync_of_a_disabled_core_does_nothing);
    return UNITY_END();
}
