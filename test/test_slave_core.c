/*
 * Protocol tests of the slave core. A bit-level master (rawmaster.h) produces
 * exact wire traffic on the simulated bus; the slaves are the real cores.
 */
#include <string.h>

#include "hubus_arb.h"
#include "hubus_crc.h"
#include "hubus_frame.h"
#include "hubus_sim.h"
#include "rawmaster.h"
#include "unity.h"

#define MAX_TEST_SLAVES 4

typedef struct {
    hubus_slave_core_t *core;
    unsigned captures;
    unsigned commands;
    uint8_t last_op;
    uint8_t last_args[HUBUS_MAX_PAYLOAD];
    uint8_t last_len;
    uint8_t status;
    volatile uint8_t state[4];
} app_t;

static hubus_sim_t sim;
static raw_t rm;
static hubus_slave_core_t core[MAX_TEST_SLAVES];
static app_t app[MAX_TEST_SLAVES];
static const uint8_t info_bytes[] = {0xD1, 0x01, 0xDE, 0xAD};

static void on_capture(void *ctx)
{
    app_t *a = ctx;

    a->captures++;
    (void)hubus_core_write(a->core, &a->status, 1, 0);
}

static void on_command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    app_t *a = ctx;

    a->commands++;
    a->last_op = opcode;
    a->last_len = len;
    memcpy(a->last_args, args, len);
}

/* Adds slave `idx` with the given MCID to the bus. */
static void add_slave_state(unsigned idx, uint8_t mcid, int capture, uint8_t state_len)
{
    hubus_core_config_t cfg;

    memset(&app[idx], 0, sizeof app[idx]);
    app[idx].core = &core[idx];
    app[idx].status = (uint8_t)(0x40 + mcid);
    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = mcid;
    cfg.info = info_bytes;
    cfg.info_len = sizeof info_bytes;
    cfg.on_status_capture = capture ? on_capture : NULL;
    cfg.on_command = on_command;
    cfg.state = state_len ? app[idx].state : NULL;
    cfg.state_len = state_len;
    cfg.ctx = &app[idx];
    TEST_ASSERT_TRUE(hubus_core_init(&core[idx], &cfg, sim.clk, sim.data));
    TEST_ASSERT_EQUAL_UINT(idx, hubus_sim_add(&sim, &core[idx]));
}

static void add_slave(unsigned idx, uint8_t mcid, int capture)
{
    add_slave_state(idx, mcid, capture, 0);
}

void setUp(void)
{
    hubus_sim_init(&sim);
    raw_init(&rm, &sim);
}

void tearDown(void)
{
    /* No slave may ever change DATA while CLK is high. */
    TEST_ASSERT_EQUAL_UINT(0, sim.violations);
}

static void buf_write(unsigned idx, const uint8_t *data, uint8_t len, uint8_t more)
{
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_sim_slave_write(&sim, idx, data, len, more));
}

/* REQUEST_DATA to `mcid`, decoded. Returns 0 for silence. Ends with STOP. */
static size_t read_data(uint8_t mcid, uint8_t *buf, hubus_header_t *h)
{
    size_t n = raw_request(&rm, HUBUS_M_REQUEST_DATA, mcid, buf);

    if (n) {
        TEST_ASSERT_TRUE(hubus_frame_decode(buf, n, h));
    }
    raw_stop(&rm, 0);
    return n;
}

/* ---- initialisation ------------------------------------------------------ */

static void test_init_rejects_invalid_config(void)
{
    hubus_slave_core_t c;
    hubus_core_config_t cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = 0;
    TEST_ASSERT_FALSE(hubus_core_init(&c, &cfg, 0, 1)); /* MCID 0 is reserved */
    cfg.mcid = 64;
    TEST_ASSERT_FALSE(hubus_core_init(&c, &cfg, 0, 1));
    cfg.mcid = 63;
    TEST_ASSERT_TRUE(hubus_core_init(&c, &cfg, 0, 1));
    cfg.mcid = 1;
    cfg.info_len = HUBUS_INFO_APP_MAX + 1u;
    cfg.info = info_bytes;
    TEST_ASSERT_FALSE(hubus_core_init(&c, &cfg, 0, 1));
    cfg.info_len = 2;
    cfg.info = NULL;
    TEST_ASSERT_FALSE(hubus_core_init(&c, &cfg, 0, 1));
    cfg.info_len = 0;
    cfg.state_len = HUBUS_MAX_PAYLOAD + 1u; /* longer than a LEN field: rejected, not truncated */
    TEST_ASSERT_FALSE(hubus_core_init(&c, &cfg, 0, 1));
    cfg.state_len = 0;
    cfg.speed_class = HUBUS_SPEED_CLASS_MAX + 1u; /* would spill into the blind class */
    TEST_ASSERT_FALSE(hubus_core_init(&c, &cfg, 0, 1));
    cfg.speed_class = 0;
    cfg.blind_class = HUBUS_BLIND_CLASS_MAX + 1u; /* does not fit into four bits */
    TEST_ASSERT_FALSE(hubus_core_init(&c, &cfg, 0, 1));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_DISABLED, hubus_core_write(&c, info_bytes, 1, 0));
}

static void test_disabled_core_stays_silent(void)
{
    hubus_core_config_t cfg;
    uint8_t buf[40];
    uint8_t v = 1;

    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = 0;
    TEST_ASSERT_FALSE(hubus_core_init(&core[0], &cfg, sim.clk, sim.data));
    hubus_sim_add(&sim, &core[0]);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_DISABLED, hubus_core_write(&core[0], &v, 1, 0));
    TEST_ASSERT_EQUAL_UINT(0, raw_request(&rm, HUBUS_M_REQUEST_INFO, 0, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
}

/* ---- REQUEST_INFO -------------------------------------------------------- */

static void test_request_info(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    size_t n;

    add_slave(0, 9, 1);
    n = raw_request(&rm, HUBUS_M_REQUEST_INFO, 9, buf);
    raw_stop(&rm, 0);

    TEST_ASSERT_EQUAL_UINT(3 + HUBUS_INFO_APP + sizeof info_bytes, n);
    TEST_ASSERT_TRUE(hubus_frame_decode(buf, n, &h));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_S_INFO, h.type);
    TEST_ASSERT_EQUAL_UINT8(9, h.mcid);
    TEST_ASSERT_EQUAL_UINT8(0, h.field); /* MORE and GEN are 000 in SLAVE_INFO */
    TEST_ASSERT_EQUAL_UINT8(HUBUS_PROTOCOL_VERSION, buf[2 + HUBUS_INFO_VERSION]);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_CAP_EVENTS | HUBUS_CAP_STATUS_CAPTURE, buf[2 + HUBUS_INFO_CAPS]);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_RX_BUF_SIZE, buf[2 + HUBUS_INFO_MAX_CMD]);
    TEST_ASSERT_EQUAL_MEMORY(info_bytes, &buf[2 + HUBUS_INFO_APP], sizeof info_bytes);
}

