/*
 * Bus simulation scenarios: the real master and real slave cores talk over
 * the simulated wired-AND bus (startup, idle handshake, bursts, status
 * capture, commands, commissioning, fragments).
 */
#include <stdlib.h>
#include <string.h>

#include "fixture.h"
#include "rawmaster.h"

static fx_t fx;

void setUp(void)
{
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

/* ---- startup -------------------------------------------------------------- */

static void test_startup_on_idle_bus_generates_reset(void)
{
    fx_add(&fx, 3);
    fx_add(&fx, 9);
    fx_startup(&fx);
    TEST_ASSERT_EQUAL_UINT(1, fx.sim.n_reset);
    TEST_ASSERT_EQUAL_UINT(1, fx.sim.n_start);
    TEST_ASSERT_EQUAL_UINT(1, fx.sim.n_stop);
    assert_bus_idle();
}

static void test_startup_clears_slave_signaling_an_event(void)
{
    unsigned s = fx_add(&fx, 3);

    fx_write(&fx, s, 1);
    TEST_ASSERT_EQUAL_UINT8(0, fx.sim.data);
    fx_startup(&fx);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[s])); /* pending data survives */
}

static void test_startup_unblocks_slave_stuck_in_arbitration(void)
{
    raw_t rm;
    unsigned s = fx_add(&fx, 63);
    int i;

    fx_write(&fx, s, 1);
    raw_init(&rm, &fx.sim);
    raw_tick(&rm);
    /* a previous master died in the middle of an arbitration */
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_POLL_PENDING, 0, 0, NULL, 0);
    raw_turnaround(&rm);
    for (i = 0; i < 2; i++) {
        raw_tick(&rm);
    }
    TEST_ASSERT_EQUAL_UINT8(0, fx.sim.data);

    fx.sim.timing_violations = 0; /* the raw setup above does not keep the master minimums */
    fx_startup(&fx);
    TEST_ASSERT_EQUAL_UINT(HUBUS_ST_EVENT, fx.core[s].state); /* idle again, announcing its data */
}

static void test_startup_unblocks_slave_stuck_in_transmission(void)
{
    raw_t rm;
    uint8_t v = 0xFF;
    unsigned s = fx_add(&fx, 9);
    unsigned i;

    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&fx.sim, s, &v, 1, 0));
    raw_init(&rm, &fx.sim);
    raw_tick(&rm);
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_REQUEST_DATA, HUBUS_TARGET_LEN_EXACT, 9, NULL, 0);
    raw_turnaround(&rm);
    for (i = 0; i < 5; i++) { /* header bits 1 0 0 0 0 ... then the payload 0xFF holds DATA low */
        raw_tick(&rm);
    }
    fx.sim.timing_violations = 0; /* the raw setup above does not keep the master minimums */
    fx_startup(&fx);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[s]));
}

static void test_startup_reports_stuck_line(void)
{
    fx_add(&fx, 3);
    fx.sim.extra_low = 1; /* some node holds DATA low permanently */
    hubus_sim_settle(&fx.sim);
    TEST_ASSERT_EQUAL(HUBUS_ERR_LINE_FAULT, hubus_master_startup(&fx.master));
    TEST_ASSERT_TRUE(fx.sim.clk_rises >= fx.cfg.line_fault_ticks); /* it clocked for at least the threshold */
    fx.sim.extra_low = 0;
    hubus_sim_settle(&fx.sim);
    fx_startup(&fx);
}

static void test_shutdown_leaves_slaves_idle(void)
{
    unsigned s = fx_add(&fx, 3);

    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_shutdown(&fx.master));
    TEST_ASSERT_EQUAL_UINT(2, fx.sim.n_reset);
    TEST_ASSERT_EQUAL_UINT(HUBUS_ST_IDLE, fx.core[s].state);
    assert_bus_idle();
}

/* ---- bursts --------------------------------------------------------------- */

static void test_burst_on_idle_bus(void)
{
    hubus_burst_stats_t st;

    fx_add(&fx, 3);
    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, st.polls);
    TEST_ASSERT_EQUAL_UINT(0, st.served);
    assert_bus_idle();
}

static void test_burst_spec_example(void)
{
    const uint8_t mcids[] = {42, 40, 25, 15};
    const uint8_t order[] = {42, 40, 25, 15};
    hubus_burst_stats_t st;
    unsigned i;

    for (i = 0; i < 4; i++) {
        unsigned s = fx_add(&fx, mcids[i]);
        fx_write(&fx, s, (uint8_t)(0x80 + mcids[i]));
    }
    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));

    /* docs/ARBITRATION.md section 7: four polls, served in descending MCID order */
    TEST_ASSERT_EQUAL_UINT(4, st.polls);
    TEST_ASSERT_EQUAL_UINT(4, st.served);
    TEST_ASSERT_EQUAL_UINT(4, fx.n_log);
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(order[i], fx.log[i].mcid);
        TEST_ASSERT_EQUAL_UINT8(0x80 + order[i], fx.log[i].first);
        TEST_ASSERT_EQUAL(HUBUS_GEN_BASELINE, fx.log[i].result);
        TEST_ASSERT_FALSE(hubus_core_is_pending(&fx.core[i]));
    }
    assert_bus_idle();
}

