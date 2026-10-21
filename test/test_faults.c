/*
 * Fault injection: bit errors, lost and extra clock edges, lost STOP, stuck
 * line, duplicate MCID, and an event signal racing a START. Every accepted
 * rare case must recover as specified (docs/PROTOCOL.md section 7,
 * examples/ERROR_SCENARIOS.md).
 */
#include <string.h>

#include "fixture.h"
#include "rawmaster.h"

static fx_t fx;

static void reset_faults(void);

void setUp(void)
{
    reset_faults();
    fx_init(&fx);
}

void tearDown(void)
{
    fx_check(&fx);
}

static void assert_bus_idle(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, fx.sim.clk);
    TEST_ASSERT_EQUAL_UINT8(1, fx.sim.data);
}

/* ---- hooks into the master HAL (act just before the n-th rising CLK edge) -- */

typedef struct {
    hubus_master_hal_t real;
    unsigned rises;     /* rising edges counted so far */
    unsigned fire_at;   /* fire before this rising edge (0 = never) */
    void (*fire)(void *);
    void *fire_ctx;
    unsigned release_at_fall; /* clear extra_low at the next falling edge after firing */
} hook_t;

static hook_t hook;
static hubus_master_hal_t orig_hal;
static int hooked;

static void hook_set_clk(void *ctx, uint8_t high)
{
    hook_t *h = ctx;

    if (high) {
        h->rises++;
        if (h->fire_at && h->rises == h->fire_at) {
            h->fire(h->fire_ctx);
        }
    }
    h->real.set_clk(h->real.ctx, high);
    if (!high && h->release_at_fall) {
        h->release_at_fall = 0;
        fx.sim.extra_low = 0; /* the foreign node lets go while CLK is low */
        hubus_sim_settle(&fx.sim);
    }
}

static void hook_set_data_low(void *ctx, uint8_t low)
{
    hook_t *h = ctx;

    h->real.set_data_low(h->real.ctx, low);
}

static uint8_t hook_read_data(void *ctx)
{
    hook_t *h = ctx;

    return h->real.read_data(h->real.ctx);
}

static void hook_delay_ns(void *ctx, uint32_t ns)
{
    hook_t *h = ctx;

    h->real.delay_ns(h->real.ctx, ns);
}

static void hook_install(unsigned fire_at, void (*fire)(void *), void *fire_ctx)
{
    memset(&hook, 0, sizeof hook);
    if (!hooked) {
        orig_hal = fx.master.hal;
        hooked = 1;
    }
    hook.real = orig_hal;
    hook.fire_at = fire_at;
    hook.fire = fire;
    hook.fire_ctx = fire_ctx;
    fx.master.hal.ctx = &hook;
    fx.master.hal.set_clk = hook_set_clk;
    fx.master.hal.set_data_low = hook_set_data_low;
    fx.master.hal.read_data = hook_read_data;
    fx.master.hal.delay_ns = hook_delay_ns;
}

/* ---- slave-view filters ------------------------------------------------------- */

typedef struct {
    unsigned victim;
    unsigned target_rise; /* absolute rising edge number (sim.clk_rises) */
    int active;
} view_fault_t;

static view_fault_t vf;

static int flip_bit_filter(hubus_sim_t *sim, unsigned slave, uint8_t *clk, uint8_t *data, void *ctx)
{
    view_fault_t *f = ctx;

    (void)clk;
    if (f->active && slave == f->victim && sim->clk && sim->clk_rises == f->target_rise) {
        *data = (uint8_t)!*data;
    }
    return 1;
}

static int lost_pulse_filter(hubus_sim_t *sim, unsigned slave, uint8_t *clk, uint8_t *data, void *ctx)
{
    view_fault_t *f = ctx;

    (void)data;
    if (f->active && slave == f->victim && sim->clk && sim->clk_rises == f->target_rise) {
        *clk = 0; /* the slave never sees this pulse */
    }
    return 1;
}

/* ---- master read filters (white-box: they look at the slave being read) ----- */

typedef struct {
    hubus_slave_core_t *core;
    unsigned tx_bit;  /* flip the sample of the response bit that has this tx_bit count */
    unsigned slot;    /* flip the arbitration slot with this index */
    int want_level;   /* only flip if the line has this level */
    int active;
    unsigned hits;
    unsigned max_hits; /* 0: unlimited */
} read_fault_t;

