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

/* ---- line conditions ------------------------------------------------ */

static void on_start(hubus_slave_core_t *c)
{
    c->data_low = 0;
    c->tx_complete = 0;
    c->armed = 0;
    c->bitcnt = 0;
    c->bytecnt = 0;
    c->shreg = 0;
    c->crc = HUBUS_CRC_INIT;
    c->state = HUBUS_ST_RX;
}

static void on_stop(hubus_slave_core_t *c)
{
    /* START immediately followed by STOP is a RESET: tx_complete is already
     * cleared by the START, so buffer, GEN, and pending flag stay intact. */
    c->armed = 1;
    c->data_low = 0;
    if (c->tx_complete && c->tx_type == HUBUS_S_DATA) {
        c->pending = 0;
        c->buf_len = 0;
        c->more = 0;
    }
    c->tx_complete = 0;
    c->state = HUBUS_ST_IDLE;
}

/* ---- receive -------------------------------------------------------- */

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
            if (op == HUBUS_OP_STATUS_CAPTURE) {
                if (c->on_status_capture) {
                    c->on_status_capture(c->ctx);
                }
            } else if (op >= HUBUS_OP_APP_MIN && c->on_command) {
                c->on_command(c->ctx, op, &c->cmd[1], (uint8_t)(c->rx_len - 1u));
            }
        }
        break;
    case HUBUS_M_POLL_PENDING:
        /* LEN 0: pending poll; LEN 1: discovery poll, every addressed slave contends; other LEN: reserved */
        if (c->rx_len > HUBUS_POLL_DISCOVER || (c->rx_len == 0u && !c->pending)) {
            go_ignore(c);
            break;
        }
        c->action = c->rx_type;
        c->phase = 0;
        c->state = HUBUS_ST_TURN;
        break;
    case HUBUS_M_REQUEST_DATA:
        if (!c->pending) {
            go_ignore(c);
            break;
        }
        /* fall through */
    default: /* HUBUS_M_REQUEST_INFO */
        c->action = c->rx_type;
        c->phase = 0;
        c->state = HUBUS_ST_TURN;
        break;
    }
}

static void rx_header(hubus_slave_core_t *c)
{
    /* header layout: TYPE(2) TARGET_LEN(3) MCID(6) LEN(5), see hubus_frame.h */
    uint8_t type = (uint8_t)(c->hdr[0] >> 6);
    uint8_t tlen = (uint8_t)((c->hdr[0] >> 3) & 7u);
    uint8_t mcid = (uint8_t)(((c->hdr[0] & 7u) << 3) | (c->hdr[1] >> 5));

    c->rx_type = type;
    c->rx_len = (uint8_t)(c->hdr[1] & 0x1Fu);
    if (!hubus_mcid_match(c->mcid, mcid, tlen) ||
        ((type == HUBUS_M_REQUEST_INFO || type == HUBUS_M_REQUEST_DATA) && tlen != HUBUS_TARGET_LEN_EXACT)) {
        go_ignore(c);
    }
}

static void rx_byte(hubus_slave_core_t *c, uint8_t b)
{
    uint8_t idx = c->bytecnt;

    if (idx < HUBUS_HEADER_BYTES) {
        c->hdr[idx] = b;
        c->crc = hubus_crc8_update(c->crc, b);
        c->bytecnt = (uint8_t)(idx + 1u);
        if (idx == 0u) {
            /* Early reject: TARGET_LEN and the first MCID bits are known. */
            uint8_t tl = (uint8_t)((b >> 3) & 7u);
            if (tl > HUBUS_TARGET_LEN_EXACT) {
                go_ignore(c);
            } else if (tl > 0u) {
                uint8_t n = (tl > 3u) ? 3u : tl;
                uint8_t mask = (uint8_t)((7u << (3u - n)) & 7u);
                if ((((c->mcid >> 3) ^ b) & mask) != 0u) {
                    go_ignore(c);
                }
            }
        } else {
            rx_header(c);
        }
        return;
    }
    if (idx < HUBUS_HEADER_BYTES + c->rx_len) {
        c->crc = hubus_crc8_update(c->crc, b);
        if (c->rx_type == HUBUS_M_SEND_COMMAND && c->rx_len <= HUBUS_RX_BUF_SIZE) {
            c->cmd[idx - HUBUS_HEADER_BYTES] = b;
        }
        c->bytecnt = (uint8_t)(idx + 1u);
        return;
    }
    rx_done(c, (uint8_t)(b == hubus_crc8_final(c->crc)));
}

