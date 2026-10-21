#include "hubus_master.h"

#include <string.h>

#include "hubus_crc.h"
#include "hubus_frame.h"

void hubus_master_cfg_default(hubus_master_cfg_t *cfg)
{
    cfg->t_clk_low_ns = 50000u;
    cfg->t_clk_high_ns = 50000u;
    cfg->t_sample_ns = 25000u;
    cfg->t_cond_ns = 50000u;
    cfg->t_turnaround_ns = 200000u;
    cfg->line_fault_ticks = 300u; /* a bit more than one maximum frame (34 bytes = 272 bits) */
    cfg->retry_budget = 2u;
    cfg->follow_more = 1u;
    cfg->max_fragments = 32u;
    cfg->dup_fail_threshold = 3u;
    cfg->line_problem_count = 3u;
}

const char *hubus_status_str(hubus_status_t s)
{
    switch (s) {
    case HUBUS_OK: return "ok";
    case HUBUS_ERR_ARG: return "invalid argument";
    case HUBUS_ERR_NO_RESPONSE: return "no response";
    case HUBUS_ERR_CRC: return "crc error";
    case HUBUS_ERR_MISMATCH: return "type or mcid mismatch";
    case HUBUS_ERR_LINE: return "line readback mismatch";
    case HUBUS_ERR_LINE_FAULT: return "line fault (DATA stuck low)";
    case HUBUS_ERR_STACK: return "pending mask stack overflow";
    }
    return "unknown";
}

void hubus_master_init(hubus_master_t *m, const hubus_master_hal_t *hal, const hubus_master_cfg_t *cfg)
{
    unsigned i;

    memset(m, 0, sizeof *m);
    m->hal = *hal;
    m->cfg = *cfg;
    if (m->cfg.t_sample_ns > m->cfg.t_clk_high_ns) {
        m->cfg.t_sample_ns = m->cfg.t_clk_high_ns;
    }
    if (m->cfg.t_turnaround_ns < m->cfg.t_clk_low_ns) {
        m->cfg.t_turnaround_ns = m->cfg.t_clk_low_ns;
    }
    for (i = 0; i <= HUBUS_MCID_MAX; i++) {
        hubus_gen_reset(&m->gen[i]);
    }
    m->hal.set_data_low(m->hal.ctx, 0);
    m->hal.set_clk(m->hal.ctx, 0);
    m->clk = 0;
}

/* ---- primitives ------------------------------------------------------ */

static void wait_ns(hubus_master_t *m, uint32_t ns)
{
    if (ns && m->hal.delay_ns) {
        m->hal.delay_ns(m->hal.ctx, ns);
    }
}

static void set_clk(hubus_master_t *m, uint8_t high)
{
    m->hal.set_clk(m->hal.ctx, high);
    m->clk = high;
}

static void set_data_low(hubus_master_t *m, uint8_t low)
{
    m->hal.set_data_low(m->hal.ctx, low);
}

static uint8_t data_high(hubus_master_t *m)
{
    return m->hal.read_data(m->hal.ctx) ? 1u : 0u;
}

/* One clock period without touching DATA. */
static void tick(hubus_master_t *m)
{
    wait_ns(m, m->cfg.t_clk_low_ns);
    set_clk(m, 1);
    wait_ns(m, m->cfg.t_clk_high_ns);
    set_clk(m, 0);
}

/* Transmit one bit (1 = pull DATA low) and verify by readback. */
static hubus_status_t send_bit(hubus_master_t *m, uint8_t bit)
{
    uint8_t high;

    set_data_low(m, bit);
    wait_ns(m, m->cfg.t_clk_low_ns);
    set_clk(m, 1);
    wait_ns(m, m->cfg.t_sample_ns);
    high = data_high(m);
    wait_ns(m, m->cfg.t_clk_high_ns - m->cfg.t_sample_ns);
    set_clk(m, 0);
    /* Wired-AND: what we pulled low must read low, and nobody else may pull
     * DATA low while we release it. */
    return (high == (bit ? 0u : 1u)) ? HUBUS_OK : HUBUS_ERR_LINE;
}