static read_fault_t rf;

/* Number of response bits the slave has put on the line so far. */
static unsigned tx_bits_driven(const hubus_slave_core_t *c)
{
    return (unsigned)c->tx_idx * 8u - c->tx_nbits;
}

static uint8_t tx_bit_filter(hubus_sim_t *sim, uint8_t level, void *ctx)
{
    read_fault_t *f = ctx;

    (void)sim;
    if (f->active && (!f->max_hits || f->hits < f->max_hits) && f->core->state == HUBUS_ST_TX &&
        tx_bits_driven(f->core) == f->tx_bit) {
        f->hits++;
        return (uint8_t)!level;
    }
    return level;
}

static uint8_t slot_filter(hubus_sim_t *sim, uint8_t level, void *ctx)
{
    read_fault_t *f = ctx;

    (void)sim;
    /* the slave has already counted the slot whose bit the master is reading */
    if (f->active && f->core->state == HUBUS_ST_ARB && f->core->slot == f->slot + 1 && level == f->want_level) {
        f->hits++;
        return (uint8_t)!level;
    }
    return level;
}

/* ---- bit errors master -> slave -------------------------------------------- */

static void test_bit_flip_in_command_is_rejected_and_repeat_works(void)
{
    const uint8_t args[] = {1, 2};
    unsigned a = fx_add(&fx, 10);
    unsigned b = fx_add(&fx, 11);
    unsigned j;

    fx_startup(&fx);
    for (j = 0; j < 40; j += 3) { /* flip one bit at several positions of the frame */
        unsigned base = fx.sim.clk_rises;
        unsigned before = fx.app[a].commands;

        vf.victim = a;
        vf.target_rise = base + 2 + j; /* rise base+1 is the START, then the frame bits */
        vf.active = 1;
        fx.sim.filter = flip_bit_filter;
        fx.sim.filter_ctx = &vf;
        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 0, 0, 0x30, args, 2));
        /* frame = 2 header + 3 payload + CRC = 6 bytes = 48 bits: every position is covered by the CRC */
        TEST_ASSERT_EQUAL_UINT(before, fx.app[a].commands); /* CRC failure: not executed */
        TEST_ASSERT_EQUAL_UINT(2 * (j / 3) + 1, fx.app[b].commands); /* the unaffected slave executed every time */
        fx.sim.filter = NULL;
        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 0, 0, 0x30, args, 2));
        TEST_ASSERT_EQUAL_UINT(before + 1, fx.app[a].commands); /* the next START resynchronised it */
    }
    assert_bus_idle();
}

static void test_bit_flip_in_poll_leaves_slave_out_of_burst_but_not_forgotten(void)
{
    hubus_burst_stats_t st;
    uint8_t event = 0;
    unsigned a = fx_add(&fx, 30);
    unsigned b = fx_add(&fx, 20);

    fx_startup(&fx);
    fx_write(&fx, a, 1);
    fx_write(&fx, b, 2);
    /* slave b misreads the POLL_PENDING frame of the first poll */
    vf.victim = b;
    vf.target_rise = fx.sim.clk_rises + 3 + 5; /* inside the first frame (one tick may precede it) */
    vf.active = 1;
    fx.sim.filter = flip_bit_filter;
    fx.sim.filter_ctx = &vf;
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    fx.sim.filter = NULL;
    TEST_ASSERT_TRUE(fx.n_log >= 1);
    TEST_ASSERT_EQUAL_UINT8(30, fx.log[0].mcid);

    /* b is still pending and announces itself; the next burst serves it */
    if (hubus_core_is_pending(&fx.core[b])) {
        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_check_event(&fx.master, &event));
        TEST_ASSERT_EQUAL_UINT8(1, event);
        TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
        TEST_ASSERT_EQUAL_UINT(1, fx.n_log);
        TEST_ASSERT_EQUAL_UINT8(20, fx.log[0].mcid);
    }
    TEST_ASSERT_FALSE(hubus_core_is_pending(&fx.core[b]));
}

/* ---- lost and extra clock edges ----------------------------------------------- */