static void test_request_info_without_status_capture_clears_capability(void)
{
    uint8_t buf[40];

    add_slave(0, 9, 0);
    TEST_ASSERT_TRUE(raw_request(&rm, HUBUS_M_REQUEST_INFO, 9, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_CAP_EVENTS, buf[2 + HUBUS_INFO_CAPS]); /* events, but no capture */
}

static void test_request_info_announces_the_speed_class(void)
{
    hubus_core_config_t cfg;
    uint8_t buf[40];

    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = 9;
    cfg.speed_class = HUBUS_SPEED_CLASS_MAX;
    TEST_ASSERT_TRUE(hubus_core_init(&core[0], &cfg, sim.clk, sim.data));
    hubus_sim_add(&sim, &core[0]);
    TEST_ASSERT_TRUE(raw_request(&rm, HUBUS_M_REQUEST_INFO, 9, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_SPEED_CLASS_MAX, buf[2 + HUBUS_INFO_TIMING]); /* blind class 0 in bits 7..4 */
    TEST_ASSERT_EQUAL_UINT8(HUBUS_CAP_EVENTS, buf[2 + HUBUS_INFO_CAPS]);
}

static void test_request_info_announces_the_blind_class(void)
{
    hubus_core_config_t cfg;
    uint8_t buf[40];

    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = 9;
    cfg.speed_class = 3;
    cfg.blind_class = HUBUS_BLIND_CLASS_MAX;
    TEST_ASSERT_TRUE(hubus_core_init(&core[0], &cfg, sim.clk, sim.data));
    hubus_sim_add(&sim, &core[0]);
    TEST_ASSERT_TRUE(raw_request(&rm, HUBUS_M_REQUEST_INFO, 9, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_HEX8(0xF3, buf[2 + HUBUS_INFO_TIMING]);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_BLIND_CLASS_MAX, HUBUS_BLIND_CLASS(buf[2 + HUBUS_INFO_TIMING]));
    TEST_ASSERT_EQUAL_UINT32(0u, HUBUS_BLIND_US(0u));
    TEST_ASSERT_EQUAL_UINT32(250u, HUBUS_BLIND_US(1u));
    TEST_ASSERT_EQUAL_UINT32(4000u, HUBUS_BLIND_US(5u));
    TEST_ASSERT_EQUAL_UINT32(4096000u, HUBUS_BLIND_US(15u));
}

static void test_request_info_does_not_clear_buffer(void)
{
    uint8_t buf[40];
    uint8_t v = 7;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    TEST_ASSERT_TRUE(raw_request(&rm, HUBUS_M_REQUEST_INFO, 9, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0]));
}

static void test_request_info_without_app_bytes(void)
{
    hubus_core_config_t cfg;
    uint8_t buf[40];

    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = 3;
    TEST_ASSERT_TRUE(hubus_core_init(&core[0], &cfg, sim.clk, sim.data));
    hubus_sim_add(&sim, &core[0]);
    TEST_ASSERT_EQUAL_UINT(3 + HUBUS_INFO_APP, raw_request(&rm, HUBUS_M_REQUEST_INFO, 3, buf)); /* header + protocol bytes + CRC */
    raw_stop(&rm, 0);
    TEST_ASSERT_TRUE(hubus_frame_decode(buf, 3 + HUBUS_INFO_APP, NULL));
}

/* ---- REQUEST_DATA -------------------------------------------------------- */

static void test_request_data_empty_buffer_is_silent(void)
{
    uint8_t buf[40];
    hubus_header_t h;

    add_slave(0, 9, 1);
    TEST_ASSERT_EQUAL_UINT(0, read_data(9, buf, &h));
    TEST_ASSERT_EQUAL_UINT8(1, sim.data); /* line released, bus idle */
}

static void test_request_data_returns_buffer_and_stop_clears(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    const uint8_t payload[] = {1, 2, 3, 4, 5};
    size_t n;

    add_slave(0, 9, 1);
    buf_write(0, payload, sizeof payload, 0);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0]));

    n = read_data(9, buf, &h);
    TEST_ASSERT_EQUAL_UINT(3 + sizeof payload, n);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_S_DATA, h.type);
    TEST_ASSERT_EQUAL_UINT8(9, h.mcid);
    TEST_ASSERT_EQUAL_UINT8(sizeof payload, h.len);
    TEST_ASSERT_EQUAL_UINT8(0, HUBUS_HDR_MORE(&h));
    TEST_ASSERT_EQUAL_UINT8(1, HUBUS_HDR_GEN(&h));
    TEST_ASSERT_EQUAL_MEMORY(payload, buf + 2, sizeof payload);

    /* STOP after the complete response cleared buffer and pending flag */
    TEST_ASSERT_FALSE(hubus_core_is_pending(&core[0]));
    TEST_ASSERT_EQUAL_UINT(0, read_data(9, buf, &h));
}

static void test_zero_length_buffer_is_a_valid_event(void)
{
    uint8_t buf[40];
    hubus_header_t h;

    add_slave(0, 9, 1);
    buf_write(0, NULL, 0, 0);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0]));
    TEST_ASSERT_EQUAL_UINT(3, read_data(9, buf, &h));
    TEST_ASSERT_EQUAL_UINT8(0, h.len);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&core[0]));
}

static void test_maximum_payload(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t payload[HUBUS_TX_BUF_SIZE];
    unsigned i;

    for (i = 0; i < sizeof payload; i++) {
        payload[i] = (uint8_t)(i * 7 + 1);
    }
    add_slave(0, 63, 1);
    buf_write(0, payload, sizeof payload, 0);
    TEST_ASSERT_EQUAL_UINT(3 + sizeof payload, read_data(63, buf, &h));
    TEST_ASSERT_EQUAL_MEMORY(payload, buf + 2, sizeof payload);
}

static void test_write_too_long_is_rejected_without_side_effects(void)
{
    uint8_t big[HUBUS_TX_BUF_SIZE + 1] = {0};

    add_slave(0, 9, 1);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_TOO_LONG, hubus_sim_slave_write(&sim, 0, big, sizeof big, 0));
    TEST_ASSERT_FALSE(hubus_core_is_pending(&core[0]));
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
}

static void test_gen_increments_per_write_and_wraps(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t v;
    unsigned i;

    add_slave(0, 9, 1);
    for (i = 1; i <= 9; i++) {
        v = (uint8_t)i;
        buf_write(0, &v, 1, 0);
        TEST_ASSERT_TRUE(read_data(9, buf, &h));
        TEST_ASSERT_EQUAL_UINT8(i & 3u, HUBUS_HDR_GEN(&h));
    }
}

static void test_overwrite_increments_gen_and_keeps_latest(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t a = 0x11, b = 0x22, c = 0x33;

    add_slave(0, 9, 1);
    buf_write(0, &a, 1, 0);
    buf_write(0, &b, 1, 0);
    buf_write(0, &c, 1, 0);
    TEST_ASSERT_TRUE(read_data(9, buf, &h));
    TEST_ASSERT_EQUAL_UINT8(0x33, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(3, HUBUS_HDR_GEN(&h)); /* three writes: two contents were overwritten */
}

static void test_more_flag_is_transmitted(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t v = 5;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 1);
    TEST_ASSERT_TRUE(read_data(9, buf, &h));
    TEST_ASSERT_EQUAL_UINT8(1, HUBUS_HDR_MORE(&h));
    buf_write(0, &v, 1, 0);
    TEST_ASSERT_TRUE(read_data(9, buf, &h));
    TEST_ASSERT_EQUAL_UINT8(0, HUBUS_HDR_MORE(&h));
}

/* ---- buffer clearing: STOP versus RESET --------------------------------- */

static void test_reset_after_complete_response_keeps_buffer(void)
{
    uint8_t buf[40], buf2[40];
    hubus_header_t h;
    uint8_t v = 0x5A;
    size_t n;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    n = raw_request(&rm, HUBUS_M_REQUEST_DATA, 9, buf);
    TEST_ASSERT_EQUAL_UINT(4, n);
    raw_reset(&rm); /* master saw an invalid response (for example a CRC error on its side) */

    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0]));
    TEST_ASSERT_EQUAL_UINT(n, read_data(9, buf2, &h));
    TEST_ASSERT_EQUAL_MEMORY(buf, buf2, n); /* identical retry, same GEN */
    TEST_ASSERT_FALSE(hubus_core_is_pending(&core[0]));
}