/* Receive one bit; returns 1 if DATA was low. `low_ns` is the CLK low time before the rising edge. */
static uint8_t recv_bit(hubus_master_t *m, uint32_t low_ns)
{
    uint8_t bit;

    wait_ns(m, low_ns);
    set_clk(m, 1);
    wait_ns(m, m->cfg.t_sample_ns);
    bit = (uint8_t)(data_high(m) ? 0u : 1u);
    wait_ns(m, m->cfg.t_clk_high_ns - m->cfg.t_sample_ns);
    set_clk(m, 0);
    return bit;
}

/* Turnaround slot: everybody releases DATA, the value is ignored. */
static void turnaround(hubus_master_t *m)
{
    set_data_low(m, 0);
    tick(m);
}

/* ---- line conditions -------------------------------------------------- */

/* Common precondition: DATA released and high, CLK known. */
static hubus_status_t prepare_condition(hubus_master_t *m)
{
    uint16_t ticks = 0;

    set_data_low(m, 0);
    if (m->clk) {
        return data_high(m) ? HUBUS_OK : HUBUS_ERR_LINE;
    }
    wait_ns(m, m->cfg.t_clk_low_ns); /* let slaves release DATA after the last falling edge */
    while (!data_high(m)) {
        /* DATA low while idle is event signaling (pending slaves announce themselves
         * whenever CLK falls): confirm with clock pulses until it is released. */
        if (++ticks > m->cfg.line_fault_ticks) {
            return HUBUS_ERR_LINE_FAULT;
        }
        tick(m);
        wait_ns(m, m->cfg.t_clk_low_ns);
    }
    return HUBUS_OK;
}

static void finish_idle(hubus_master_t *m, uint8_t keep_high)
{
    if (!keep_high && m->clk) {
        set_clk(m, 0);
        wait_ns(m, m->cfg.t_clk_low_ns);
    }
}

static hubus_status_t recover(hubus_master_t *m);

static hubus_status_t cond_start(hubus_master_t *m)
{
    hubus_status_t r = prepare_condition(m);

    if (r != HUBUS_OK) {
        return r;
    }
    if (!m->clk) {
        set_clk(m, 1);
        wait_ns(m, m->cfg.t_cond_ns);
        if (!data_high(m)) {
            /* A slave started event signaling just before CLK rose (docs/PROTOCOL.md section 7.1).
             * Finish the clock pulse so it releases DATA; the caller recovers and repeats. */
            wait_ns(m, m->cfg.t_clk_high_ns);
            set_clk(m, 0);
            return HUBUS_ERR_LINE;
        }
    }
    set_data_low(m, 1); /* DATA falls while CLK is high */
    wait_ns(m, m->cfg.t_cond_ns);
    set_clk(m, 0);
    return HUBUS_OK;
}

static hubus_status_t cond_stop(hubus_master_t *m)
{
    hubus_status_t r = HUBUS_OK;

    set_data_low(m, 1);
    wait_ns(m, m->cfg.t_clk_low_ns);
    set_clk(m, 1);
    wait_ns(m, m->cfg.t_cond_ns);
    set_data_low(m, 0); /* DATA rises while CLK is high */
    wait_ns(m, m->cfg.t_cond_ns);
    if (!data_high(m)) {
        r = HUBUS_ERR_LINE;
    }
    finish_idle(m, m->hold);
    return r;
}

static hubus_status_t cond_reset(hubus_master_t *m)
{
    hubus_status_t r = prepare_condition(m);

    if (r != HUBUS_OK) {
        return r;
    }
    if (!m->clk) {
        set_clk(m, 1);
        wait_ns(m, m->cfg.t_cond_ns);
        if (!data_high(m)) {
            wait_ns(m, m->cfg.t_clk_high_ns);
            set_clk(m, 0); /* event signaling raced with the condition, see cond_start() */
            return HUBUS_ERR_LINE;
        }
    }
    set_data_low(m, 1); /* START ... */
    wait_ns(m, m->cfg.t_cond_ns);
    set_data_low(m, 0); /* ... immediately followed by STOP, no clock pulse in between */
    wait_ns(m, m->cfg.t_cond_ns);
    if (!data_high(m)) {
        r = HUBUS_ERR_LINE;
    }
    finish_idle(m, m->hold);
    return r;
}