static void test_lost_clock_pulse_in_command_frame(void)
{
    const uint8_t args[] = {5};
    unsigned a = fx_add(&fx, 10);
    unsigned pulse;

    fx_startup(&fx);
    for (pulse = 0; pulse < 24; pulse += 5) {
        unsigned before = fx.app[a].commands;

        vf.victim = a;
        vf.target_rise = fx.sim.clk_rises + 2 + pulse;
        vf.active = 1;
        fx.sim.filter = lost_pulse_filter;
        fx.sim.filter_ctx = &vf;
        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 10, 0x30, args, 1));
        fx.sim.filter = NULL;
        TEST_ASSERT_EQUAL_UINT(before, fx.app[a].commands); /* a lost pulse corrupts the frame */
        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 10, 0x30, args, 1));
        TEST_ASSERT_EQUAL_UINT(before + 1, fx.app[a].commands); /* resynchronised by the next START */
    }
    assert_bus_idle();
}

static void glitch_slave0(void *ctx)
{
    (void)ctx;
    hubus_sim_glitch_clk(&fx.sim, 0);
}

static void test_extra_clock_pulse_in_command_frame(void)
{
    const uint8_t args[] = {5};
    unsigned a = fx_add(&fx, 10);
    unsigned k;

    fx_startup(&fx);
    for (k = 3; k < 25; k += 7) {
        unsigned before = fx.app[a].commands;

        hook_install(k, glitch_slave0, NULL);
        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 10, 0x30, args, 1));
        TEST_ASSERT_EQUAL_UINT(before, fx.app[a].commands);
        hook.fire_at = 0;
        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 10, 0x30, args, 1));
        TEST_ASSERT_EQUAL_UINT(before + 1, fx.app[a].commands);
    }
}

typedef struct {
    unsigned victim;
    unsigned target_start;
} start_fault_t;

static start_fault_t stf;

static int lost_start_filter(hubus_sim_t *sim, unsigned slave, uint8_t *clk, uint8_t *data, void *ctx)
{
    start_fault_t *f = ctx;

    (void)clk;
    if (slave == f->victim && sim->clk && sim->n_start == f->target_start) {
        *data = 1; /* the falling DATA edge of the START is not seen */
    }
    return 1;
}

static void test_slave_that_misses_a_start_ignores_the_frame_and_is_collected_by_the_next_burst(void)
{
    hubus_burst_stats_t st;
    unsigned a = fx_add(&fx, 10);

    fx_startup(&fx);
    fx_write(&fx, a, 0x42);
    /* burst: one group REQUEST_DATA (START #1); the slave misses the START and ignores the frame */
    stf.victim = a;
    stf.target_start = fx.sim.n_start + 1;
    fx.sim.filter = lost_start_filter;
    fx.sim.filter_ctx = &stf;
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    fx.sim.filter = NULL;
    TEST_ASSERT_EQUAL_UINT(0, st.served); /* nobody contended: the burst ends */
    TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[a])); /* the data stays buffered */

    /* the slave announces itself again and the next burst collects the data */
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, st.served);
    TEST_ASSERT_EQUAL_UINT8(0x42, fx.log[0].first);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&fx.core[a]));
}

/* ---- response errors slave -> master ----------------------------------------- */

static void test_transient_crc_error_inside_a_burst_is_retried(void)
{
    hubus_burst_stats_t st;
    unsigned a = fx_add(&fx, 10);

    fx_startup(&fx);
    fx_write(&fx, a, 0x42);
    rf.core = &fx.core[a];
    rf.tx_bit = 14; /* one bit in the middle of the response */
    rf.max_hits = 1; /* first attempt only */
    rf.active = 1;
    fx.sim.read_filter = tx_bit_filter;
    fx.sim.read_filter_ctx = &rf;
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, rf.hits);
    TEST_ASSERT_EQUAL_UINT(1, st.retries);
    TEST_ASSERT_EQUAL_UINT(1, st.served);
    TEST_ASSERT_EQUAL_UINT(0, st.skipped);
    TEST_ASSERT_EQUAL_UINT(1, fx.n_log); /* delivered exactly once */
    TEST_ASSERT_EQUAL_UINT8(0x42, fx.log[0].first);
    TEST_ASSERT_EQUAL(HUBUS_GEN_BASELINE, fx.log[0].result);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&fx.core[a]));
    TEST_ASSERT_EQUAL_UINT(0, fx.master.fail[10]); /* success clears the failure counter */
}