static void rx_bit(hubus_slave_core_t *c, uint8_t bit)
{
    c->shreg = (uint8_t)((c->shreg << 1) | bit);
    if (++c->bitcnt == 8u) {
        c->bitcnt = 0;
        rx_byte(c, c->shreg);
    }
}

/* ---- arbitration ---------------------------------------------------- */

static void arb_drive(hubus_slave_core_t *c)
{
    uint8_t k;

    if (c->slot >= HUBUS_ARB_SLOTS) {
        go_ignore(c);
        return;
    }
    k = (uint8_t)(HUBUS_MCID_BITS - 1u - (c->slot >> 1));
    if (!(c->slot & 1u)) {
        c->data_low = (uint8_t)(c->arb_active && ((c->mcid >> k) & 1u));
    } else {
        c->data_low = (uint8_t)(c->lost == k);
    }
}

static void arb_sample(hubus_slave_core_t *c, uint8_t bit)
{
    uint8_t k;

    if (c->slot >= HUBUS_ARB_SLOTS) {
        return;
    }
    k = (uint8_t)(HUBUS_MCID_BITS - 1u - (c->slot >> 1));
    if (!(c->slot & 1u) && c->arb_active && !((c->mcid >> k) & 1u) && bit) {
        c->arb_active = 0;
        c->lost = k;
    }
    c->slot++;
}

/* ---- transmit ------------------------------------------------------- */

static uint8_t tx_byte(hubus_slave_core_t *c, uint8_t idx)
{
    uint8_t b;

    if (idx < HUBUS_HEADER_BYTES) {
        b = c->tx_hdr[idx];
    } else if (idx < HUBUS_HEADER_BYTES + c->tx_len) {
        uint8_t i = (uint8_t)(idx - HUBUS_HEADER_BYTES);
        if (c->tx_type == HUBUS_S_DATA) {
            b = c->buf[i];
        } else if (i == HUBUS_INFO_VERSION) {
            b = HUBUS_PROTOCOL_VERSION;
        } else if (i == HUBUS_INFO_CAPS) {
            b = c->on_status_capture ? HUBUS_CAP_STATUS_CAPTURE : 0u;
        } else if (i == HUBUS_INFO_MAX_CMD) {
            b = HUBUS_RX_BUF_SIZE;
        } else {
            b = HUBUS_INFO_READ(&c->info[i - HUBUS_INFO_APP]);
        }
    } else {
        return hubus_crc8_final(c->tx_crc);
    }
    c->tx_crc = hubus_crc8_update(c->tx_crc, b);
    return b;
}

static void tx_init(hubus_slave_core_t *c)
{
    uint8_t field;

    if (c->action == HUBUS_M_REQUEST_INFO) {
        c->tx_type = HUBUS_S_INFO;
        c->tx_len = (uint8_t)(HUBUS_INFO_APP + c->info_len);
        field = 0;
    } else {
        c->tx_type = HUBUS_S_DATA;
        c->tx_len = c->buf_len;
        field = HUBUS_SLAVE_FIELD(c->more, c->gen);
    }
    hubus_header_pack(c->tx_hdr, c->tx_type, field, c->mcid, c->tx_len);
    c->tx_crc = HUBUS_CRC_INIT;
    c->tx_idx = 0;
    c->tx_nbits = 0;
    c->tx_total = (uint8_t)(HUBUS_HEADER_BYTES + 1u + c->tx_len);
}