/* Startup steps 1-3, also the error recovery sequence. Leaves CLK low. */
static hubus_status_t recover_impl(hubus_master_t *m)
{
    uint16_t ticks = 0;
    uint8_t saved_hold = m->hold;
    hubus_status_t r;

    m->hold = 0;
    set_data_low(m, 0);
    if (m->clk) {
        set_clk(m, 0);
    }
    while (!data_high(m)) {
        if (++ticks > m->cfg.line_fault_ticks) {
            m->hold = saved_hold;
            return HUBUS_ERR_LINE_FAULT;
        }
        tick(m);
        wait_ns(m, m->cfg.t_clk_low_ns);
    }
    r = cond_reset(m);
    if (r != HUBUS_OK) {
        /* One more attempt after additional clocking, then report. */
        while (!data_high(m) && ticks++ < m->cfg.line_fault_ticks) {
            tick(m);
        }
        r = data_high(m) ? cond_reset(m) : HUBUS_ERR_LINE_FAULT;
    }
    m->hold = saved_hold;
    return r == HUBUS_ERR_LINE ? HUBUS_ERR_LINE_FAULT : r;
}

static hubus_status_t recover(hubus_master_t *m)
{
    hubus_status_t r;

    m->counters.recoveries++;
    r = recover_impl(m);
    if (r == HUBUS_ERR_LINE_FAULT) {
        m->counters.line_faults++;
    }
    return r;
}

hubus_status_t hubus_master_startup(hubus_master_t *m)
{
    return recover(m);
}

hubus_status_t hubus_master_shutdown(hubus_master_t *m)
{
    uint8_t saved_hold = m->hold;
    hubus_status_t r;

    m->hold = 0;
    r = cond_reset(m);
    m->hold = saved_hold;
    return r;
}

hubus_status_t hubus_master_check_event(hubus_master_t *m, uint8_t *event)
{
    uint16_t ticks = 0;

    *event = 0;
    if (m->clk) {
        set_clk(m, 0);
    }
    set_data_low(m, 0);
    wait_ns(m, m->cfg.t_clk_low_ns);
    if (data_high(m)) {
        return HUBUS_OK;
    }
    /* DATA low while idle: event signaling. Confirm with clock pulses. */
    while (!data_high(m)) {
        if (++ticks > m->cfg.line_fault_ticks) {
            return HUBUS_ERR_LINE_FAULT;
        }
        tick(m);
        wait_ns(m, m->cfg.t_clk_low_ns);
    }
    *event = 1;
    return HUBUS_OK;
}

/* ---- frames ------------------------------------------------------------ */

static hubus_status_t send_byte(hubus_master_t *m, uint8_t b)
{
    int i;

    for (i = 7; i >= 0; i--) {
        hubus_status_t r = send_bit(m, (uint8_t)((b >> i) & 1));
        if (r != HUBUS_OK) {
            return r;
        }
    }
    return HUBUS_OK;
}

static hubus_status_t send_frame(hubus_master_t *m, uint8_t type, uint8_t target_len, uint8_t prefix,
                                 const uint8_t *payload, uint8_t len)
{
    uint8_t hdr[HUBUS_HEADER_BYTES];
    uint8_t crc = HUBUS_CRC_INIT;
    hubus_status_t r;
    uint8_t i;

    hubus_header_pack(hdr, type, target_len, (uint8_t)(prefix & hubus_prefix_mask(target_len)), len);
    for (i = 0; i < HUBUS_HEADER_BYTES; i++) {
        crc = hubus_crc8_update(crc, hdr[i]);
        r = send_byte(m, hdr[i]);
        if (r != HUBUS_OK) {
            return r;
        }
    }
    for (i = 0; i < len; i++) {
        crc = hubus_crc8_update(crc, payload[i]);
        r = send_byte(m, payload[i]);
        if (r != HUBUS_OK) {
            return r;
        }
    }
    return send_byte(m, hubus_crc8_final(crc));
}

/* Starts a transaction: START and master frame. A line error runs the recovery sequence. */
static hubus_status_t begin_transaction(hubus_master_t *m, uint8_t type, uint8_t target_len, uint8_t prefix,
                                        const uint8_t *payload, uint8_t len)
{
    hubus_status_t r = cond_start(m);

    if (r == HUBUS_OK) {
        r = send_frame(m, type, target_len, prefix, payload, len);
    }
    if (r != HUBUS_OK) {
        if (r == HUBUS_ERR_LINE) {
            m->counters.line_errors++;
        }
        hubus_status_t rec = recover(m);
        return rec == HUBUS_OK ? r : rec;
    }
    return HUBUS_OK;
}