static void test_burst_single_slave_lowest_and_highest_mcid(void)
{
    hubus_burst_stats_t st;
    unsigned lo = fx_add(&fx, 1);
    unsigned hi = fx_add(&fx, 63);

    fx_startup(&fx);
    fx_write(&fx, lo, 0x11);
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, fx.n_log);
    TEST_ASSERT_EQUAL_UINT8(1, fx.log[0].mcid);

    fx_write(&fx, hi, 0x22);
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, fx.n_log);
    TEST_ASSERT_EQUAL_UINT8(63, fx.log[0].mcid);
}

static void test_burst_all_63_slaves(void)
{
    hubus_burst_stats_t st;
    unsigned mcid;
    unsigned i;

    for (mcid = 1; mcid <= 63; mcid++) {
        unsigned s = fx_add(&fx, (uint8_t)mcid);
        fx_write(&fx, s, (uint8_t)mcid);
    }
    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(63, fx.n_log);
    for (i = 0; i < 63; i++) {
        TEST_ASSERT_EQUAL_UINT8(63 - i, fx.log[i].mcid);
        TEST_ASSERT_EQUAL_UINT8(63 - i, fx.log[i].first);
    }
    /* exact masks are requested without a poll, so fewer than one poll per slave */
    TEST_ASSERT_TRUE(st.polls < 63);
    TEST_ASSERT_EQUAL_UINT(0, st.retries);
    TEST_ASSERT_EQUAL_UINT(0, st.skipped);
    assert_bus_idle();
}

static void test_burst_random_subsets(void)
{
    unsigned seed = 99;
    int round;

    for (round = 0; round < 25; round++) {
        uint8_t used[64] = {0};
        uint8_t order[16];
        unsigned n = 0;
        unsigned k, i;
        hubus_burst_stats_t st;

        fx_init(&fx);
        while (n < 12) {
            uint8_t mcid;
            seed = seed * 1103515245u + 12345u;
            mcid = (uint8_t)(1 + (seed >> 16) % 63);
            if (!used[mcid]) {
                used[mcid] = 1;
                fx_add(&fx, mcid);
                n++;
            }
        }
        seed = seed * 1103515245u + 12345u;
        k = 1 + (seed >> 16) % 12; /* number of slaves with data */
        for (i = 0; i < n; i++) {
            if (i < k) {
                fx_write(&fx, i, fx.core[i].mcid);
            }
        }
        /* expected order: pending MCIDs, descending */
        for (i = 0, n = 0; i < 64; i++) {
            unsigned j;
            for (j = 0; j < k; j++) {
                if (fx.core[j].mcid == 63 - i) {
                    order[n++] = (uint8_t)(63 - i);
                }
            }
        }
        fx_startup(&fx);
        TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
        TEST_ASSERT_EQUAL_UINT(k, fx.n_log);
        for (i = 0; i < k; i++) {
            TEST_ASSERT_EQUAL_UINT8(order[i], fx.log[i].mcid);
        }
        fx_check(&fx);
    }
}

static void reappear(void *ctx, const hubus_slave_frame_t *f)
{
    fx_t *f_x = ctx;

    if (f->mcid == 50) {
        fx_write(f_x, 0, 9); /* slave index 0 has MCID 50 */
    }
}

static void test_busy_high_priority_slave_does_not_starve_lower_ones(void)
{
    hubus_burst_stats_t st;
    unsigned hi = fx_add(&fx, 50);
    unsigned lo = fx_add(&fx, 20);

    fx_write(&fx, hi, 1);
    fx_write(&fx, lo, 2);
    fx_startup(&fx);
    fx.on_record = NULL;
    fx.on_record_ctx = &fx;
    fx.on_record = reappear;
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    /* 50 wrote again while the burst ran: 20 is still served in this burst, 50 only in the next one */
    TEST_ASSERT_EQUAL_UINT(2, fx.n_log);
    TEST_ASSERT_EQUAL_UINT8(50, fx.log[0].mcid);
    TEST_ASSERT_EQUAL_UINT8(20, fx.log[1].mcid);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[hi]));
    fx.on_record = NULL;
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, fx.n_log);
    TEST_ASSERT_EQUAL_UINT8(50, fx.log[0].mcid);
    TEST_ASSERT_EQUAL_UINT8(9, fx.log[0].first);
}