static void test_reset_preserves_gen_buffer_and_pending(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t v = 1;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    buf_write(0, &v, 1, 1);
    raw_reset(&rm);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0]));
    TEST_ASSERT_TRUE(read_data(9, buf, &h));
    TEST_ASSERT_EQUAL_UINT8(2, HUBUS_HDR_GEN(&h));
    TEST_ASSERT_EQUAL_UINT8(1, HUBUS_HDR_MORE(&h));
}

static void test_stop_after_partial_response_clears_nothing(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t v = 0x77;
    unsigned i;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);

    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_REQUEST_DATA, HUBUS_TARGET_LEN_EXACT, 9, NULL, 0);
    raw_turnaround(&rm);
    for (i = 0; i < 12; i++) {
        raw_recv_bit(&rm);
    }
    while (!sim.data && i < 23) { /* stop where the slave releases DATA, so the STOP is visible */
        raw_recv_bit(&rm);
        i++;
    }
    TEST_ASSERT_TRUE(i < 24);
    raw_clk(&rm, 0);
    raw_stop(&rm, 0);

    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0]));
    TEST_ASSERT_TRUE(read_data(9, buf, &h));
    TEST_ASSERT_EQUAL_UINT8(0x77, buf[2]);
}

static void test_lost_stop_keeps_buffer_and_next_start_resynchronises(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t v = 0x21;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    TEST_ASSERT_EQUAL_UINT(4, raw_request(&rm, HUBUS_M_REQUEST_DATA, 9, buf));
    /* The STOP never arrives at the slave; the master goes on with the next transaction. */
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0]));
    TEST_ASSERT_TRUE(read_data(9, buf, &h)); /* served again: the START cleared the "complete" memory */
    TEST_ASSERT_EQUAL_UINT8(0x21, buf[2]);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&core[0]));
}

static void test_write_during_response_is_refused(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t v = 0x10, w = 0x20;
    unsigned i;
    size_t n;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);

    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_REQUEST_DATA, HUBUS_TARGET_LEN_EXACT, 9, NULL, 0);
    raw_turnaround(&rm);
    buf[0] = 0;
    for (i = 0; i < 9; i++) { /* the response is in progress */
        raw_recv_bit(&rm);
    }
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_BUSY, hubus_sim_slave_write(&sim, 0, &w, 1, 0));
    raw_stop(&rm, 0); /* aborted: nothing is cleared */

    n = read_data(9, buf, &h);
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_UINT8(0x10, buf[2]); /* the original content, not a mix */
}

static void test_write_between_response_and_stop_survives_the_stop(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t v = 0x10, w = 0x20;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    TEST_ASSERT_EQUAL_UINT(4, raw_request(&rm, HUBUS_M_REQUEST_DATA, 9, buf));
    TEST_ASSERT_EQUAL_UINT8(0x10, buf[2]);
    buf_write(0, &w, 1, 0); /* application writes new content before the STOP arrives */
    raw_stop(&rm, 0);

    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0])); /* the STOP must not clear the new content */
    TEST_ASSERT_TRUE(read_data(9, buf, &h));
    TEST_ASSERT_EQUAL_UINT8(0x20, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(2, HUBUS_HDR_GEN(&h));
}

/* ---- addressing ---------------------------------------------------------- */

static void test_other_mcid_is_silent(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t v = 1;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    TEST_ASSERT_EQUAL_UINT(0, read_data(8, buf, &h));
    TEST_ASSERT_EQUAL_UINT(0, read_data(10, buf, &h));
    TEST_ASSERT_EQUAL_UINT(0, read_data(0, buf, &h)); /* MCID 0 matches no slave */
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0]));
}

typedef struct {
    uint16_t slots; /* arbitration slots at their position in the 12-slot pattern */
    uint8_t buf[40];
    size_t n; /* response length, 0 if nobody answered */
} group_t;

/* One complete group request transaction, ended with STOP. */
static void group_request(group_t *g, uint8_t type, uint8_t tlen, uint8_t prefix)
{
    raw_start(&rm);
    raw_send_frame(&rm, type, tlen, prefix, NULL, 0);
    raw_turnaround(&rm);
    g->slots = raw_recv_arb(&rm, tlen);
    g->n = raw_recv_frame(&rm, g->buf);
    raw_stop(&rm, 0);
}

static void test_group_requests_are_answered_by_the_matching_slave_only(void)
{
    unsigned tlen;
    group_t g;

    add_slave(0, 0x2A, 1);
    for (tlen = 0; tlen < 6; tlen++) {
        uint8_t v = 1;

        buf_write(0, &v, 1, 0);
        raw_tick(&rm);
        group_request(&g, HUBUS_M_REQUEST_INFO, (uint8_t)tlen, 0x2A);
        TEST_ASSERT_TRUE(g.n > 0);
        TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0])); /* static info: nothing cleared */
        group_request(&g, HUBUS_M_REQUEST_DATA, (uint8_t)tlen, 0x2A);
        TEST_ASSERT_TRUE(g.n > 0);
        TEST_ASSERT_FALSE(hubus_core_is_pending(&core[0])); /* the complete SLAVE_DATA was cleared by the STOP */
    }
    /* group 0xxxxx does not contain 0x2A (101010) */
    buf_write(0, &(uint8_t){1}, 1, 0);
    raw_tick(&rm);
    group_request(&g, HUBUS_M_REQUEST_DATA, 1, 0);
    TEST_ASSERT_EQUAL_UINT(0, g.n);
    group_request(&g, HUBUS_M_REQUEST_INFO, 1, 0);
    TEST_ASSERT_EQUAL_UINT(0, g.n);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0]));
}

static void test_invalid_target_len_is_ignored(void)
{
    uint8_t buf[40];
    uint8_t v = 1;

    add_slave(0, 0x2A, 1);
    buf_write(0, &v, 1, 0);
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_REQUEST_DATA, 7, 0x2A, NULL, 0);
    raw_turnaround(&rm);
    TEST_ASSERT_EQUAL_UINT(0, raw_recv_frame(&rm, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT(4, (unsigned)read_data(0x2A, buf, &(hubus_header_t){0}));
}

static void test_bad_crc_request_is_ignored(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t v = 1;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    raw_start(&rm);
    raw_send_frame_x(&rm, HUBUS_M_REQUEST_DATA, HUBUS_TARGET_LEN_EXACT, 9, NULL, 0, 0x04);
    raw_turnaround(&rm);
    TEST_ASSERT_EQUAL_UINT(0, raw_recv_frame(&rm, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_TRUE(read_data(9, buf, &h)); /* the next, valid request works */
}

/* ---- SEND_COMMAND -------------------------------------------------------- */

static void send_cmd(uint8_t tlen, uint8_t prefix, const uint8_t *payload, uint8_t len)
{
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_SEND_COMMAND, tlen, prefix, payload, len);
    raw_stop(&rm, 0);
}

static void test_status_capture_exact(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    const uint8_t op = HUBUS_OP_STATUS_CAPTURE;

    add_slave(0, 9, 1);
    add_slave(1, 10, 1);
    send_cmd(6, 9, &op, 1);
    TEST_ASSERT_EQUAL_UINT(1, app[0].captures);
    TEST_ASSERT_EQUAL_UINT(0, app[1].captures);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0])); /* capture wrote the buffer and set the flag */
    TEST_ASSERT_FALSE(hubus_core_is_pending(&core[1]));
    TEST_ASSERT_TRUE(read_data(9, buf, &h));
    TEST_ASSERT_EQUAL_UINT8(0x40 + 9, buf[2]);
}