/* ---- transactions ---------------------------------------------------- */

hubus_status_t hubus_master_send_command(hubus_master_t *m, uint8_t target_len, uint8_t prefix, uint8_t opcode,
                                         const uint8_t *args, uint8_t args_len)
{
    uint8_t payload[HUBUS_MAX_PAYLOAD];
    hubus_status_t r;
    uint8_t attempts;

    if (target_len > HUBUS_TARGET_LEN_EXACT || args_len > HUBUS_MAX_PAYLOAD - 1u || (args_len && !args) ||
        prefix > HUBUS_MCID_MAX) {
        return HUBUS_ERR_ARG;
    }
    m->counters.commands++;
    payload[0] = opcode;
    if (args_len) {
        memcpy(&payload[1], args, args_len);
    }
    /* A corrupted frame (readback mismatch) is rejected by every slave's CRC; repeating it is safe. */
    for (attempts = 0;; attempts++) {
        r = begin_transaction(m, HUBUS_M_SEND_COMMAND, target_len, prefix, payload, (uint8_t)(args_len + 1u));
        if (r == HUBUS_OK) {
            break;
        }
        if (r != HUBUS_ERR_LINE || attempts >= m->cfg.retry_budget) {
            return r;
        }
    }
    r = cond_stop(m);
    if (r != HUBUS_OK) {
        (void)recover(m);
    }
    return r;
}

hubus_status_t hubus_master_status_capture(hubus_master_t *m, uint8_t target_len, uint8_t prefix)
{
    return hubus_master_send_command(m, target_len, prefix, HUBUS_OP_STATUS_CAPTURE, NULL, 0);
}

/*
 * One REQUEST_INFO / REQUEST_DATA transaction. With target_len < 6 the matching slaves arbitrate and the winner
 * answers in the same transaction (docs/ARBITRATION.md); `res` then tells winner and derived masks, also when the
 * response is invalid. With target_len == 6 the addressed slave answers directly.
 */
static hubus_status_t request_impl(hubus_master_t *m, uint8_t req_type, uint8_t resp_type, uint8_t target_len,
                                   uint8_t prefix, hubus_arb_result_t *res, hubus_slave_frame_t *out)
{
    uint8_t buf[HUBUS_MAX_FRAME_BYTES];
    hubus_header_t h;
    uint8_t cur;
    uint16_t nbits;
    uint16_t total_bits;
    uint16_t slots = 0;
    uint8_t nslots = (uint8_t)(2u * (HUBUS_MCID_BITS - target_len));
    uint8_t i;
    hubus_status_t r;
    uint8_t ok;
    uint8_t attempts;

    res->contender = 0;
    res->winner = 0;
    res->n_masks = 0;
    if (target_len > HUBUS_TARGET_LEN_EXACT || prefix > HUBUS_MCID_MAX ||
        (target_len == HUBUS_TARGET_LEN_EXACT && prefix < HUBUS_MCID_MIN)) {
        return HUBUS_ERR_ARG;
    }
    prefix &= hubus_prefix_mask(target_len);
    for (attempts = 0;; attempts++) {
        r = begin_transaction(m, req_type, target_len, prefix, NULL, 0);
        if (r == HUBUS_OK) {
            break;
        }
        /* a group request is repeated on line errors; an exact one is left to the caller */
        if (target_len == HUBUS_TARGET_LEN_EXACT || r != HUBUS_ERR_LINE || attempts >= m->cfg.retry_budget) {
            return r;
        }
    }
    turnaround(m);

    /* Arbitration slots of the open MCID bits, placed at their position of the 12-slot pattern. */
    for (i = 0; i < nslots; i++) {
        if (recv_bit(m, i == 0 ? m->cfg.t_turnaround_ns : m->cfg.t_clk_low_ns)) {
            slots |= (uint16_t)(1u << (2u * target_len + i));
        }
    }

    /* Slave type codes start with 1: a released first bit means no response / no contender. */
    if (!recv_bit(m, nslots == 0u ? m->cfg.t_turnaround_ns : m->cfg.t_clk_low_ns)) {
        /* RESET instead of STOP: a slave that sent a partial response keeps its buffer. */
        if (cond_reset(m) != HUBUS_OK) {
            (void)recover(m);
        }
        return HUBUS_ERR_NO_RESPONSE;
    }
    (void)hubus_arb_decode(slots, prefix, target_len, res);
    cur = 1;
    nbits = 1;
    total_bits = (HUBUS_HEADER_BYTES + 1u) * 8u;
    while (nbits < total_bits) {
        cur = (uint8_t)((cur << 1) | recv_bit(m, m->cfg.t_clk_low_ns));
        nbits++;
        if ((nbits & 7u) == 0u) {
            buf[nbits / 8u - 1u] = cur;
            cur = 0;
            if (nbits == 16u) {
                total_bits = (uint16_t)((HUBUS_HEADER_BYTES + 1u + (buf[1] & 0x1Fu)) * 8u);
            }
        }
    }

    ok = hubus_frame_decode(buf, total_bits / 8u, &h);
    if (!ok) {
        r = HUBUS_ERR_CRC;
    } else if (h.type != resp_type || h.mcid != res->winner) {
        r = HUBUS_ERR_MISMATCH;
    } else {
        r = HUBUS_OK;
    }

    if (r == HUBUS_OK) {
        hubus_status_t end = cond_stop(m);
        if (end != HUBUS_OK) {
            (void)recover(m);
        }
        if (out) {
            out->type = h.type;
            out->mcid = h.mcid;
            out->more = HUBUS_HDR_MORE(&h);
            out->gen = HUBUS_HDR_GEN(&h);
            out->len = h.len;
            memcpy(out->payload, &buf[HUBUS_HEADER_BYTES], h.len);
        }
        return HUBUS_OK;
    }
    /* Invalid response: RESET keeps buffer and pending flag in the slave. */
    if (cond_reset(m) != HUBUS_OK) {
        (void)recover(m);
    }
    return r;
}