static void test_persistent_corruption_exhausts_the_retry_budget_without_losing_data(void)
{
    hubus_burst_stats_t st;
    unsigned a = fx_add(&fx, 10);
    unsigned resets;

    fx_startup(&fx);
    fx_write(&fx, a, 0x42);
    rf.core = &fx.core[a];
    rf.tx_bit = 14;
    rf.active = 1;
    fx.sim.read_filter = tx_bit_filter;
    fx.sim.read_filter_ctx = &rf;
    resets = fx.sim.n_reset;
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(fx.cfg.retry_budget, st.retries);
    TEST_ASSERT_EQUAL_UINT(1, st.skipped);
    TEST_ASSERT_EQUAL_UINT(0, st.served);
    /* every failed attempt ended with RESET, not STOP */
    TEST_ASSERT_EQUAL_UINT(resets + 1u + fx.cfg.retry_budget, fx.sim.n_reset);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[a])); /* RESET kept buffer and pending flag */

    /* the fault disappears: the data is still there and is delivered */
    rf.active = 0;
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, fx.n_log);
    TEST_ASSERT_EQUAL_UINT8(0x42, fx.log[0].first);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&fx.core[a]));
}

static void test_transient_crc_error_is_retried(void)
{
    hubus_burst_stats_t st;
    unsigned a = fx_add(&fx, 10);
    hubus_slave_frame_t f;

    fx_startup(&fx);
    fx_write(&fx, a, 0x42);
    rf.core = &fx.core[a];
    rf.tx_bit = 14;
    rf.active = 1;
    rf.hits = 0;
    fx.sim.read_filter = tx_bit_filter;
    fx.sim.read_filter_ctx = &rf;
    TEST_ASSERT_EQUAL(HUBUS_ERR_CRC, hubus_master_request_data(&fx.master, 10, &f)); /* ended with RESET */
    TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[a]));
    rf.active = 0;
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_data(&fx.master, 10, &f)); /* direct retry */
    TEST_ASSERT_EQUAL_UINT8(0x42, f.payload[0]);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&fx.core[a]));
    (void)st;
    assert_bus_idle();
}

static void test_corrupted_data_stops_being_retried_and_flags_the_mcid(void)
{
    hubus_burst_stats_t st;
    uint64_t suspects;
    unsigned a = fx_add(&fx, 10);

    fx_startup(&fx);
    fx_write(&fx, a, 0x42);
    rf.core = &fx.core[a];
    rf.tx_bit = 14;
    rf.active = 1;
    fx.sim.read_filter = tx_bit_filter;
    fx.sim.read_filter_ctx = &rf;
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1 + fx.cfg.retry_budget, fx.master.fail[10]);
    TEST_ASSERT_EQUAL_UINT(1 + fx.cfg.retry_budget, fx.master.mcid_counters[10].crc_errors);
    TEST_ASSERT_EQUAL_UINT(1 + fx.cfg.retry_budget, fx.master.counters.crc_errors);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_DIAG_DUPLICATE, hubus_master_diagnose(&fx.master, &suspects));
    TEST_ASSERT_TRUE(suspects & ((uint64_t)1 << 10));
}

/* ---- arbitration noise ---------------------------------------------------------- */

static void test_false_handoff_wastes_one_poll(void)
{
    hubus_burst_stats_t st;
    unsigned a = fx_add(&fx, 42); /* 101010 */

    fx_startup(&fx);
    fx_write(&fx, a, 1);
    rf.core = &fx.core[a];
    rf.slot = 5; /* handoff slot after bit 3, where the winner has a 1 */
    rf.want_level = 1;
    rf.active = 1;
    rf.hits = 0;
    fx.sim.read_filter = slot_filter;
    fx.sim.read_filter_ctx = &rf;
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, rf.hits);
    TEST_ASSERT_EQUAL_UINT(1, st.served);
    TEST_ASSERT_EQUAL_UINT(2, st.polls); /* the mask without contender cost one extra poll */
    TEST_ASSERT_EQUAL_UINT(0, st.skipped);
}