/* ---- idle event handshake -------------------------------------------------- */

static void test_check_event_without_events(void)
{
    uint8_t event = 1;

    fx_add(&fx, 3);
    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_check_event(&fx.master, &event));
    TEST_ASSERT_EQUAL_UINT8(0, event);
    assert_bus_idle();
}

static void test_event_handshake_then_burst(void)
{
    hubus_burst_stats_t st;
    uint8_t event = 0;
    unsigned s = fx_add(&fx, 3);

    fx_startup(&fx);
    fx_write(&fx, s, 0x55); /* e.g. a button press while the bus is idle */
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_check_event(&fx.master, &event));
    TEST_ASSERT_EQUAL_UINT8(1, event);
    TEST_ASSERT_EQUAL_UINT8(1, fx.sim.data); /* released after the confirmation pulse */
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, fx.n_log);
    TEST_ASSERT_EQUAL_UINT8(0x55, fx.log[0].first);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_check_event(&fx.master, &event));
    TEST_ASSERT_EQUAL_UINT8(0, event);
}

static void test_service_runs_burst_only_on_events(void)
{
    hubus_burst_stats_t st;
    unsigned a = fx_add(&fx, 3);
    unsigned b = fx_add(&fx, 4);

    fx_startup(&fx);
    fx.n_log = 0;
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_service(&fx.master, fx_collect, &fx, &st));
    TEST_ASSERT_EQUAL_UINT(0, st.polls);
    fx_write(&fx, a, 1);
    fx_write(&fx, b, 2);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_service(&fx.master, fx_collect, &fx, &st));
    TEST_ASSERT_EQUAL_UINT(2, st.served);
    TEST_ASSERT_EQUAL_UINT8(4, fx.log[0].mcid);
    TEST_ASSERT_EQUAL_UINT8(3, fx.log[1].mcid);
}

/* ---- commands -------------------------------------------------------------- */

static void test_status_capture_broadcast_then_burst(void)
{
    const uint8_t mcids[] = {42, 40, 25, 15};
    hubus_burst_stats_t st;
    unsigned i;

    for (i = 0; i < 4; i++) {
        fx_add(&fx, mcids[i]);
    }
    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_status_capture(&fx.master, 0, 0)); /* wire timeline example */
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT(1, fx.app[i].captures);
        TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[i]));
    }
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(4, fx.n_log);
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(mcids[i], fx.log[i].mcid);
        TEST_ASSERT_EQUAL_UINT8(0x40 + mcids[i], fx.log[i].first);
    }
}

static void test_status_capture_of_a_group(void)
{
    hubus_burst_stats_t st;
    unsigned a = fx_add(&fx, 40);
    unsigned b = fx_add(&fx, 41);
    unsigned c = fx_add(&fx, 42);

    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_status_capture(&fx.master, 5, 40)); /* 10100x */
    TEST_ASSERT_EQUAL_UINT(1, fx.app[a].captures);
    TEST_ASSERT_EQUAL_UINT(1, fx.app[b].captures);
    TEST_ASSERT_EQUAL_UINT(0, fx.app[c].captures);
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(2, fx.n_log);
}

static void test_status_capture_single_slave_and_read_back(void)
{
    hubus_slave_frame_t f;
    unsigned s = fx_add(&fx, 7);

    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_status_capture(&fx.master, 6, 7));
    TEST_ASSERT_EQUAL_UINT(1, fx.app[s].captures);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_data(&fx.master, 7, &f));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_S_DATA, f.type);
    TEST_ASSERT_EQUAL_UINT8(1, f.len);
    TEST_ASSERT_EQUAL_UINT8(0x47, f.payload[0]);
    assert_bus_idle();
}

static void test_send_command_exact_and_group(void)
{
    const uint8_t args[] = {1, 2, 3};
    unsigned a = fx_add(&fx, 40);
    unsigned b = fx_add(&fx, 41);
    unsigned c = fx_add(&fx, 50);

    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 41, 0x20, args, 3));
    TEST_ASSERT_EQUAL_UINT(0, fx.app[a].commands);
    TEST_ASSERT_EQUAL_UINT(1, fx.app[b].commands);
    TEST_ASSERT_EQUAL_UINT(0, fx.app[c].commands);
    TEST_ASSERT_EQUAL_UINT8(0x20, fx.app[b].last_op);
    TEST_ASSERT_EQUAL_UINT8(3, fx.app[b].last_len);
    TEST_ASSERT_EQUAL_MEMORY(args, fx.app[b].last_args, 3);

    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 5, 40, 0x21, NULL, 0));
    TEST_ASSERT_EQUAL_UINT(1, fx.app[a].commands);
    TEST_ASSERT_EQUAL_UINT(2, fx.app[b].commands);
    TEST_ASSERT_EQUAL_UINT(0, fx.app[c].commands);

    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 0, 0, 0x22, NULL, 0));
    TEST_ASSERT_EQUAL_UINT(2, fx.app[a].commands);
    TEST_ASSERT_EQUAL_UINT(3, fx.app[b].commands);
    TEST_ASSERT_EQUAL_UINT(1, fx.app[c].commands);
    assert_bus_idle();
}