static hubus_status_t request(hubus_master_t *m, uint8_t req_type, uint8_t resp_type, uint8_t target_len,
                              uint8_t prefix, hubus_arb_result_t *res, hubus_slave_frame_t *out)
{
    hubus_arb_result_t local;
    hubus_status_t r;
    uint8_t mcid;

    if (!res) {
        res = &local;
    }
    if (req_type == HUBUS_M_REQUEST_INFO) {
        m->counters.info_requests++;
    } else if (req_type == HUBUS_M_REQUEST_STATE) {
        m->counters.state_requests++;
    } else {
        m->counters.data_requests++;
    }
    if (target_len < HUBUS_TARGET_LEN_EXACT) {
        m->counters.polls++;
    }
    r = request_impl(m, req_type, resp_type, target_len, prefix, res, out);
    /* A group request that nobody answers is an empty group, not an error. */
    if (r == HUBUS_ERR_NO_RESPONSE && target_len < HUBUS_TARGET_LEN_EXACT) {
        return HUBUS_OK;
    }
    mcid = res->contender ? res->winner : (uint8_t)(prefix & HUBUS_MCID_MAX);
    switch (r) {
    case HUBUS_OK:
        m->mcid_counters[mcid].frames++;
        break;
    case HUBUS_ERR_NO_RESPONSE:
        m->counters.no_response++;
        m->mcid_counters[mcid].no_response++;
        break;
    case HUBUS_ERR_CRC:
        m->counters.crc_errors++;
        m->mcid_counters[mcid].crc_errors++;
        break;
    case HUBUS_ERR_MISMATCH:
        m->counters.mismatches++;
        m->mcid_counters[mcid].crc_errors++;
        break;
    default:
        break;
    }
    return r;
}

hubus_status_t hubus_master_request_info(hubus_master_t *m, uint8_t mcid, hubus_slave_frame_t *out)
{
    return request(m, HUBUS_M_REQUEST_INFO, HUBUS_S_INFO, HUBUS_TARGET_LEN_EXACT, mcid, NULL, out);
}

hubus_status_t hubus_master_request_data(hubus_master_t *m, uint8_t mcid, hubus_slave_frame_t *out)
{
    return request(m, HUBUS_M_REQUEST_DATA, HUBUS_S_DATA, HUBUS_TARGET_LEN_EXACT, mcid, NULL, out);
}

hubus_status_t hubus_master_request_state(hubus_master_t *m, uint8_t mcid, hubus_slave_frame_t *out)
{
    return request(m, HUBUS_M_REQUEST_STATE, HUBUS_S_DATA, HUBUS_TARGET_LEN_EXACT, mcid, NULL, out);
}