static void test_noise_in_arbitration_bits_never_breaks_the_burst(void)
{
    unsigned slot;

    for (slot = 0; slot < HUBUS_ARB_SLOTS; slot++) {
        hubus_burst_stats_t st;
        unsigned a, b;

        fx_init(&fx);
        a = fx_add(&fx, 42);
        b = fx_add(&fx, 40);
        fx_startup(&fx);
        fx_write(&fx, a, 1);
        fx_write(&fx, b, 2);
        rf.core = &fx.core[a];
        rf.slot = slot;
        rf.want_level = 0;
        rf.active = 1;
        rf.hits = 0;
        fx.sim.read_filter = slot_filter;
        fx.sim.read_filter_ctx = &rf;
        /* whatever one noisy slot does, the burst finishes and nothing is corrupted */
        TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
        rf.active = 0;
        fx.sim.read_filter = NULL;
        TEST_ASSERT_TRUE(fx.n_log <= 2);
        {
            uint8_t event;
            unsigned guard = 0;
            /* anything left over is collected by the regular return path */
            while ((hubus_core_is_pending(&fx.core[a]) || hubus_core_is_pending(&fx.core[b])) && guard++ < 5) {
                TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_check_event(&fx.master, &event));
                TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_burst(&fx.master, NULL, NULL, NULL));
            }
        }
        TEST_ASSERT_FALSE(hubus_core_is_pending(&fx.core[a]));
        TEST_ASSERT_FALSE(hubus_core_is_pending(&fx.core[b]));
        fx_check(&fx);
    }
}

static void test_missed_handoff_slot_slave_is_served_in_the_next_burst(void)
{
    hubus_burst_stats_t st;
    unsigned a = fx_add(&fx, 42); /* 101010 */
    unsigned b = fx_add(&fx, 40); /* 101000: loses at bit 1, handoff slot index 9 */

    fx_startup(&fx);
    fx_write(&fx, a, 1);
    fx_write(&fx, b, 2);
    rf.core = &fx.core[a];
    rf.slot = 9;
    rf.want_level = 0; /* the handoff slot is really low: read it as high */
    rf.active = 1;
    rf.hits = 0;
    fx.sim.read_filter = slot_filter;
    fx.sim.read_filter_ctx = &rf;
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, rf.hits);
    TEST_ASSERT_EQUAL_UINT(1, st.served); /* only 42; the handoff of 40 was missed */
    TEST_ASSERT_EQUAL_UINT8(42, fx.log[0].mcid);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[b]));
    rf.active = 0;

    /* 40 is still pending and announces itself again */
    fx.n_log = 0;
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_service(&fx.master, fx_collect, &fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, fx.n_log);
    TEST_ASSERT_EQUAL_UINT8(40, fx.log[0].mcid);
}

/* ---- lost STOP ------------------------------------------------------------------ */

typedef struct {
    unsigned victim;
    unsigned target_stop;
} stop_fault_t;

static stop_fault_t sf;

static void reset_faults(void)
{
    memset(&hook, 0, sizeof hook);
    memset(&vf, 0, sizeof vf);
    memset(&rf, 0, sizeof rf);
    memset(&sf, 0, sizeof sf);
    memset(&stf, 0, sizeof stf);
    hooked = 0;
}

static int lost_stop_filter(hubus_sim_t *sim, unsigned slave, uint8_t *clk, uint8_t *data, void *ctx)
{
    stop_fault_t *f = ctx;

    (void)clk;
    if (slave == f->victim && sim->clk && sim->n_stop == f->target_stop) {
        *data = 0; /* the rising DATA edge is not seen */
    }
    return 1;
}