static void test_status_capture_group_and_broadcast(void)
{
    const uint8_t op = HUBUS_OP_STATUS_CAPTURE;

    add_slave(0, 40, 1); /* 101000 */
    add_slave(1, 41, 1); /* 101001 */
    add_slave(2, 42, 1); /* 101010 */
    send_cmd(5, 40, &op, 1); /* 10100x */
    TEST_ASSERT_EQUAL_UINT(1, app[0].captures);
    TEST_ASSERT_EQUAL_UINT(1, app[1].captures);
    TEST_ASSERT_EQUAL_UINT(0, app[2].captures);
    send_cmd(0, 0, &op, 1); /* broadcast */
    TEST_ASSERT_EQUAL_UINT(2, app[0].captures);
    TEST_ASSERT_EQUAL_UINT(2, app[1].captures);
    TEST_ASSERT_EQUAL_UINT(1, app[2].captures);
}

static void test_commands_are_never_answered(void)
{
    uint8_t buf[40];
    const uint8_t op = HUBUS_OP_STATUS_CAPTURE;

    add_slave(0, 9, 1);
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_SEND_COMMAND, HUBUS_TARGET_LEN_EXACT, 9, &op, 1);
    raw_turnaround(&rm);
    TEST_ASSERT_EQUAL_UINT(0, raw_recv_frame(&rm, buf)); /* even where a turnaround slot would follow */
    raw_stop(&rm, 0);
}

static void test_command_executes_when_frame_completes(void)
{
    const uint8_t cmd[] = {0x10, 1, 2, 3};

    add_slave(0, 9, 1);
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_SEND_COMMAND, HUBUS_TARGET_LEN_EXACT, 9, cmd, sizeof cmd);
    TEST_ASSERT_EQUAL_UINT(1, app[0].commands); /* before the STOP */
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT(1, app[0].commands);
}

static void test_application_command_gets_opcode_and_arguments(void)
{
    const uint8_t cmd[] = {0xA5, 9, 8, 7, 6};

    add_slave(0, 9, 1);
    send_cmd(6, 9, cmd, sizeof cmd);
    TEST_ASSERT_EQUAL_UINT(1, app[0].commands);
    TEST_ASSERT_EQUAL_UINT8(0xA5, app[0].last_op);
    TEST_ASSERT_EQUAL_UINT8(4, app[0].last_len);
    TEST_ASSERT_EQUAL_MEMORY(&cmd[1], app[0].last_args, 4);
    TEST_ASSERT_EQUAL_UINT(0, app[0].captures);
}

static void test_command_without_arguments(void)
{
    const uint8_t cmd[] = {0x10};

    add_slave(0, 9, 1);
    send_cmd(6, 9, cmd, 1);
    TEST_ASSERT_EQUAL_UINT(1, app[0].commands);
    TEST_ASSERT_EQUAL_UINT8(0, app[0].last_len);
}

static void test_maximum_size_command(void)
{
    uint8_t cmd[HUBUS_MAX_PAYLOAD];
    unsigned i;

    cmd[0] = 0x80;
    for (i = 1; i < sizeof cmd; i++) {
        cmd[i] = (uint8_t)i;
    }
    add_slave(0, 9, 1);
    send_cmd(6, 9, cmd, sizeof cmd);
    TEST_ASSERT_EQUAL_UINT(1, app[0].commands);
    TEST_ASSERT_EQUAL_UINT8(30, app[0].last_len);
    TEST_ASSERT_EQUAL_MEMORY(&cmd[1], app[0].last_args, 30);
}

static void test_reserved_opcodes_are_ignored(void)
{
    unsigned op;

    add_slave(0, 9, 1);
    for (op = 0x01; op <= HUBUS_OP_RESERVED_MAX; op++) {
        uint8_t cmd[2];

        cmd[0] = (uint8_t)op;
        cmd[1] = 0;
        send_cmd(6, 9, cmd, 2);
    }
    TEST_ASSERT_EQUAL_UINT(0, app[0].commands);
    TEST_ASSERT_EQUAL_UINT(0, app[0].captures);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&core[0]));
}

static void test_empty_command_is_ignored(void)
{
    add_slave(0, 9, 1);
    send_cmd(6, 9, NULL, 0);
    TEST_ASSERT_EQUAL_UINT(0, app[0].commands);
    TEST_ASSERT_EQUAL_UINT(0, app[0].captures);
}

static void test_command_with_bad_crc_is_not_executed(void)
{
    const uint8_t cmd[] = {0x10, 1};

    add_slave(0, 9, 1);
    raw_start(&rm);
    raw_send_frame_x(&rm, HUBUS_M_SEND_COMMAND, HUBUS_TARGET_LEN_EXACT, 9, cmd, 2, 0x80);
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT(0, app[0].commands);
}

static void test_status_capture_unsupported_is_ignored(void)
{
    const uint8_t op = HUBUS_OP_STATUS_CAPTURE;

    add_slave(0, 9, 0);
    send_cmd(6, 9, &op, 1);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&core[0]));
}

static void test_command_for_other_slave_is_ignored(void)
{
    const uint8_t cmd[] = {0x10};

    add_slave(0, 9, 1);
    send_cmd(6, 8, cmd, 1);
    send_cmd(6, 0, cmd, 1);
    TEST_ASSERT_EQUAL_UINT(0, app[0].commands);
}

/* ---- group requests: arbitration, then the winner answers ---------------- */

static void make_pending(unsigned idx)
{
    uint8_t v = 1;

    buf_write(idx, &v, 1, 0);
    raw_tick(&rm); /* idle event handshake: DATA is released after one pulse */
}

static void test_group_request_with_nobody_pending(void)
{
    group_t g;

    add_slave(0, 9, 1);
    group_request(&g, HUBUS_M_REQUEST_DATA, 0, 0);
    TEST_ASSERT_EQUAL_HEX16(0, g.slots);
    TEST_ASSERT_EQUAL_UINT(0, g.n);
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
}

static void test_single_pending_slave_answers_every_mcid(void)
{
    unsigned mcid;

    for (mcid = 1; mcid < 64; mcid++) {
        hubus_arb_result_t r;
        hubus_header_t h;
        group_t g;
        uint8_t v = (uint8_t)(mcid * 3);

        hubus_sim_init(&sim);
        raw_init(&rm, &sim);
        add_slave(0, (uint8_t)mcid, 1);
        buf_write(0, &v, 1, 0);
        raw_tick(&rm);
        group_request(&g, HUBUS_M_REQUEST_DATA, 0, 0);
        TEST_ASSERT_TRUE(hubus_arb_decode(g.slots, 0, 0, &r));
        TEST_ASSERT_EQUAL_UINT8(mcid, r.winner);
        TEST_ASSERT_EQUAL_UINT8(0, r.n_masks);
        TEST_ASSERT_EQUAL_UINT(4, g.n);
        TEST_ASSERT_TRUE(hubus_frame_decode(g.buf, g.n, &h));
        TEST_ASSERT_EQUAL_UINT8(HUBUS_S_DATA, h.type);
        TEST_ASSERT_EQUAL_UINT8(mcid, h.mcid);
        TEST_ASSERT_EQUAL_UINT8(v, g.buf[2]);
        TEST_ASSERT_FALSE(hubus_core_is_pending(&core[0])); /* the STOP after the complete frame cleared it */
        TEST_ASSERT_EQUAL_UINT8(0, sim.violations);
    }
}