hubus_status_t hubus_master_request_group(hubus_master_t *m, uint8_t req_type, uint8_t prefix, uint8_t target_len,
                                          hubus_arb_result_t *res, hubus_slave_frame_t *out)
{
    if ((req_type != HUBUS_M_REQUEST_INFO && req_type != HUBUS_M_REQUEST_DATA && req_type != HUBUS_M_REQUEST_STATE) ||
        target_len >= HUBUS_TARGET_LEN_EXACT || !res) {
        return HUBUS_ERR_ARG;
    }
    return request(m, req_type, req_type == HUBUS_M_REQUEST_INFO ? HUBUS_S_INFO : HUBUS_S_DATA, target_len, prefix,
                   res, out);
}

/* ---- burst ------------------------------------------------------------ */

static void note_failure(hubus_master_t *m, uint8_t mcid, hubus_status_t r)
{
    if ((r == HUBUS_ERR_CRC || r == HUBUS_ERR_MISMATCH) && m->fail[mcid] < 255u) {
        m->fail[mcid]++;
    }
}

/*
 * Handles the response of `mcid` (with retries). `pre` is the response that already arrived in the group request
 * that selected `mcid`, with its status `pre_r`; NULL if there is none (exact request). Returns an error only for
 * conditions that abort the burst.
 */
static hubus_status_t serve(hubus_master_t *m, uint8_t mcid, uint8_t polled, const hubus_slave_frame_t *pre,
                            hubus_status_t pre_r, hubus_data_fn on_data, void *ctx, hubus_burst_stats_t *st)
{
    uint8_t attempts = 0;
    uint8_t fragments = 0;

    for (;;) {
        hubus_slave_frame_t f;
        hubus_status_t r;

        if (pre) {
            r = pre_r;
            if (r == HUBUS_OK) {
                f = *pre;
            }
            pre = NULL;
        } else {
            r = hubus_master_request_data(m, mcid, &f);
        }

        if (r == HUBUS_OK) {
            hubus_gen_tracker_t *t = &m->gen[mcid];
            uint8_t last = t->gen;
            hubus_gen_result_t g = hubus_gen_eval(t, f.gen, f.more, f.payload, f.len);

            m->fail[mcid] = 0;
            attempts = 0;
            if (g == HUBUS_GEN_DUPLICATE) {
                m->mcid_counters[mcid].duplicates++;
                st->duplicates++;
            } else {
                uint8_t lost = 0;
                if (g == HUBUS_GEN_GAP) {
                    st->gaps++;
                    m->mcid_counters[mcid].gaps++;
                    m->mcid_counters[mcid].lost += hubus_gen_lost(last, f.gen);
                    lost = hubus_gen_lost(last, f.gen);
                }
                st->served++;
                if (on_data) {
                    on_data(ctx, &f, g, lost);
                }
            }
            if (f.more && m->cfg.follow_more && ++fragments < m->cfg.max_fragments) {
                polled = 0; /* the next fragment may not be ready: silence is fine */
                continue;
            }
            return HUBUS_OK;
        }
        if (r == HUBUS_ERR_LINE_FAULT) {
            return r;
        }
        note_failure(m, mcid, r);
        if (r == HUBUS_ERR_NO_RESPONSE && !polled) {
            return HUBUS_OK; /* nothing buffered (anymore): not an error */
        }
        if (attempts >= m->cfg.retry_budget) {
            st->skipped++;
            return HUBUS_OK; /* data stays buffered; the slave announces itself again */
        }
        attempts++;
        st->retries++;
    }
}

