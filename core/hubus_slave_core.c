#include "hubus_slave_core.h"

#include <string.h>

#include "hubus_crc.h"
#include "hubus_frame.h"

#define LOST_NONE 0xFFu

static void go_ignore(hubus_slave_core_t *c)
{
    c->data_low = 0;
    c->state = HUBUS_ST_IGNORE;
}

#if HUBUS_ENABLE_EVENTS
/* Idle event signaling: hold DATA low until the next falling CLK edge. */
static void start_event(hubus_slave_core_t *c)
{
    c->armed = 0;
    c->data_low = 1;
    c->state = HUBUS_ST_EVENT;
}
#endif

static void tx_start(hubus_slave_core_t *c);

/* ---- line conditions ------------------------------------------------ */

static void on_start(hubus_slave_core_t *c)
{
    c->data_low = 0;
#if HUBUS_ENABLE_EVENTS
    c->tx_complete = 0;
    c->armed = 0;
#endif
    c->nbits = 0;
    c->idx = 0;
    c->crc = HUBUS_CRC_INIT;
    c->state = HUBUS_ST_RX;
}

static void on_stop(hubus_slave_core_t *c)
{
#if HUBUS_ENABLE_EVENTS
    /* START immediately followed by STOP is a RESET: tx_complete is already
     * cleared by the START, so buffer, GEN, and pending flag stay intact. */
    c->armed = 1;
    if (c->tx_complete) {
        c->pending = 0;
    }
#endif
    c->data_low = 0;
    c->state = HUBUS_ST_IDLE;
}

/* ---- receive -------------------------------------------------------- */

/* 1 if the first `n` (0..3) bits of the 3-bit groups `a` and `b` differ. */
static uint8_t prefix3_differs(uint8_t a, uint8_t b, uint8_t n)
{
    return (uint8_t)(((a ^ b) & (7u << (3u - n)) & 7u) != 0u);
}

static void rx_done(hubus_slave_core_t *c, uint8_t crc_ok)
{
    if (!crc_ok) {
        go_ignore(c);
        return;
    }
    switch (c->rx_type) {
    case HUBUS_M_SEND_COMMAND:
        go_ignore(c);
        if (c->rx_len >= 1u && c->rx_len <= HUBUS_RX_BUF_SIZE) {
            uint8_t op = c->cmd[0];
#if HUBUS_ENABLE_EVENTS
            if (op == HUBUS_OP_STATUS_CAPTURE) {
                if (c->on_status_capture) {
                    c->on_status_capture(c->ctx);
                }
            } else
#endif
#if HUBUS_ENABLE_UPDATE
            if (op == HUBUS_OP_UPDATE_ENTER) {
                /* exact addressing only: a group or broadcast never switches several slaves at once */
                if (c->rx_len == 3u && c->cmd[1] == HUBUS_UPDATE_KEY0 && c->cmd[2] == HUBUS_UPDATE_KEY1 &&
                    ((c->hdr[0] >> 3) & 7u) == HUBUS_TARGET_LEN_EXACT) {
                    c->update_req = 1;
                }
            } else
#endif
            if (op >= (HUBUS_UPDATER ? HUBUS_OP_UPDATE_LOAD : HUBUS_OP_APP_MIN) && c->on_command) {
                c->on_command(c->ctx, op, &c->cmd[1], (uint8_t)(c->rx_len - 1u));
            }
        }
        break;
    default: {
        /* A slave without pending data (REQUEST_DATA) or without a state (REQUEST_STATE) does not take part and
         * drops out at once. */
        uint8_t can_answer = 1u;

        if (c->rx_type == HUBUS_M_REQUEST_DATA) {
#if HUBUS_ENABLE_EVENTS
            can_answer = c->pending;
#else
            can_answer = 0u;
#endif
        } else if (c->rx_type == HUBUS_M_REQUEST_STATE) {
#if HUBUS_ENABLE_STATE
            can_answer = c->app_state_len;
#else
            can_answer = 0u;
#endif
        }
        if (can_answer) {
            c->slot = 0; /* turnaround phase */
            c->state = HUBUS_ST_TURN;
        } else {
            go_ignore(c);
        }
        break;
    }
    }
}