static void test_lost_stop_leads_to_a_duplicate_that_is_discarded(void)
{
    hubus_burst_stats_t st;
    unsigned a = fx_add(&fx, 10);

    fx_startup(&fx);
    fx_write(&fx, a, 0x42);
    /* burst: one group REQUEST_DATA (STOP #1) - the slave misses it */
    sf.victim = a;
    sf.target_stop = fx.sim.n_stop + 1;
    fx.sim.filter = lost_stop_filter;
    fx.sim.filter_ctx = &sf;
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    fx.sim.filter = NULL;
    TEST_ASSERT_EQUAL_UINT(1, st.served);
    TEST_ASSERT_EQUAL_UINT8(0x42, fx.log[0].first);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[a])); /* it did not clear its buffer */

    /* the next burst delivers the same content again; GEN and payload identify it as a duplicate */
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(0, fx.n_log);
    TEST_ASSERT_EQUAL_UINT(1, st.duplicates);
    TEST_ASSERT_EQUAL_UINT(0, st.served);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&fx.core[a])); /* this time the STOP arrived */
    TEST_ASSERT_EQUAL_UINT(1, fx.master.mcid_counters[10].duplicates);
}

/* ---- stuck line / master restart ------------------------------------------------ */

static void test_stuck_line_during_operation_is_reported(void)
{
    hubus_burst_stats_t st;

    fx_add(&fx, 10);
    fx_startup(&fx);
    fx.sim.extra_low = 1;
    hubus_sim_settle(&fx.sim);
    TEST_ASSERT_EQUAL(HUBUS_ERR_LINE_FAULT, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL(HUBUS_ERR_LINE_FAULT, hubus_master_send_command(&fx.master, 0, 0, 0x10, NULL, 0));
    {
        uint8_t event;
        TEST_ASSERT_EQUAL(HUBUS_ERR_LINE_FAULT, hubus_master_check_event(&fx.master, &event));
    }
    fx.sim.extra_low = 0;
    hubus_sim_settle(&fx.sim);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_startup(&fx.master)); /* recovers when the fault is gone */
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    assert_bus_idle();
}

static void test_master_stops_mid_transaction_and_a_new_master_recovers(void)
{
    raw_t rm;
    hubus_burst_stats_t st;
    hubus_master_hal_t hal;
    unsigned a = fx_add(&fx, 10);
    unsigned i;

    fx_startup(&fx);
    fx_write(&fx, a, 0x42);

    /* the old master dies in the middle of the response, with the slave holding DATA low */
    raw_init(&rm, &fx.sim);
    raw_tick(&rm);
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_REQUEST_DATA, HUBUS_TARGET_LEN_EXACT, 10, NULL, 0);
    raw_turnaround(&rm);
    for (i = 0; i < 8; i++) {
        (void)raw_recv_bit(&rm);
    }
    TEST_ASSERT_EQUAL_UINT8(1, fx.core[a].state == HUBUS_ST_TX);

    /* a fresh master process takes over: startup sequence, then a full burst */
    fx.sim.timing_violations = 0; /* the raw traffic above does not keep the master minimums */
    hal = hubus_sim_master_hal(&fx.sim);
    hubus_master_init(&fx.master, &hal, &fx.cfg);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_startup(&fx.master));
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, fx.n_log);
    TEST_ASSERT_EQUAL_UINT8(0x42, fx.log[0].first); /* buffered data survived */
}

/* ---- event signal racing a START (accepted rare case 7.1) ------------------------ */

static void write_slave0(void *ctx)
{
    (void)ctx;
    fx_write(&fx, 0, 0x99);
}

static void test_event_racing_a_start_is_detected_and_the_command_repeated(void)
{
    const uint8_t args[] = {7};
    unsigned racer = fx_add(&fx, 9);
    unsigned target = fx_add(&fx, 10);

    fx_startup(&fx);
    /* slave 9 decides to signal an event just before the master raises CLK for the START */
    hook_install(1, write_slave0, NULL);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 10, 0x30, args, 1));
    TEST_ASSERT_TRUE(fx.sim.n_reset >= 2); /* the race was detected and recovered with a RESET */
    TEST_ASSERT_EQUAL_UINT(1, fx.app[target].commands); /* executed exactly once */
    TEST_ASSERT_EQUAL_UINT8(7, fx.app[target].last_args[0]);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[racer])); /* the event is not lost */
    hook.fire_at = 0;
    {
        hubus_burst_stats_t st;
        TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
        TEST_ASSERT_EQUAL_UINT(1, fx.n_log);
        TEST_ASSERT_EQUAL_UINT8(0x99, fx.log[0].first);
    }
}