hubus_status_t hubus_master_burst(hubus_master_t *m, hubus_data_fn on_data, void *ctx, hubus_burst_stats_t *stats)
{
    hubus_burst_stats_t local;
    hubus_burst_stats_t *st = stats ? stats : &local;
    hubus_mask_t mask = {0, 0};
    hubus_status_t result = HUBUS_OK;

    memset(st, 0, sizeof *st);
    hubus_mask_stack_clear(&m->stack);
    m->hold = 1;

    for (;;) {
        if (mask.len == HUBUS_TARGET_LEN_EXACT) {
            /* One MCID exactly: a plain exactly addressed request. */
            hubus_status_t r = serve(m, mask.prefix, 0, NULL, HUBUS_OK, on_data, ctx, st);
            if (r != HUBUS_OK) {
                result = r;
                break;
            }
        } else {
            hubus_arb_result_t res;
            hubus_slave_frame_t f;
            hubus_status_t r = hubus_master_request_group(m, HUBUS_M_REQUEST_DATA, mask.prefix, mask.len, &res, &f);
            uint8_t i;

            st->polls++;
            if (!res.contender) {
                if (r != HUBUS_OK) {
                    result = r;
                    break;
                }
            } else {
                /* The masks are valid even if the winner's frame was not. */
                for (i = 0; i < res.n_masks; i++) {
                    if (!hubus_mask_stack_push(&m->stack, res.masks[i])) {
                        result = HUBUS_ERR_STACK;
                        break;
                    }
                }
                if (result != HUBUS_OK) {
                    break;
                }
                r = serve(m, res.winner, 1, &f, r, on_data, ctx, st);
                if (r != HUBUS_OK) {
                    result = r;
                    break;
                }
            }
        }
        if (!hubus_mask_stack_pop(&m->stack, &mask)) {
            break;
        }
    }

    m->hold = 0;
    if (result == HUBUS_OK || result == HUBUS_ERR_STACK) {
        finish_idle(m, 0);
    }
    if (result == HUBUS_ERR_STACK) {
        (void)recover(m);
    }
    return result;
}

hubus_status_t hubus_master_service(hubus_master_t *m, hubus_data_fn on_data, void *ctx, hubus_burst_stats_t *stats)
{
    uint8_t event;
    hubus_status_t r = hubus_master_check_event(m, &event);

    if (stats) {
        memset(stats, 0, sizeof *stats);
    }
    if (r != HUBUS_OK || !event) {
        return r;
    }
    return hubus_master_burst(m, on_data, ctx, stats);
}

/* ---- state ------------------------------------------------------------------- */

/* Asks `mcid` exactly for its state, repeating failed attempts within the retry budget. */
static hubus_status_t state_retry(hubus_master_t *m, uint8_t mcid, hubus_status_t r, hubus_slave_frame_t *f)
{
    uint8_t attempts = 0;

    while (r != HUBUS_OK && r != HUBUS_ERR_LINE_FAULT && r != HUBUS_ERR_ARG && attempts < m->cfg.retry_budget) {
        note_failure(m, mcid, r);
        attempts++;
        r = hubus_master_request_state(m, mcid, f);
    }
    return r;
}

hubus_status_t hubus_master_read_states(hubus_master_t *m, hubus_state_fn on_state, void *ctx, uint16_t *count)
{
    hubus_mask_t mask = {0, 0};
    hubus_status_t result = HUBUS_OK;
    uint16_t n = 0;

    hubus_mask_stack_clear(&m->stack);
    m->hold = 1;

    for (;;) {
        hubus_slave_frame_t f;
        hubus_status_t r;
        uint8_t mcid = 0;
        uint8_t have = 0;

        if (mask.len == HUBUS_TARGET_LEN_EXACT) {
            /* One MCID exactly: a slave lost at bit 0 against it, so it exists. */
            mcid = mask.prefix;
            have = 1;
            r = hubus_master_request_state(m, mcid, &f);
        } else {
            hubus_arb_result_t res;
            uint8_t i;

            r = hubus_master_request_group(m, HUBUS_M_REQUEST_STATE, mask.prefix, mask.len, &res, &f);
            if (res.contender) {
                for (i = 0; i < res.n_masks; i++) {
                    if (!hubus_mask_stack_push(&m->stack, res.masks[i])) {
                        result = HUBUS_ERR_STACK;
                        break;
                    }
                }
                mcid = res.winner;
                have = 1;
            }
        }
        if (result != HUBUS_OK) {
            break;
        }
        if (have) {
            r = state_retry(m, mcid, r, &f);
            if (r == HUBUS_OK) {
                n++;
                if (on_state) {
                    on_state(ctx, &f);
                }
            } else if (r == HUBUS_ERR_LINE_FAULT || r == HUBUS_ERR_ARG) {
                result = r;
                break;
            }
        } else if (r != HUBUS_OK) {
            result = r;
            break;
        }
        if (!hubus_mask_stack_pop(&m->stack, &mask)) {
            break;
        }
    }

    m->hold = 0;
    if (result == HUBUS_OK || result == HUBUS_ERR_STACK) {
        finish_idle(m, 0);
    }
    if (result == HUBUS_ERR_STACK) {
        (void)recover(m);
    }
    if (count) {
        *count = n;
    }
    return result;
}