static void rx_byte(hubus_slave_core_t *c, uint8_t b)
{
    uint8_t idx = c->idx;

    if (idx < HUBUS_HEADER_BYTES) {
        /* header layout: TYPE(2) TARGET_LEN(3) MCID(6) LEN(5), see hubus_frame.h. The MCID prefix is checked as soon as
         * its bits arrive: up to three in the first byte, the rest in the second. */
        uint8_t tl;

        c->hdr[idx] = b;
        c->crc = hubus_crc8_update(c->crc, b);
        c->idx = (uint8_t)(idx + 1u);
        tl = (uint8_t)((c->hdr[0] >> 3) & 7u);
        if (idx == 0u) {
            if (tl > HUBUS_TARGET_LEN_EXACT || prefix3_differs((uint8_t)(c->mcid >> 3), b, (tl > 3u) ? 3u : tl)) {
                go_ignore(c);
            }
        } else {
            c->rx_type = (uint8_t)(c->hdr[0] >> 6);
            c->rx_len = (uint8_t)(b & 0x1Fu);
            if (tl > 3u && prefix3_differs(c->mcid, (uint8_t)(b >> 5), (uint8_t)(tl - 3u))) {
                go_ignore(c);
            }
        }
        return;
    }
    if (idx < HUBUS_HEADER_BYTES + c->rx_len) {
        c->crc = hubus_crc8_update(c->crc, b);
        if (c->rx_type == HUBUS_M_SEND_COMMAND && c->rx_len <= HUBUS_RX_BUF_SIZE) {
            c->cmd[idx - HUBUS_HEADER_BYTES] = b;
        }
        c->idx = (uint8_t)(idx + 1u);
        return;
    }
    rx_done(c, (uint8_t)(b == hubus_crc8_final(c->crc)));
}

static void rx_bit(hubus_slave_core_t *c, uint8_t bit)
{
    c->shift = (uint8_t)((c->shift << 1) | bit);
    if (++c->nbits == 8u) {
        c->nbits = 0;
        rx_byte(c, c->shift);
    }
}

/* ---- arbitration ---------------------------------------------------- */

/* MCID bit of the current slot pair (arbitration bit, handoff slot): bit 5 first. */
static uint8_t arb_bit(const hubus_slave_core_t *c)
{
    return (uint8_t)(HUBUS_MCID_BITS - 1u - (c->slot >> 1));
}

static void arb_drive(hubus_slave_core_t *c)
{
    uint8_t k;

    if (c->slot >= HUBUS_ARB_SLOTS) {
        /* After the last handoff slot the winner answers in the same transaction. */
        if (c->lost == LOST_NONE) {
            tx_start(c);
        } else {
            go_ignore(c);
        }
        return;
    }
    k = arb_bit(c);
    if (!(c->slot & 1u)) {
        c->data_low = (uint8_t)(c->lost == LOST_NONE && ((c->mcid >> k) & 1u));
    } else {
        c->data_low = (uint8_t)(c->lost == k);
    }
}

/* Rising edges alternate with falling ones, and arb_drive leaves ARB once the pattern is complete: slot < 12 here. */
static void arb_sample(hubus_slave_core_t *c, uint8_t bit)
{
    uint8_t k = arb_bit(c);

    if (!(c->slot & 1u) && c->lost == LOST_NONE && !((c->mcid >> k) & 1u) && bit) {
        c->lost = k;
    }
    c->slot++;
}

/* ---- transmit ------------------------------------------------------- */