static void test_send_maximum_command(void)
{
    uint8_t args[HUBUS_MAX_PAYLOAD - 1];
    unsigned s = fx_add(&fx, 12);
    unsigned i;

    for (i = 0; i < sizeof args; i++) {
        args[i] = (uint8_t)(i + 100);
    }
    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 12, 0xFF, args, sizeof args));
    TEST_ASSERT_EQUAL_UINT8(30, fx.app[s].last_len);
    TEST_ASSERT_EQUAL_MEMORY(args, fx.app[s].last_args, sizeof args);
}

static void test_argument_validation(void)
{
    uint8_t args[31] = {0};
    hubus_slave_frame_t f;
    hubus_arb_result_t r;

    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_ERR_ARG, hubus_master_send_command(&fx.master, 7, 0, 0x10, NULL, 0));
    TEST_ASSERT_EQUAL(HUBUS_ERR_ARG, hubus_master_send_command(&fx.master, 6, 64, 0x10, NULL, 0));
    TEST_ASSERT_EQUAL(HUBUS_ERR_ARG, hubus_master_send_command(&fx.master, 6, 1, 0x10, args, 31)); /* opcode + 31 bytes */
    TEST_ASSERT_EQUAL(HUBUS_ERR_ARG, hubus_master_send_command(&fx.master, 6, 1, 0x10, NULL, 1));
    TEST_ASSERT_EQUAL(HUBUS_ERR_ARG, hubus_master_request_data(&fx.master, 0, &f)); /* MCID 0 is reserved */
    TEST_ASSERT_EQUAL(HUBUS_ERR_ARG, hubus_master_request_info(&fx.master, 64, &f));
    TEST_ASSERT_EQUAL(HUBUS_ERR_ARG, hubus_master_poll(&fx.master, 0, 7, &r));
    TEST_ASSERT_EQUAL_UINT(1, fx.sim.n_start); /* nothing was put on the bus (only the startup RESET) */
}

/* ---- requests -------------------------------------------------------------- */

static void test_request_info_roundtrip(void)
{
    hubus_slave_frame_t f;

    fx_add_ex(&fx, 9, 1, 0xC3);
    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_info(&fx.master, 9, &f));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_S_INFO, f.type);
    TEST_ASSERT_EQUAL_UINT8(9, f.mcid);
    TEST_ASSERT_EQUAL_UINT8(7, f.len);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_PROTOCOL_VERSION, f.payload[HUBUS_INFO_VERSION]);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_CAP_STATUS_CAPTURE, f.payload[HUBUS_INFO_CAPS]);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_RX_BUF_SIZE, f.payload[HUBUS_INFO_MAX_CMD]);
    TEST_ASSERT_EQUAL_UINT8(0xC3, f.payload[HUBUS_INFO_APP + 3]);
    assert_bus_idle();
}

static void test_request_to_absent_slave_is_no_response(void)
{
    hubus_slave_frame_t f;

    fx_add(&fx, 9);
    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_ERR_NO_RESPONSE, hubus_master_request_info(&fx.master, 10, &f));
    TEST_ASSERT_EQUAL(HUBUS_ERR_NO_RESPONSE, hubus_master_request_data(&fx.master, 9, &f)); /* empty buffer */
    assert_bus_idle();
}

static void test_request_data_reads_buffer_and_clears_it(void)
{
    hubus_slave_frame_t f;
    const uint8_t payload[] = {9, 8, 7};
    unsigned s = fx_add(&fx, 9);

    fx_startup(&fx);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&fx.sim, s, payload, 3, 1));
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_data(&fx.master, 9, &f));
    TEST_ASSERT_EQUAL_UINT8(3, f.len);
    TEST_ASSERT_EQUAL_UINT8(1, f.more);
    TEST_ASSERT_EQUAL_MEMORY(payload, f.payload, 3);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&fx.core[s]));
}

static void test_poll_reports_arbitration(void)
{
    hubus_arb_result_t r;
    unsigned a = fx_add(&fx, 42);
    unsigned b = fx_add(&fx, 40);

    fx_startup(&fx);
    fx_write(&fx, a, 1);
    fx_write(&fx, b, 1);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_poll(&fx.master, 0, 0, &r));
    TEST_ASSERT_TRUE(r.contender);
    TEST_ASSERT_EQUAL_UINT8(42, r.winner);
    TEST_ASSERT_EQUAL_UINT8(1, r.n_masks);
    TEST_ASSERT_EQUAL_UINT8(5, r.masks[0].len);
    TEST_ASSERT_EQUAL_UINT8(40, r.masks[0].prefix);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[a])); /* a poll never clears anything */
}