static void test_burst_example_from_the_spec(void)
{
    hubus_arb_result_t r;
    hubus_header_t h;
    group_t g;

    add_slave(0, 42, 1);
    add_slave(1, 40, 1);
    add_slave(2, 25, 1);
    add_slave(3, 15, 1);
    make_pending(0);
    make_pending(1);
    make_pending(2);
    make_pending(3);

    /* docs/spec/05-arbitration.md section 5.6, request 1: arb bits 101010, handoff after bit 5 and bit 1 */
    group_request(&g, HUBUS_M_REQUEST_DATA, 0, 0);
    TEST_ASSERT_EQUAL_HEX16((1u << 0) | (1u << 1) | (1u << 4) | (1u << 8) | (1u << 9), g.slots);
    TEST_ASSERT_TRUE(hubus_arb_decode(g.slots, 0, 0, &r));
    TEST_ASSERT_EQUAL_UINT8(42, r.winner);
    TEST_ASSERT_EQUAL_UINT8(2, r.n_masks);
    TEST_ASSERT_TRUE(hubus_frame_decode(g.buf, g.n, &h));
    TEST_ASSERT_EQUAL_UINT8(42, h.mcid);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&core[0]));
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[1])); /* the losers keep their data */
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[2]));
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[3]));

    /* request 2: group 10100x (2 slots), 40 has only a 0 bit left: both slots high, 40 still answers */
    group_request(&g, HUBUS_M_REQUEST_DATA, 5, 40);
    TEST_ASSERT_EQUAL_HEX16(0, g.slots);
    TEST_ASSERT_TRUE(hubus_arb_decode(g.slots, 40, 5, &r));
    TEST_ASSERT_EQUAL_UINT8(40, r.winner);
    TEST_ASSERT_EQUAL_UINT8(0, r.n_masks);
    TEST_ASSERT_TRUE(hubus_frame_decode(g.buf, g.n, &h));
    TEST_ASSERT_EQUAL_UINT8(40, h.mcid);

    /* request 3: group 0xxxxx (10 slots): 25 wins, 15 loses at bit 4 */
    group_request(&g, HUBUS_M_REQUEST_DATA, 1, 0);
    TEST_ASSERT_TRUE(hubus_arb_decode(g.slots, 0, 1, &r));
    TEST_ASSERT_EQUAL_UINT8(25, r.winner);
    TEST_ASSERT_EQUAL_UINT8(1, r.n_masks);
    TEST_ASSERT_EQUAL_UINT8(2, r.masks[0].len);
    TEST_ASSERT_TRUE(hubus_frame_decode(g.buf, g.n, &h));
    TEST_ASSERT_EQUAL_UINT8(25, h.mcid);

    /* request 4: group 00xxxx (8 slots) -> 15 */
    group_request(&g, HUBUS_M_REQUEST_DATA, 2, 0);
    TEST_ASSERT_TRUE(hubus_arb_decode(g.slots, 0, 2, &r));
    TEST_ASSERT_EQUAL_UINT8(15, r.winner);
    TEST_ASSERT_TRUE(hubus_frame_decode(g.buf, g.n, &h));
    TEST_ASSERT_EQUAL_UINT8(15, h.mcid);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&core[3]));
}

static void test_only_pending_slaves_contend(void)
{
    hubus_arb_result_t r;
    group_t g;

    add_slave(0, 42, 1);
    add_slave(1, 50, 1); /* higher MCID but nothing buffered */
    make_pending(0);
    group_request(&g, HUBUS_M_REQUEST_DATA, 0, 0);
    TEST_ASSERT_TRUE(hubus_arb_decode(g.slots, 0, 0, &r));
    TEST_ASSERT_EQUAL_UINT8(42, r.winner);
    TEST_ASSERT_EQUAL_UINT8(0, r.n_masks);
}

static void test_prefix_mask_selects_contenders(void)
{
    hubus_arb_result_t r;
    group_t g;

    add_slave(0, 42, 1); /* 101010 */
    add_slave(1, 21, 1); /* 010101 */
    make_pending(0);
    make_pending(1);
    group_request(&g, HUBUS_M_REQUEST_DATA, 1, 0); /* 0xxxxx */
    TEST_ASSERT_TRUE(hubus_arb_decode(g.slots, 0, 1, &r));
    TEST_ASSERT_EQUAL_UINT8(21, r.winner);
    group_request(&g, HUBUS_M_REQUEST_DATA, 1, 0x20); /* 1xxxxx */
    TEST_ASSERT_TRUE(hubus_arb_decode(g.slots, 0x20, 1, &r));
    TEST_ASSERT_EQUAL_UINT8(42, r.winner);
    group_request(&g, HUBUS_M_REQUEST_DATA, 2, 0x30); /* 11xxxx: nobody */
    TEST_ASSERT_EQUAL_HEX16(0, g.slots);
    TEST_ASSERT_EQUAL_UINT(0, g.n);
}

static void test_group_info_request_lets_every_slave_contend(void)
{
    hubus_arb_result_t r;
    hubus_header_t h;
    group_t g;

    add_slave(0, 42, 1); /* nothing buffered */
    add_slave(1, 40, 1);
    group_request(&g, HUBUS_M_REQUEST_INFO, 0, 0);
    TEST_ASSERT_TRUE(hubus_arb_decode(g.slots, 0, 0, &r));
    TEST_ASSERT_EQUAL_UINT8(42, r.winner);
    TEST_ASSERT_EQUAL_UINT8(1, r.n_masks);
    TEST_ASSERT_EQUAL_UINT8(5, r.masks[0].len);
    TEST_ASSERT_EQUAL_UINT8(40, r.masks[0].prefix);
    TEST_ASSERT_TRUE(hubus_frame_decode(g.buf, g.n, &h));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_S_INFO, h.type);
    TEST_ASSERT_EQUAL_UINT8(42, h.mcid);
}

static void test_group_info_request_changes_no_state(void)
{
    uint8_t v = 7;
    group_t g;

    add_slave(0, 42, 1);
    add_slave(1, 40, 1);
    buf_write(1, &v, 1, 0);
    raw_tick(&rm);
    group_request(&g, HUBUS_M_REQUEST_INFO, 0, 0);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&core[0]));
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[1])); /* SLAVE_INFO is static: the STOP clears nothing */
    TEST_ASSERT_EQUAL_UINT8(1, core[1].buf_len);
}

static void test_group_info_request_respects_prefix_mask(void)
{
    hubus_arb_result_t r;
    group_t g;

    add_slave(0, 42, 1); /* 101010 */
    add_slave(1, 21, 1); /* 010101 */
    group_request(&g, HUBUS_M_REQUEST_INFO, 1, 0);
    TEST_ASSERT_TRUE(hubus_arb_decode(g.slots, 0, 1, &r));
    TEST_ASSERT_EQUAL_UINT8(21, r.winner);
    group_request(&g, HUBUS_M_REQUEST_INFO, 2, 0x30);
    TEST_ASSERT_EQUAL_UINT(0, g.n);
}

static void test_lone_contender_with_all_open_bits_zero_still_answers(void)
{
    hubus_header_t h;
    group_t g;

    add_slave(0, 40, 1); /* 101000 */
    make_pending(0);
    group_request(&g, HUBUS_M_REQUEST_DATA, 5, 40); /* 10100x */
    TEST_ASSERT_EQUAL_HEX16(0, g.slots);
    TEST_ASSERT_TRUE(g.n > 0);
    TEST_ASSERT_TRUE(hubus_frame_decode(g.buf, g.n, &h));
    TEST_ASSERT_EQUAL_UINT8(40, h.mcid);
}