static void tx_drive(hubus_slave_core_t *c)
{
    if (c->tx_nbits == 0u) {
        if (c->tx_idx >= c->tx_total) {
            go_ignore(c);
            return;
        }
        c->tx_shift = tx_byte(c, c->tx_idx++);
        c->tx_nbits = 8u;
    }
    c->data_low = (uint8_t)(c->tx_shift >> 7);
    c->tx_shift = (uint8_t)(c->tx_shift << 1);
    c->tx_nbits--;
}

/* ---- clock edges ---------------------------------------------------- */

static void turn_fall(hubus_slave_core_t *c)
{
    if (c->phase == 0u) {
        c->phase = 1u; /* falling edge that opens the turnaround slot */
    } else if (c->phase == 2u) { /* falling edge after the turnaround slot */
        if (c->action == HUBUS_M_POLL_PENDING) {
            c->slot = 0;
            c->arb_active = 1;
            c->lost = LOST_NONE;
            c->state = HUBUS_ST_ARB;
            arb_drive(c);
        } else {
            tx_init(c);
            c->state = HUBUS_ST_TX;
            tx_drive(c);
        }
    }
}

static void on_fall(hubus_slave_core_t *c)
{
    switch (c->state) {
    case HUBUS_ST_IDLE:
        if (c->pending && c->armed) { /* the bus became idle: announce unread data once */
            c->armed = 0;
            c->data_low = 1;
            c->state = HUBUS_ST_EVENT;
        }
        break;
    case HUBUS_ST_EVENT:
        /* one complete clock pulse seen: releasing now cannot look like STOP */
        c->data_low = 0;
        c->state = HUBUS_ST_IDLE;
        break;
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
        if (c->phase == 1u) {
            c->phase = 2u;
        }
        break;
    case HUBUS_ST_ARB:
        arb_sample(c, bit);
        break;
    case HUBUS_ST_TX:
        if (c->tx_idx >= c->tx_total && c->tx_nbits == 0u && c->tx_type == HUBUS_S_DATA) {
            c->tx_complete = 1; /* last bit has been sampled by the master */
        }
        break;
    default:
        break;
    }
}

/* ---- public API ----------------------------------------------------- */

uint8_t hubus_core_init(hubus_slave_core_t *c, const hubus_core_config_t *cfg, uint8_t clk, uint8_t data)
{
    memset(c, 0, sizeof *c);
    c->clk = clk ? 1u : 0u;
    c->data = data ? 1u : 0u;
    c->lost = LOST_NONE;
    c->state = HUBUS_ST_IDLE;
    if (cfg->mcid < HUBUS_MCID_MIN || cfg->mcid > HUBUS_MCID_MAX ||
        cfg->info_len > HUBUS_INFO_APP_MAX || (cfg->info_len > 0u && cfg->info == NULL)) {
        return 0;
    }
    c->mcid = cfg->mcid;
    c->info = cfg->info;
    c->info_len = cfg->info_len;
    c->on_status_capture = cfg->on_status_capture;
    c->on_command = cfg->on_command;
    c->ctx = cfg->ctx;
    c->enabled = 1;
    return 1;
}

void hubus_core_pins(hubus_slave_core_t *c, uint8_t clk, uint8_t data)
{
    if (!c->enabled) {
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

uint8_t hubus_core_write(hubus_slave_core_t *c, const uint8_t *data, uint8_t len, uint8_t more)
{
    uint8_t i;
    uint8_t was_pending;

    if (!c->enabled) {
        return HUBUS_WRITE_DISABLED;
    }
    if (len > HUBUS_TX_BUF_SIZE) {
        return HUBUS_WRITE_TOO_LONG;
    }
    if (c->state == HUBUS_ST_TX && c->tx_type == HUBUS_S_DATA) {
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
        c->armed = 0;
        c->data_low = 1;
        c->state = HUBUS_ST_EVENT;
    }
    return HUBUS_WRITE_OK;
}