/* ---- fragments ------------------------------------------------------------- */

static unsigned fragments_left;

static void next_fragment(void *ctx, const hubus_slave_frame_t *f)
{
    fx_t *f_x = ctx;

    if (f->mcid == 9 && fragments_left > 0) {
        uint8_t next = (uint8_t)(f->payload[0] + 1);
        fragments_left--;
        TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&f_x->sim, 0, &next, 1, fragments_left > 0));
    }
}

static void test_fragmented_transfer_in_one_burst(void)
{
    hubus_burst_stats_t st;
    uint8_t first = 1;
    unsigned s = fx_add(&fx, 9);

    fx_startup(&fx);
    fragments_left = 2; /* the application writes two more fragments after reading the first */
    fx.on_record = next_fragment;
    fx.on_record_ctx = &fx;
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&fx.sim, s, &first, 1, 1));
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(3, fx.n_log);
    TEST_ASSERT_EQUAL_UINT8(1, fx.log[0].first);
    TEST_ASSERT_EQUAL_UINT8(2, fx.log[1].first);
    TEST_ASSERT_EQUAL_UINT8(3, fx.log[2].first);
    TEST_ASSERT_EQUAL_UINT8(1, fx.log[0].more);
    TEST_ASSERT_EQUAL_UINT8(1, fx.log[1].more);
    TEST_ASSERT_EQUAL_UINT8(0, fx.log[2].more); /* last fragment */
    TEST_ASSERT_EQUAL(HUBUS_GEN_NORMAL, fx.log[1].result);
    TEST_ASSERT_EQUAL(HUBUS_GEN_NORMAL, fx.log[2].result);
    TEST_ASSERT_EQUAL_UINT(1, st.polls); /* the follow-up requests need no further poll */
    TEST_ASSERT_FALSE(hubus_core_is_pending(&fx.core[s]));
}

static void test_next_fragment_not_ready_is_not_an_error(void)
{
    hubus_burst_stats_t st;
    uint8_t first = 1;
    unsigned s = fx_add(&fx, 9);

    fx_startup(&fx);
    fx.on_record = NULL;
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&fx.sim, s, &first, 1, 1));
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, fx.n_log);
    TEST_ASSERT_EQUAL_UINT(0, st.skipped);
    TEST_ASSERT_EQUAL_UINT(0, st.retries);
    /* the application writes the next fragment later and announces it by itself */
    first = 2;
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&fx.sim, s, &first, 1, 0));
    TEST_ASSERT_EQUAL_UINT8(0, fx.sim.data);
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT8(2, fx.log[0].first);
}

/* ---- generation counter ---------------------------------------------------- */

static void test_overwritten_data_is_reported_as_gap(void)
{
    hubus_burst_stats_t st;
    unsigned s = fx_add(&fx, 9);

    fx_startup(&fx);
    fx_write(&fx, s, 1);
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL(HUBUS_GEN_BASELINE, fx.log[0].result);

    fx_write(&fx, s, 2);
    fx_write(&fx, s, 3); /* the content 2 is overwritten unread */
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, fx.n_log);
    TEST_ASSERT_EQUAL(HUBUS_GEN_GAP, fx.log[0].result);
    TEST_ASSERT_EQUAL_UINT8(1, fx.log[0].lost);
    TEST_ASSERT_EQUAL_UINT8(3, fx.log[0].first);
    TEST_ASSERT_EQUAL_UINT(1, st.gaps);
}

static void test_counter_wrap_with_same_payload_after_four_writes(void)
{
    hubus_burst_stats_t st;
    unsigned s = fx_add(&fx, 9);
    unsigned i;

    fx_startup(&fx);
    fx_write(&fx, s, 7);
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    for (i = 0; i < 4; i++) {
        fx_write(&fx, s, 7);
    }
    /* four writes: GEN is equal again and the payload is identical -> treated as a duplicate (harmless) */
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, st.duplicates);

    fx_write(&fx, s, 8);
    fx_write(&fx, s, 8);
    fx_write(&fx, s, 8);
    fx_write(&fx, s, 8);
    fx_write(&fx, s, 9);
    fx_write(&fx, s, 10);
    fx_write(&fx, s, 11);
    fx_write(&fx, s, 12);
    /* equal GEN but different payload: new content, not a duplicate */
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, fx.n_log);
    TEST_ASSERT_EQUAL(HUBUS_GEN_WRAPPED, fx.log[0].result);
    TEST_ASSERT_EQUAL_UINT8(12, fx.log[0].first);
}