static void test_event_racing_a_request_is_repeated(void)
{
    hubus_slave_frame_t f;
    unsigned racer = fx_add(&fx, 9);
    unsigned target = fx_add(&fx, 10);

    fx_startup(&fx);
    fx_write(&fx, target, 0x31);
    {
        uint8_t event = 0;
        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_check_event(&fx.master, &event)); /* handshake: the event is released */
        TEST_ASSERT_EQUAL_UINT8(1, event);
    }
    hook_install(1, write_slave0, NULL);
    {
        hubus_status_t r = hubus_master_request_data(&fx.master, 10, &f);
        /* either the race was detected and repeated internally or reported as a line error */
        if (r != HUBUS_OK) {
            TEST_ASSERT_EQUAL(HUBUS_ERR_LINE, r);
            hook.fire_at = 0;
            TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_data(&fx.master, 10, &f));
        }
    }
    TEST_ASSERT_EQUAL_UINT8(0x31, f.payload[0]);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[racer]));
}

/* ---- readback while transmitting -------------------------------------------------- */

static void foreign_node_pulls_low(void *ctx)
{
    (void)ctx;
    fx.sim.extra_low = 1;
    hubus_sim_settle(&fx.sim);
    hook.release_at_fall = 1;
}

static void test_foreign_driver_during_frame_is_detected_by_readback(void)
{
    const uint8_t args[] = {1};
    unsigned a = fx_add(&fx, 10);

    fx_startup(&fx);
    /* rise 1 = START, rises 2.. = frame bits; bit index 4 of a SEND_COMMAND header is a released 0 */
    hook_install(6, foreign_node_pulls_low, NULL);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 10, 0x30, args, 1));
    TEST_ASSERT_TRUE(fx.sim.n_reset >= 2); /* readback caught the foreign driver, recovery ran */
    TEST_ASSERT_EQUAL_UINT(1, fx.app[a].commands); /* the corrupted attempt was not executed, the repeat was */
    assert_bus_idle();
}

/* ---- duplicate MCID ---------------------------------------------------------------- */

static void test_duplicate_mcid_with_one_pending_slave_goes_unnoticed(void)
{
    hubus_burst_stats_t st;
    unsigned a = fx_add_ex(&fx, 9, 1, 1);

    fx_add_ex(&fx, 9, 1, 2);
    fx_startup(&fx);
    fx_write(&fx, a, 0x42);
    /* accepted limitation: only one of the two has data, the response is clean */
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, st.served);
    TEST_ASSERT_EQUAL_UINT(0, fx.master.fail[9]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bit_flip_in_command_is_rejected_and_repeat_works);
    RUN_TEST(test_bit_flip_in_poll_leaves_slave_out_of_burst_but_not_forgotten);
    RUN_TEST(test_lost_clock_pulse_in_command_frame);
    RUN_TEST(test_extra_clock_pulse_in_command_frame);
    RUN_TEST(test_slave_that_misses_a_start_ignores_the_frame_and_is_collected_by_the_next_burst);
    RUN_TEST(test_transient_crc_error_inside_a_burst_is_retried);
    RUN_TEST(test_persistent_corruption_exhausts_the_retry_budget_without_losing_data);
    RUN_TEST(test_transient_crc_error_is_retried);
    RUN_TEST(test_corrupted_data_stops_being_retried_and_flags_the_mcid);
    RUN_TEST(test_false_handoff_wastes_one_poll);
    RUN_TEST(test_noise_in_arbitration_bits_never_breaks_the_burst);
    RUN_TEST(test_missed_handoff_slot_slave_is_served_in_the_next_burst);
    RUN_TEST(test_lost_stop_leads_to_a_duplicate_that_is_discarded);
    RUN_TEST(test_stuck_line_during_operation_is_reported);
    RUN_TEST(test_master_stops_mid_transaction_and_a_new_master_recovers);
    RUN_TEST(test_event_racing_a_start_is_detected_and_the_command_repeated);
    RUN_TEST(test_event_racing_a_request_is_repeated);
    RUN_TEST(test_foreign_driver_during_frame_is_detected_by_readback);
    RUN_TEST(test_duplicate_mcid_with_one_pending_slave_goes_unnoticed);
    return UNITY_END();
}