static void test_group_request_with_bad_crc_does_not_arbitrate(void)
{
    uint8_t buf[40];

    add_slave(0, 42, 1);
    make_pending(0);
    raw_start(&rm);
    raw_send_frame_x(&rm, HUBUS_M_REQUEST_DATA, 0, 0, NULL, 0, 0x10);
    raw_turnaround(&rm);
    TEST_ASSERT_EQUAL_HEX16(0, raw_recv_arb(&rm, 0));
    TEST_ASSERT_EQUAL_UINT(0, raw_recv_frame(&rm, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0]));
}

static void test_failed_group_response_ended_with_reset_keeps_data(void)
{
    uint8_t buf[40];

    add_slave(0, 42, 1);
    make_pending(0);
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_REQUEST_DATA, 0, 0, NULL, 0);
    raw_turnaround(&rm);
    (void)raw_recv_arb(&rm, 0);
    TEST_ASSERT_TRUE(raw_recv_frame(&rm, buf) > 0);
    raw_reset(&rm);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0])); /* RESET preserves buffer and pending flag */
}

/* ---- REQUEST_STATE ---------------------------------------------------------- */

static void test_request_state_returns_the_live_state(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    size_t n;

    add_slave_state(0, 9, 1, 3);
    app[0].state[0] = 0x11;
    app[0].state[1] = 0x22;
    app[0].state[2] = 0x33;
    n = raw_request(&rm, HUBUS_M_REQUEST_STATE, 9, buf);
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT(3 + 3, n);
    TEST_ASSERT_TRUE(hubus_frame_decode(buf, n, &h));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_S_DATA, h.type);
    TEST_ASSERT_EQUAL_UINT8(9, h.mcid);
    TEST_ASSERT_EQUAL_UINT8(0, h.field); /* MORE = 0, GEN = 00 */
    TEST_ASSERT_EQUAL_UINT8(3, h.len);
    TEST_ASSERT_EQUAL_UINT8(0x11, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x33, buf[4]);

    /* it is live: a change of the application state shows in the next request */
    app[0].state[2] = 0x44;
    n = raw_request(&rm, HUBUS_M_REQUEST_STATE, 9, buf);
    raw_stop(&rm, 0);
    TEST_ASSERT_TRUE(hubus_frame_decode(buf, n, NULL));
    TEST_ASSERT_EQUAL_UINT8(0x44, buf[4]);
}

static void test_request_state_changes_neither_buffer_nor_gen(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t v = 7;
    size_t n;

    add_slave_state(0, 9, 1, 2);
    buf_write(0, &v, 1, 0);
    n = raw_request(&rm, HUBUS_M_REQUEST_STATE, 9, buf);
    raw_stop(&rm, 0);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0])); /* the STOP after a state frame clears nothing */
    TEST_ASSERT_EQUAL_UINT8(1, core[0].buf_len);

    /* the event is still delivered afterwards, with its own GEN */
    TEST_ASSERT_TRUE(read_data(9, buf, &h) > 0);
    TEST_ASSERT_EQUAL_UINT8(1, HUBUS_HDR_GEN(&h));
    TEST_ASSERT_EQUAL_UINT8(7, buf[2]);
    TEST_ASSERT_FALSE(hubus_core_is_pending(&core[0]));
}