/* ---- commissioning --------------------------------------------------------- */

static void test_scan_finds_all_present_slaves(void)
{
    static hubus_scan_t scan;
    unsigned mcid;

    fx_add_ex(&fx, 5, 1, 0x05);
    fx_add_ex(&fx, 17, 0, 0x11);
    fx_add_ex(&fx, 63, 1, 0x3F);
    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_scan(&fx.master, &scan));
    for (mcid = 1; mcid <= 63; mcid++) {
        unsigned expect = (mcid == 5 || mcid == 17 || mcid == 63) ? HUBUS_SCAN_PRESENT : HUBUS_SCAN_ABSENT;
        TEST_ASSERT_EQUAL_UINT(expect, scan.state[mcid]);
    }
    TEST_ASSERT_EQUAL_UINT8(HUBUS_CAP_STATUS_CAPTURE, scan.info[5].payload[HUBUS_INFO_CAPS]);
    TEST_ASSERT_EQUAL_UINT8(0, scan.info[17].payload[HUBUS_INFO_CAPS]);
    TEST_ASSERT_EQUAL_UINT8(0x3F, scan.info[63].payload[HUBUS_INFO_APP + 3]);
    assert_bus_idle();
}

static void test_scan_flags_duplicate_mcid_with_unique_serials(void)
{
    static hubus_scan_t scan;

    fx_add_ex(&fx, 9, 1, 0xA1);
    fx_add_ex(&fx, 9, 1, 0xB2); /* configuration fault: same MCID, different serial number */
    fx_add_ex(&fx, 10, 1, 0xC3);
    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_scan(&fx.master, &scan));
    TEST_ASSERT_EQUAL_UINT(HUBUS_SCAN_SUSPECT, scan.state[9]);
    TEST_ASSERT_EQUAL_UINT(HUBUS_SCAN_PRESENT, scan.state[10]);
}

static void test_scan_blind_spot_for_identical_duplicates(void)
{
    static hubus_scan_t scan;

    /* accepted limitation (PROTOCOL.md 3.5): identical responses overlap into a valid frame */
    fx_add_ex(&fx, 9, 1, 0x77);
    fx_add_ex(&fx, 9, 1, 0x77);
    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_scan(&fx.master, &scan));
    TEST_ASSERT_EQUAL_UINT(HUBUS_SCAN_PRESENT, scan.state[9]);
}

/* ---- discovery ------------------------------------------------------------- */

static void test_poll_discover_finds_idle_slaves_and_clears_nothing(void)
{
    hubus_arb_result_t r;
    unsigned a = fx_add(&fx, 42);
    unsigned b = fx_add(&fx, 40);

    fx_startup(&fx);
    fx_write(&fx, b, 1);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_poll_discover(&fx.master, 0, 0, &r));
    TEST_ASSERT_TRUE(r.contender);
    TEST_ASSERT_EQUAL_UINT8(42, r.winner); /* not pending, but contends */
    TEST_ASSERT_EQUAL_UINT8(1, r.n_masks);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&fx.core[a]));
    TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[b]));
}

static void test_scan_uses_far_fewer_transactions_than_full_scan(void)
{
    static hubus_scan_t scan;
    uint16_t discover_info;

    fx_add_ex(&fx, 5, 1, 0x05);
    fx_add_ex(&fx, 17, 0, 0x11);
    fx_add_ex(&fx, 63, 1, 0x3F);
    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_scan(&fx.master, &scan));
    discover_info = fx.master.counters.info_requests;
    TEST_ASSERT_EQUAL_UINT16(3, discover_info); /* one REQUEST_INFO per slave found */
    TEST_ASSERT_TRUE(fx.master.counters.polls <= 6u);
    TEST_ASSERT_EQUAL_UINT(HUBUS_SCAN_PRESENT, scan.state[5]);
    TEST_ASSERT_EQUAL_UINT(HUBUS_SCAN_PRESENT, scan.state[17]);
    TEST_ASSERT_EQUAL_UINT(HUBUS_SCAN_PRESENT, scan.state[63]);
    TEST_ASSERT_EQUAL_UINT(HUBUS_SCAN_ABSENT, scan.state[6]);
    assert_bus_idle();
}

static void test_scan_finds_every_possible_mcid(void)
{
    static hubus_scan_t scan;
    unsigned mcid;

    for (mcid = 1; mcid <= 63; mcid++) {
        fx_add_ex(&fx, mcid, 1, mcid);
    }
    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_scan(&fx.master, &scan));
    for (mcid = 1; mcid <= 63; mcid++) {
        TEST_ASSERT_EQUAL_UINT(HUBUS_SCAN_PRESENT, scan.state[mcid]);
        TEST_ASSERT_EQUAL_UINT8(mcid, scan.info[mcid].payload[HUBUS_INFO_APP + 3]);
    }
}