static uint8_t tx_byte(hubus_slave_core_t *c, uint8_t idx)
{
    uint8_t b;

    if (idx < HUBUS_HEADER_BYTES) {
        b = c->hdr[idx];
    } else if (idx < HUBUS_HEADER_BYTES + c->tx_len) {
        uint8_t i = (uint8_t)(idx - HUBUS_HEADER_BYTES);
#if HUBUS_ENABLE_STATE
        if (c->rx_type == HUBUS_M_REQUEST_STATE) {
            b = c->app_state[i];
        } else
#endif
#if HUBUS_ENABLE_EVENTS
        if (c->rx_type == HUBUS_M_REQUEST_DATA) {
            b = c->buf[i];
        } else
#endif
        if (i == HUBUS_INFO_VERSION) {
            b = HUBUS_PROTOCOL_VERSION;
        } else if (i == HUBUS_INFO_CAPS) {
#if HUBUS_ENABLE_EVENTS
            b = c->on_status_capture ? (HUBUS_CAP_EVENTS | HUBUS_CAP_STATUS_CAPTURE) : HUBUS_CAP_EVENTS;
#else
            b = 0u;
#endif
#if HUBUS_ENABLE_STATE
            if (c->app_state_len) {
                b |= HUBUS_CAP_STATE;
            }
#endif
#if HUBUS_ENABLE_UPDATE
            b |= HUBUS_CAP_UPDATE;
#endif
#if HUBUS_UPDATER
            b |= HUBUS_CAP_UPDATER;
#endif
        } else if (i == HUBUS_INFO_MAX_CMD) {
            b = HUBUS_RX_BUF_SIZE;
        } else if (i == HUBUS_INFO_SPEED) {
            b = c->speed_class;
        } else {
            b = HUBUS_INFO_READ(&c->info[i - HUBUS_INFO_APP]);
        }
    } else {
        return hubus_crc8_final(c->crc);
    }
    c->crc = hubus_crc8_update(c->crc, b);
    return b;
}

static void tx_init(hubus_slave_core_t *c)
{
    uint8_t field = 0; /* SLAVE_INFO and state frames: MORE = 0, GEN = 00 */
    uint8_t type = HUBUS_S_DATA;

    /* Only requests this slave can answer get here: REQUEST_INFO always, REQUEST_STATE and REQUEST_DATA only with
     * the respective part compiled in. */
    if (c->rx_type == HUBUS_M_REQUEST_INFO) {
        type = HUBUS_S_INFO;
        c->tx_len = (uint8_t)(HUBUS_INFO_APP + c->info_len);
    }
#if HUBUS_ENABLE_STATE
    else if (c->rx_type == HUBUS_M_REQUEST_STATE) {
        c->tx_len = c->app_state_len;
    }
#endif
#if HUBUS_ENABLE_EVENTS
    else {
        c->tx_len = c->buf_len;
        field = HUBUS_SLAVE_FIELD(c->more, c->gen);
    }
#endif
    hubus_header_pack(c->hdr, type, field, c->mcid, c->tx_len);
    c->crc = HUBUS_CRC_INIT;
    c->idx = 0;
    c->nbits = 0;
}

/* Header, payload, and CRC loaded and every bit of the last byte driven. */
static uint8_t tx_finished(const hubus_slave_core_t *c)
{
    return (uint8_t)(c->nbits == 0u && c->idx >= (uint8_t)(HUBUS_HEADER_BYTES + 1u + c->tx_len));
}

static void tx_drive(hubus_slave_core_t *c)
{
    if (tx_finished(c)) {
        go_ignore(c);
        return;
    }
    if (c->nbits == 0u) {
        c->shift = tx_byte(c, c->idx++);
        c->nbits = 8u;
    }
    c->data_low = (uint8_t)(c->shift >> 7);
    c->shift = (uint8_t)(c->shift << 1);
    c->nbits--;
}

static void tx_start(hubus_slave_core_t *c)
{
    tx_init(c);
    c->state = HUBUS_ST_TX;
    tx_drive(c);
}

/* ---- clock edges ---------------------------------------------------- */

static void turn_fall(hubus_slave_core_t *c)
{
    if (c->slot == 0u) {
        c->slot = 1u; /* falling edge that opens the turnaround slot */
    } else if (c->slot == 2u) { /* falling edge after the turnaround slot */
        /* The MCID bits of the request prefix are not arbitrated: start at slot 2 * TARGET_LEN.
         * With an exact request (TARGET_LEN 6) that is the end of the pattern and the slave answers at once. */
        c->slot = (uint8_t)(((c->hdr[0] >> 3) & 7u) << 1);
        c->lost = LOST_NONE;
        c->state = HUBUS_ST_ARB;
        arb_drive(c);
    }
}

static void on_fall(hubus_slave_core_t *c)
{
    switch (c->state) {
#if HUBUS_ENABLE_EVENTS
    case HUBUS_ST_IDLE:
        if (c->pending && c->armed) { /* the bus became idle: announce unread data once */
            start_event(c);
        }
        break;
    case HUBUS_ST_EVENT:
        /* one complete clock pulse seen: releasing now cannot look like STOP */
        c->data_low = 0;
        c->state = HUBUS_ST_IDLE;
        break;
#endif
    case HUBUS_ST_TURN:
        turn_fall(c);
        break;
    case HUBUS_ST_ARB:
        arb_drive(c);
        break;
    case HUBUS_ST_TX:
        tx_drive(c);
        break;
    default:
        break;
    }
}