static void test_request_state_without_state_is_silent(void)
{
    uint8_t buf[40];

    add_slave(0, 9, 1);
    TEST_ASSERT_EQUAL_UINT(0, raw_request(&rm, HUBUS_M_REQUEST_STATE, 9, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
}

static void test_slave_info_announces_the_state_capability(void)
{
    uint8_t buf[40];

    add_slave_state(0, 9, 1, 1);
    add_slave(1, 10, 1);
    add_slave_state(2, 11, 0, 1);
    TEST_ASSERT_TRUE(raw_request(&rm, HUBUS_M_REQUEST_INFO, 9, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_CAP_EVENTS | HUBUS_CAP_STATUS_CAPTURE | HUBUS_CAP_STATE, buf[2 + HUBUS_INFO_CAPS]);
    TEST_ASSERT_TRUE(raw_request(&rm, HUBUS_M_REQUEST_INFO, 10, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_CAP_EVENTS | HUBUS_CAP_STATUS_CAPTURE, buf[2 + HUBUS_INFO_CAPS]);
    TEST_ASSERT_TRUE(raw_request(&rm, HUBUS_M_REQUEST_INFO, 11, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_CAP_EVENTS | HUBUS_CAP_STATE, buf[2 + HUBUS_INFO_CAPS]);
}

static void test_group_state_request_lets_every_slave_with_a_state_contend(void)
{
    hubus_arb_result_t r;
    hubus_header_t h;
    group_t g;

    add_slave_state(0, 42, 1, 1); /* has a state, nothing pending */
    add_slave_state(1, 40, 1, 1);
    add_slave(2, 50, 1);          /* higher MCID but no state: drops out */
    group_request(&g, HUBUS_M_REQUEST_STATE, 0, 0);
    TEST_ASSERT_TRUE(hubus_arb_decode(g.slots, 0, 0, &r));
    TEST_ASSERT_EQUAL_UINT8(42, r.winner);
    TEST_ASSERT_EQUAL_UINT8(1, r.n_masks);
    TEST_ASSERT_EQUAL_UINT8(40, r.masks[0].prefix);
    TEST_ASSERT_TRUE(hubus_frame_decode(g.buf, g.n, &h));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_S_DATA, h.type);
    TEST_ASSERT_EQUAL_UINT8(42, h.mcid);
}

static void test_event_request_drops_slaves_without_pending_data_while_state_request_does_not(void)
{
    hubus_arb_result_t r;
    group_t g;
    uint8_t v = 1;

    add_slave_state(0, 42, 1, 1);
    add_slave_state(1, 25, 1, 1);
    buf_write(1, &v, 1, 0); /* only 25 has an event */
    raw_tick(&rm);

    group_request(&g, HUBUS_M_REQUEST_DATA, 0, 0);
    TEST_ASSERT_TRUE(hubus_arb_decode(g.slots, 0, 0, &r));
    TEST_ASSERT_EQUAL_UINT8(25, r.winner); /* 42 has a state but no change: out */
    TEST_ASSERT_EQUAL_UINT8(0, r.n_masks);

    group_request(&g, HUBUS_M_REQUEST_STATE, 0, 0);
    TEST_ASSERT_TRUE(hubus_arb_decode(g.slots, 0, 0, &r));
    TEST_ASSERT_EQUAL_UINT8(42, r.winner);
    TEST_ASSERT_EQUAL_UINT8(1, r.n_masks);
}

static void test_write_during_state_transmission_is_not_busy(void)
{
    uint8_t buf[40];
    uint8_t v = 3;

    add_slave_state(0, 9, 1, 2);
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_REQUEST_STATE, HUBUS_TARGET_LEN_EXACT, 9, NULL, 0);
    raw_turnaround(&rm);
    (void)raw_recv_bit(&rm); /* the state frame is in progress */
    TEST_ASSERT_EQUAL_UINT(HUBUS_ST_TX, core[0].state);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_core_write(&core[0], &v, 1, 0)); /* the event buffer is not being sent */
    raw_reset(&rm);
    (void)buf;
}

static void test_maximum_state_and_state_without_pointer(void)
{
    hubus_core_config_t cfg;
    uint8_t buf[40];
    static volatile uint8_t st[31];
    unsigned i;

    for (i = 0; i < sizeof st; i++) {
        st[i] = (uint8_t)(i + 1);
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = 5;
    cfg.state = st;
    cfg.state_len = 31;
    TEST_ASSERT_TRUE(hubus_core_init(&core[0], &cfg, sim.clk, sim.data));
    hubus_sim_add(&sim, &core[0]);
    TEST_ASSERT_EQUAL_UINT(3 + 31, raw_request(&rm, HUBUS_M_REQUEST_STATE, 5, buf));
    raw_stop(&rm, 0);
    TEST_ASSERT_TRUE(hubus_frame_decode(buf, 34, NULL));
    TEST_ASSERT_EQUAL_UINT8(31, buf[2 + 30]);

    /* a length without a pointer means: no state */
    hubus_sim_init(&sim);
    raw_init(&rm, &sim);
    cfg.state = NULL;
    cfg.state_len = 2;
    TEST_ASSERT_TRUE(hubus_core_init(&core[1], &cfg, sim.clk, sim.data));
    hubus_sim_add(&sim, &core[1]);
    TEST_ASSERT_EQUAL_UINT(0, raw_request(&rm, HUBUS_M_REQUEST_STATE, 5, buf));
    raw_stop(&rm, 0);
}

/* ---- idle event signaling ------------------------------------------------ */

static void test_write_while_idle_signals_event(void)
{
    uint8_t v = 1;

    add_slave(0, 9, 1);
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
    buf_write(0, &v, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(0, sim.data); /* DATA pulled low while CLK is low */
    TEST_ASSERT_EQUAL_UINT8(0, sim.clk);
}

static void test_event_is_released_after_one_complete_clock_pulse(void)
{
    uint8_t v = 1;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    raw_clk(&rm, 1);
    TEST_ASSERT_EQUAL_UINT8(0, sim.data); /* still low while CLK is high: releasing now would look like STOP */
    TEST_ASSERT_EQUAL_UINT(0, sim.n_stop);
    raw_clk(&rm, 0);
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0]));
    TEST_ASSERT_EQUAL_UINT(0, sim.n_stop);
    TEST_ASSERT_EQUAL_UINT(0, sim.n_start);
}

static void test_event_without_a_running_master_stays_low(void)
{
    uint8_t v = 1;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    /* no timeout in the slave: DATA stays low indefinitely */
    hubus_sim_settle(&sim);
    TEST_ASSERT_EQUAL_UINT8(0, sim.data);
    TEST_ASSERT_EQUAL_UINT(HUBUS_ST_EVENT, core[0].state);
}

static void test_several_slaves_signal_and_release_together(void)
{
    uint8_t v = 1;

    add_slave(0, 9, 1);
    add_slave(1, 10, 1);
    add_slave(2, 11, 1);
    buf_write(0, &v, 1, 0);
    buf_write(1, &v, 1, 0);
    buf_write(2, &v, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(0, sim.data);
    raw_tick(&rm);
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
}

static void test_pending_slave_signals_again_after_stop(void)
{
    uint8_t v = 1;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    raw_tick(&rm);
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
    group_request(&(group_t){0}, HUBUS_M_REQUEST_DATA, 2, 0x30); /* a request that does not address the slave */
    TEST_ASSERT_EQUAL_UINT8(0, sim.data); /* STOP, CLK low, slave still pending: announces itself again */
}

static void test_no_signal_between_back_to_back_transactions(void)
{
    uint8_t v = 1;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    raw_tick(&rm);

    /* STOP with CLK held high, then START immediately: CLK never falls in idle */
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_REQUEST_DATA, 2, 0x30, NULL, 0);
    raw_turnaround(&rm);
    (void)raw_recv_arb(&rm, 2);
    raw_stop(&rm, 1);
    TEST_ASSERT_EQUAL_UINT8(1, sim.clk);
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
    raw_start(&rm);
    TEST_ASSERT_EQUAL_UINT(2, sim.n_start);
    raw_send_frame(&rm, HUBUS_M_REQUEST_DATA, 2, 0x30, NULL, 0);
    raw_turnaround(&rm);
    (void)raw_recv_arb(&rm, 2);
    raw_stop(&rm, 0);
    TEST_ASSERT_EQUAL_UINT8(0, sim.data); /* only after CLK goes low does it announce itself */
}

static void test_second_write_does_not_signal_again_while_pending(void)
{
    uint8_t v = 1;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    raw_tick(&rm);
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
    buf_write(0, &v, 1, 0); /* overwrite of unread data */
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
}

static void test_write_during_frame_does_not_touch_the_line(void)
{
    uint8_t buf[40];
    uint8_t v = 1;

    add_slave(0, 9, 1);
    add_slave(1, 10, 1);
    raw_start(&rm);
    raw_send_bit(&rm, 0);
    buf_write(1, &v, 1, 0); /* CLK is low here, but the bus is busy: the slave is receiving */
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
    raw_stop(&rm, 0);
    (void)buf;
}

/* ---- line conditions and robustness ------------------------------------- */

static void test_start_discards_partial_frame(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t v = 3;
    unsigned i;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    raw_tick(&rm);
    raw_start(&rm);
    for (i = 0; i < 11; i++) { /* part of a frame */
        raw_send_bit(&rm, (int)(i & 1));
    }
    TEST_ASSERT_TRUE(read_data(9, buf, &h)); /* a new START resynchronises */
}

static void test_stop_in_the_middle_returns_to_idle(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t v = 3;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    raw_tick(&rm);
    raw_start(&rm);
    raw_send_bit(&rm, 1);
    raw_send_bit(&rm, 0);
    raw_stop(&rm, 0);
    TEST_ASSERT_TRUE(read_data(9, buf, &h));
}

static void test_reset_during_arbitration_returns_to_idle(void)
{
    uint8_t v = 1;
    int i;

    add_slave(0, 63, 1);
    buf_write(0, &v, 1, 0);
    raw_tick(&rm);
    raw_start(&rm);
    raw_send_frame(&rm, HUBUS_M_REQUEST_DATA, 0, 0, NULL, 0);
    raw_turnaround(&rm);
    for (i = 0; i < 3; i++) { /* arbitration bit 5, handoff 5, arbitration bit 4 ... */
        raw_clk(&rm, 1);
        raw_clk(&rm, 0);
    }
    TEST_ASSERT_EQUAL_UINT(HUBUS_ST_ARB, core[0].state);
    TEST_ASSERT_EQUAL_UINT8(1, sim.data); /* handoff slot: released */
    /* RESET by hand, to look at the line before CLK returns low */
    raw_pull(&rm, 0);
    raw_clk(&rm, 1);
    raw_pull(&rm, 1);
    raw_pull(&rm, 0);
    TEST_ASSERT_EQUAL_UINT8(1, sim.data); /* the arbitrating slave let go of DATA */
    TEST_ASSERT_EQUAL_UINT(HUBUS_ST_IDLE, core[0].state);
    raw_clk(&rm, 0);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0]));
    TEST_ASSERT_EQUAL_UINT8(0, sim.data); /* it has unread data and announces it again */
}

static void test_data_changes_while_clk_low_are_ignored(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t v = 3;
    int i;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    raw_tick(&rm);
    for (i = 0; i < 5; i++) { /* noise on DATA in idle with CLK low */
        raw_pull(&rm, 1);
        raw_pull(&rm, 0);
    }
    TEST_ASSERT_EQUAL_UINT(0, sim.n_start);
    TEST_ASSERT_EQUAL_UINT(0, sim.n_stop);
    TEST_ASSERT_TRUE(read_data(9, buf, &h));
}