static void test_scan_does_not_disturb_pending_data(void)
{
    static hubus_scan_t scan;
    hubus_burst_stats_t st;
    unsigned a = fx_add_ex(&fx, 5, 1, 0x05);

    fx_add_ex(&fx, 17, 1, 0x11);
    fx_startup(&fx);
    fx_write(&fx, a, 1);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_scan(&fx.master, &scan));
    TEST_ASSERT_TRUE(hubus_core_is_pending(&fx.core[a]));
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_burst(&fx.master, NULL, NULL, &st));
    TEST_ASSERT_EQUAL_UINT16(1, st.served);
}

static void test_full_scan_still_finds_all_present_slaves(void)
{
    static hubus_scan_t scan;
    unsigned mcid;

    fx_add_ex(&fx, 5, 1, 0x05);
    fx_add_ex(&fx, 17, 0, 0x11);
    fx_add_ex(&fx, 63, 1, 0x3F);
    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_scan_full(&fx.master, &scan));
    for (mcid = 1; mcid <= 63; mcid++) {
        unsigned expect = (mcid == 5 || mcid == 17 || mcid == 63) ? HUBUS_SCAN_PRESENT : HUBUS_SCAN_ABSENT;
        TEST_ASSERT_EQUAL_UINT(expect, scan.state[mcid]);
    }
    TEST_ASSERT_EQUAL_UINT16(63, fx.master.counters.info_requests);
}

static void test_scan_without_slaves_finds_nothing(void)
{
    static hubus_scan_t scan;
    unsigned mcid;

    fx_startup(&fx);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_scan(&fx.master, &scan));
    for (mcid = 1; mcid <= 63; mcid++) {
        TEST_ASSERT_EQUAL_UINT(HUBUS_SCAN_ABSENT, scan.state[mcid]);
    }
    TEST_ASSERT_EQUAL_UINT16(1, fx.master.counters.polls);
    TEST_ASSERT_EQUAL_UINT16(0, fx.master.counters.info_requests);
}


static void test_runtime_heuristic_flags_duplicate_mcid(void)
{
    hubus_burst_stats_t st;
    uint64_t suspects;
    unsigned a = fx_add_ex(&fx, 9, 1, 1);
    unsigned b = fx_add_ex(&fx, 9, 1, 2);
    unsigned other = fx_add(&fx, 12);

    fx_startup(&fx);
    fx_write(&fx, a, 0x0F);
    fx_write(&fx, b, 0xF0); /* both have different data: the overlapping frames fail the CRC */
    fx_write(&fx, other, 0x33);
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_TRUE(st.skipped >= 1);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_DIAG_DUPLICATE, hubus_master_diagnose(&fx.master, &suspects));
    TEST_ASSERT_TRUE(suspects & ((uint64_t)1 << 9));
    TEST_ASSERT_FALSE(suspects & ((uint64_t)1 << 12));
    /* the clean slave was served */
    TEST_ASSERT_EQUAL_UINT8(12, fx.log[0].mcid);
}

static void test_diagnose_many_failing_mcids_is_a_line_problem(void)
{
    uint64_t suspects;
    unsigned mcid;

    TEST_ASSERT_EQUAL_UINT8(HUBUS_DIAG_OK, hubus_master_diagnose(&fx.master, &suspects));
    TEST_ASSERT_EQUAL_UINT64(0, suspects);
    for (mcid = 1; mcid <= 5; mcid++) {
        fx.master.fail[mcid] = fx.cfg.dup_fail_threshold;
    }
    TEST_ASSERT_EQUAL_UINT8(HUBUS_DIAG_LINE_PROBLEM, hubus_master_diagnose(&fx.master, &suspects));
    fx.master.fail[1] = fx.master.fail[2] = fx.master.fail[3] = 0;
    TEST_ASSERT_EQUAL_UINT8(HUBUS_DIAG_DUPLICATE, hubus_master_diagnose(&fx.master, &suspects));
}

static void test_counters_track_transactions_and_errors(void)
{
    hubus_slave_frame_t f;
    hubus_burst_stats_t st;
    unsigned s = fx_add(&fx, 3);

    fx_startup(&fx);
    memset(&fx.master.counters, 0, sizeof fx.master.counters);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 3, 0x20, NULL, 0));
    TEST_ASSERT_EQUAL(HUBUS_ERR_NO_RESPONSE, hubus_master_request_info(&fx.master, 4, &f));
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_info(&fx.master, 3, &f));
    fx_write(&fx, s, 1);
    fx_write(&fx, s, 2);
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, fx.master.counters.commands);
    TEST_ASSERT_EQUAL_UINT(2, fx.master.counters.info_requests);
    TEST_ASSERT_EQUAL_UINT(1, fx.master.counters.data_requests);
    TEST_ASSERT_EQUAL_UINT(1, fx.master.counters.polls);
    TEST_ASSERT_EQUAL_UINT(1, fx.master.counters.no_response);
    TEST_ASSERT_EQUAL_UINT(0, fx.master.counters.crc_errors);
    TEST_ASSERT_EQUAL_UINT(2, fx.master.mcid_counters[3].frames);
    TEST_ASSERT_EQUAL_UINT(1, fx.master.mcid_counters[4].no_response);
    TEST_ASSERT_EQUAL_UINT(0, fx.master.mcid_counters[3].gaps);
}