/* ---- commissioning --------------------------------------------------- */

/* REQUEST_INFO to one MCID with retries on CRC failures. */
static hubus_status_t verify_mcid(hubus_master_t *m, hubus_scan_t *scan, unsigned mcid)
{
    uint8_t attempts;
    uint8_t crc_fail = 0;
    uint8_t present = 0;

    for (attempts = 0; attempts <= m->cfg.retry_budget && !present; attempts++) {
        hubus_status_t r = hubus_master_request_info(m, (uint8_t)mcid, &scan->info[mcid]);

        if (r == HUBUS_OK) {
            present = 1;
            m->fail[mcid] = 0;
        } else if (r == HUBUS_ERR_CRC || r == HUBUS_ERR_MISMATCH) {
            crc_fail = 1;
            note_failure(m, (uint8_t)mcid, r);
        } else if (r == HUBUS_ERR_LINE_FAULT) {
            return r;
        } else if (r == HUBUS_ERR_NO_RESPONSE && !crc_fail) {
            break; /* silence on the first attempt: MCID unused */
        }
    }
    if (present) {
        scan->state[mcid] = HUBUS_SCAN_PRESENT;
    } else if (crc_fail) {
        scan->state[mcid] = HUBUS_SCAN_SUSPECT;
    }
    return HUBUS_OK;
}

hubus_status_t hubus_master_scan(hubus_master_t *m, hubus_scan_t *scan)
{
    hubus_mask_t mask = {0, 0};
    hubus_status_t result = HUBUS_OK;

    memset(scan, 0, sizeof *scan);
    hubus_mask_stack_clear(&m->stack);
    m->hold = 1;

    for (;;) {
        if (mask.len == HUBUS_TARGET_LEN_EXACT) {
            /* A slave lost at bit 0 against this MCID, so it exists: read its info exactly. */
            result = verify_mcid(m, scan, mask.prefix);
            if (result != HUBUS_OK) {
                break;
            }
        } else {
            hubus_arb_result_t res;
            hubus_slave_frame_t f;
            hubus_status_t r = hubus_master_request_group(m, HUBUS_M_REQUEST_INFO, mask.prefix, mask.len, &res, &f);
            uint8_t i;

            if (!res.contender) {
                if (r != HUBUS_OK) {
                    result = r;
                    break;
                }
            } else {
                for (i = 0; i < res.n_masks; i++) {
                    if (!hubus_mask_stack_push(&m->stack, res.masks[i])) {
                        result = HUBUS_ERR_STACK;
                        break;
                    }
                }
                if (result != HUBUS_OK) {
                    break;
                }
                if (r == HUBUS_OK) {
                    scan->info[res.winner] = f;
                    scan->state[res.winner] = HUBUS_SCAN_PRESENT;
                    m->fail[res.winner] = 0;
                } else {
                    /* CRC failure: a duplicate MCID answers like this. Ask the winner exactly, with retries. */
                    note_failure(m, res.winner, r);
                    result = verify_mcid(m, scan, res.winner);
                    if (result != HUBUS_OK) {
                        break;
                    }
                }
            }
        }
        if (!hubus_mask_stack_pop(&m->stack, &mask)) {
            break;
        }
    }

    m->hold = 0;
    if (result == HUBUS_OK || result == HUBUS_ERR_STACK) {
        finish_idle(m, 0);
    }
    if (result == HUBUS_ERR_STACK) {
        (void)recover(m);
    }
    return result;
}

uint8_t hubus_master_diagnose(const hubus_master_t *m, uint64_t *suspects)
{
    unsigned mcid;
    unsigned n = 0;
    uint64_t mask = 0;

    for (mcid = HUBUS_MCID_MIN; mcid <= HUBUS_MCID_MAX; mcid++) {
        if (m->fail[mcid] >= m->cfg.dup_fail_threshold) {
            mask |= (uint64_t)1 << mcid;
            n++;
        }
    }
    if (suspects) {
        *suspects = mask;
    }
    if (n == 0) {
        return HUBUS_DIAG_OK;
    }
    return n > m->cfg.line_problem_count ? HUBUS_DIAG_LINE_PROBLEM : HUBUS_DIAG_DUPLICATE;
}