static void on_rise(hubus_slave_core_t *c, uint8_t bit)
{
    switch (c->state) {
    case HUBUS_ST_RX:
        rx_bit(c, bit);
        break;
    case HUBUS_ST_TURN:
        if (c->slot == 1u) {
            c->slot = 2u;
        }
        break;
    case HUBUS_ST_ARB:
        arb_sample(c, bit);
        break;
#if HUBUS_ENABLE_EVENTS
    case HUBUS_ST_TX:
        if (tx_finished(c) && c->rx_type == HUBUS_M_REQUEST_DATA) {
            c->tx_complete = 1; /* last bit has been sampled by the master */
        }
        break;
#endif
    default:
        break;
    }
}

/* ---- public API ----------------------------------------------------- */

uint8_t hubus_core_init(hubus_slave_core_t *c, const hubus_core_config_t *cfg, uint8_t clk, uint8_t data)
{
    memset(c, 0, sizeof *c); /* state IDLE, MCID 0: disabled until the configuration is accepted */
    c->clk = clk ? 1u : 0u;
    c->data = data ? 1u : 0u;
    if (cfg->mcid < HUBUS_MCID_MIN || cfg->mcid > HUBUS_MCID_MAX ||
        cfg->info_len > HUBUS_INFO_APP_MAX || (cfg->info_len > 0u && cfg->info == NULL) ||
        cfg->state_len > HUBUS_MAX_PAYLOAD || cfg->speed_class > HUBUS_SPEED_CLASS_MAX) {
        return 0;
    }
    c->speed_class = cfg->speed_class;
    c->info = cfg->info;
    c->info_len = cfg->info_len;
#if HUBUS_ENABLE_STATE
    c->app_state = cfg->state;
    c->app_state_len = cfg->state ? cfg->state_len : 0u;
#endif
#if HUBUS_ENABLE_EVENTS
    c->on_status_capture = cfg->on_status_capture;
#endif
    c->on_command = cfg->on_command;
    c->ctx = cfg->ctx;
    c->mcid = cfg->mcid;
    return 1;
}

void hubus_core_pins(hubus_slave_core_t *c, uint8_t clk, uint8_t data)
{
    if (!c->mcid) {
        return;
    }
    clk = clk ? 1u : 0u;
    data = data ? 1u : 0u;
    if (clk != c->clk) {
        /* CLK edge: DATA is sampled (rising) or ignored (falling). A DATA
         * change that coincides with the edge is part of the same event. */
        c->clk = clk;
        c->data = data;
        if (clk) {
            on_rise(c, (uint8_t)!data);
        } else {
            on_fall(c);
        }
    } else if (data != c->data) {
        c->data = data;
        if (clk) { /* DATA change while CLK is high: line condition */
            if (data) {
                on_stop(c);
            } else {
                on_start(c);
            }
        } /* DATA change while CLK is low (including own writes): ignored */
    }
}

#if HUBUS_ENABLE_EVENTS
uint8_t hubus_core_write(hubus_slave_core_t *c, const uint8_t *data, uint8_t len, uint8_t more)
{
    uint8_t i;
    uint8_t was_pending;

    if (!c->mcid) {
        return HUBUS_WRITE_DISABLED;
    }
    if (len > HUBUS_TX_BUF_SIZE) {
        return HUBUS_WRITE_TOO_LONG;
    }
    if (c->state == HUBUS_ST_TX && c->rx_type == HUBUS_M_REQUEST_DATA) {
        return HUBUS_WRITE_BUSY; /* the frame must not mix old and new content */
    }
    for (i = 0; i < len; i++) {
        c->buf[i] = data[i];
    }
    c->buf_len = len;
    c->more = more ? 1u : 0u;
    c->gen = (uint8_t)((c->gen + 1u) & 3u);
    c->tx_complete = 0; /* a complete response must not clear the new content */
    was_pending = c->pending;
    c->pending = 1;
    if (!was_pending && c->state == HUBUS_ST_IDLE && !c->clk) {
        start_event(c);
    }
    return HUBUS_WRITE_OK;
}
#endif