/* ---- timing ----------------------------------------------------------------- */

static void test_configured_minimum_timing_is_respected(void)
{
    hubus_master_cfg_t cfg;
    hubus_master_hal_t hal;
    hubus_burst_stats_t st;
    unsigned s;

    fx_init(&fx);
    hubus_master_cfg_default(&cfg);
    cfg.t_clk_low_ns = 123456;
    cfg.t_clk_high_ns = 76543;
    cfg.t_cond_ns = 99999;
    cfg.t_turnaround_ns = 300000;
    hal = hubus_sim_master_hal(&fx.sim);
    hubus_master_init(&fx.master, &hal, &cfg);
    hubus_sim_check_timing(&fx.sim, cfg.t_clk_low_ns, cfg.t_clk_high_ns, cfg.t_cond_ns);

    s = fx_add(&fx, 20);
    fx_add(&fx, 21);
    fx_startup(&fx);
    fx_write(&fx, s, 1);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 21, 0x10, NULL, 0));
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, st.served);
    TEST_ASSERT_TRUE(fx.sim.time_ns > 0);
    TEST_ASSERT_EQUAL_UINT(0, fx.sim.timing_violations);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_startup_on_idle_bus_generates_reset);
    RUN_TEST(test_startup_clears_slave_signaling_an_event);
    RUN_TEST(test_startup_unblocks_slave_stuck_in_arbitration);
    RUN_TEST(test_startup_unblocks_slave_stuck_in_transmission);
    RUN_TEST(test_startup_reports_stuck_line);
    RUN_TEST(test_shutdown_leaves_slaves_idle);
    RUN_TEST(test_burst_on_idle_bus);
    RUN_TEST(test_burst_spec_example);
    RUN_TEST(test_burst_single_slave_lowest_and_highest_mcid);
    RUN_TEST(test_burst_all_63_slaves);
    RUN_TEST(test_burst_random_subsets);
    RUN_TEST(test_busy_high_priority_slave_does_not_starve_lower_ones);
    RUN_TEST(test_check_event_without_events);
    RUN_TEST(test_event_handshake_then_burst);
    RUN_TEST(test_service_runs_burst_only_on_events);
    RUN_TEST(test_status_capture_broadcast_then_burst);
    RUN_TEST(test_status_capture_of_a_group);
    RUN_TEST(test_status_capture_single_slave_and_read_back);
    RUN_TEST(test_send_command_exact_and_group);
    RUN_TEST(test_send_maximum_command);
    RUN_TEST(test_argument_validation);
    RUN_TEST(test_request_info_roundtrip);
    RUN_TEST(test_request_to_absent_slave_is_no_response);
    RUN_TEST(test_request_data_reads_buffer_and_clears_it);
    RUN_TEST(test_poll_reports_arbitration);
    RUN_TEST(test_fragmented_transfer_in_one_burst);
    RUN_TEST(test_next_fragment_not_ready_is_not_an_error);
    RUN_TEST(test_overwritten_data_is_reported_as_gap);
    RUN_TEST(test_counter_wrap_with_same_payload_after_four_writes);
    RUN_TEST(test_scan_finds_all_present_slaves);
    RUN_TEST(test_scan_flags_duplicate_mcid_with_unique_serials);
    RUN_TEST(test_scan_blind_spot_for_identical_duplicates);
    RUN_TEST(test_poll_discover_finds_idle_slaves_and_clears_nothing);
    RUN_TEST(test_scan_uses_far_fewer_transactions_than_full_scan);
    RUN_TEST(test_scan_finds_every_possible_mcid);
    RUN_TEST(test_scan_does_not_disturb_pending_data);
    RUN_TEST(test_full_scan_still_finds_all_present_slaves);
    RUN_TEST(test_scan_without_slaves_finds_nothing);
    RUN_TEST(test_runtime_heuristic_flags_duplicate_mcid);
    RUN_TEST(test_diagnose_many_failing_mcids_is_a_line_problem);
    RUN_TEST(test_counters_track_transactions_and_errors);
    RUN_TEST(test_configured_minimum_timing_is_respected);
    return UNITY_END();
}