static void test_extra_clock_pulse_corrupts_only_that_frame(void)
{
    uint8_t buf[40];
    hubus_header_t h;
    uint8_t v = 3;
    unsigned i;

    add_slave(0, 9, 1);
    buf_write(0, &v, 1, 0);
    raw_tick(&rm);
    raw_start(&rm);
    for (i = 0; i < 6; i++) {
        raw_send_bit(&rm, 0);
    }
    hubus_sim_glitch_clk(&sim, 0); /* extra edge seen only by the slave */
    raw_send_frame(&rm, HUBUS_M_REQUEST_DATA, HUBUS_TARGET_LEN_EXACT, 9, NULL, 0);
    raw_stop(&rm, 0);
    TEST_ASSERT_TRUE(hubus_core_is_pending(&core[0]));
    TEST_ASSERT_TRUE(read_data(9, buf, &h)); /* next START: back in sync */
}

static void test_glitch_while_idle_has_no_effect(void)
{
    uint8_t buf[40];
    hubus_header_t h;

    add_slave(0, 9, 1);
    hubus_sim_glitch_clk(&sim, 0);
    hubus_sim_glitch_clk(&sim, 0);
    TEST_ASSERT_EQUAL_UINT8(1, sim.data);
    TEST_ASSERT_EQUAL_UINT(0, read_data(9, buf, &h));
}

static void test_state_after_reset_is_idle_and_data_released(void)
{
    add_slave(0, 9, 1);
    raw_start(&rm);
    raw_send_bit(&rm, 1);
    raw_reset(&rm);
    TEST_ASSERT_EQUAL_UINT(HUBUS_ST_IDLE, core[0].state);
    TEST_ASSERT_EQUAL_UINT8(0, hubus_core_data_low(&core[0]));
    TEST_ASSERT_EQUAL_UINT(1, sim.n_reset);
}

static void test_core_has_no_time_base(void)
{
    uint8_t v = 1;
    uint8_t buf[40];
    hubus_header_t h;
    uint32_t i;

    add_slave(0, 9, 1);
    raw_start(&rm);
    raw_send_bit(&rm, 1);
    /* arbitrarily long pause in the middle of a frame: the slave just waits */
    for (i = 0; i < 100000; i++) {
        sim.time_ns += 1000000;
    }
    raw_stop(&rm, 0);
    buf_write(0, &v, 1, 0);
    raw_tick(&rm);
    TEST_ASSERT_TRUE(read_data(9, buf, &h));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_invalid_config);
    RUN_TEST(test_disabled_core_stays_silent);
    RUN_TEST(test_request_info);
    RUN_TEST(test_request_info_without_status_capture_clears_capability);
    RUN_TEST(test_request_info_announces_the_speed_class);
    RUN_TEST(test_request_info_announces_the_blind_class);
    RUN_TEST(test_request_info_does_not_clear_buffer);
    RUN_TEST(test_request_info_without_app_bytes);
    RUN_TEST(test_request_data_empty_buffer_is_silent);
    RUN_TEST(test_request_data_returns_buffer_and_stop_clears);
    RUN_TEST(test_zero_length_buffer_is_a_valid_event);
    RUN_TEST(test_maximum_payload);
    RUN_TEST(test_write_too_long_is_rejected_without_side_effects);
    RUN_TEST(test_gen_increments_per_write_and_wraps);
    RUN_TEST(test_overwrite_increments_gen_and_keeps_latest);
    RUN_TEST(test_more_flag_is_transmitted);
    RUN_TEST(test_reset_after_complete_response_keeps_buffer);
    RUN_TEST(test_reset_preserves_gen_buffer_and_pending);
    RUN_TEST(test_stop_after_partial_response_clears_nothing);
    RUN_TEST(test_lost_stop_keeps_buffer_and_next_start_resynchronises);
    RUN_TEST(test_write_during_response_is_refused);
    RUN_TEST(test_write_between_response_and_stop_survives_the_stop);
    RUN_TEST(test_other_mcid_is_silent);
    RUN_TEST(test_group_requests_are_answered_by_the_matching_slave_only);
    RUN_TEST(test_invalid_target_len_is_ignored);
    RUN_TEST(test_bad_crc_request_is_ignored);
    RUN_TEST(test_status_capture_exact);
    RUN_TEST(test_status_capture_group_and_broadcast);
    RUN_TEST(test_commands_are_never_answered);
    RUN_TEST(test_command_executes_when_frame_completes);
    RUN_TEST(test_application_command_gets_opcode_and_arguments);
    RUN_TEST(test_command_without_arguments);
    RUN_TEST(test_maximum_size_command);
    RUN_TEST(test_reserved_opcodes_are_ignored);
    RUN_TEST(test_empty_command_is_ignored);
    RUN_TEST(test_command_with_bad_crc_is_not_executed);
    RUN_TEST(test_status_capture_unsupported_is_ignored);
    RUN_TEST(test_command_for_other_slave_is_ignored);
    RUN_TEST(test_group_request_with_nobody_pending);
    RUN_TEST(test_single_pending_slave_answers_every_mcid);
    RUN_TEST(test_burst_example_from_the_spec);
    RUN_TEST(test_only_pending_slaves_contend);
    RUN_TEST(test_prefix_mask_selects_contenders);
    RUN_TEST(test_group_info_request_lets_every_slave_contend);
    RUN_TEST(test_group_info_request_changes_no_state);
    RUN_TEST(test_group_info_request_respects_prefix_mask);
    RUN_TEST(test_lone_contender_with_all_open_bits_zero_still_answers);
    RUN_TEST(test_group_request_with_bad_crc_does_not_arbitrate);
    RUN_TEST(test_failed_group_response_ended_with_reset_keeps_data);
    RUN_TEST(test_request_state_returns_the_live_state);
    RUN_TEST(test_request_state_changes_neither_buffer_nor_gen);
    RUN_TEST(test_request_state_without_state_is_silent);
    RUN_TEST(test_slave_info_announces_the_state_capability);
    RUN_TEST(test_group_state_request_lets_every_slave_with_a_state_contend);
    RUN_TEST(test_event_request_drops_slaves_without_pending_data_while_state_request_does_not);
    RUN_TEST(test_write_during_state_transmission_is_not_busy);
    RUN_TEST(test_maximum_state_and_state_without_pointer);
    RUN_TEST(test_write_while_idle_signals_event);
    RUN_TEST(test_event_is_released_after_one_complete_clock_pulse);
    RUN_TEST(test_event_without_a_running_master_stays_low);
    RUN_TEST(test_several_slaves_signal_and_release_together);
    RUN_TEST(test_pending_slave_signals_again_after_stop);
    RUN_TEST(test_no_signal_between_back_to_back_transactions);
    RUN_TEST(test_second_write_does_not_signal_again_while_pending);
    RUN_TEST(test_write_during_frame_does_not_touch_the_line);
    RUN_TEST(test_start_discards_partial_frame);
    RUN_TEST(test_stop_in_the_middle_returns_to_idle);
    RUN_TEST(test_reset_during_arbitration_returns_to_idle);
    RUN_TEST(test_data_changes_while_clk_low_are_ignored);
    RUN_TEST(test_extra_clock_pulse_corrupts_only_that_frame);
    RUN_TEST(test_glitch_while_idle_has_no_effect);
    RUN_TEST(test_state_after_reset_is_idle_and_data_released);
    RUN_TEST(test_core_has_no_time_base);
    return UNITY_END();
}
