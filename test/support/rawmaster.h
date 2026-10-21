/*
 * Bit-level master for protocol tests of the slave core. Unlike the real
 * master it has no recovery logic and does not look at readbacks, so tests
 * can produce exactly the wire traffic they want, including malformed frames.
 */
#ifndef HUBUS_RAWMASTER_H
#define HUBUS_RAWMASTER_H

#include <stddef.h>
#include <stdint.h>

#include "hubus_crc.h"
#include "hubus_frame.h"
#include "hubus_sim.h"

typedef struct {
    hubus_sim_t *sim;
    hubus_master_hal_t hal;
} raw_t;

static inline void raw_init(raw_t *r, hubus_sim_t *sim)
{
    r->sim = sim;
    r->hal = hubus_sim_master_hal(sim);
}

static inline void raw_clk(raw_t *r, uint8_t v) { r->hal.set_clk(r->hal.ctx, v); }
static inline void raw_pull(raw_t *r, uint8_t low) { r->hal.set_data_low(r->hal.ctx, low); }
static inline uint8_t raw_line(raw_t *r) { return r->hal.read_data(r->hal.ctx); }

/* Idle event handshake: clock until a signaling slave has released DATA (bounded). */
static inline void raw_idle_handshake(raw_t *r)
{
    unsigned guard = 0;

    while (!r->sim->clk && !raw_line(r) && guard++ < 4) {
        raw_clk(r, 1);
        raw_clk(r, 0);
    }
}

/* START: with DATA released, raise CLK, pull DATA low, lower CLK. */
static inline void raw_start(raw_t *r)
{
    raw_pull(r, 0);
    raw_idle_handshake(r);
    if (!r->sim->clk) {
        raw_clk(r, 1);
    }
    raw_pull(r, 1);
    raw_clk(r, 0);
}

/* STOP from CLK low: pull DATA low, raise CLK, release DATA. CLK is lowered afterwards unless `hold`. */
static inline void raw_stop(raw_t *r, int hold)
{
    raw_pull(r, 1);
    raw_clk(r, 1);
    raw_pull(r, 0);
    if (!hold) {
        raw_clk(r, 0);
    }
}

/* RESET: START immediately followed by STOP without a clock pulse. */
static inline void raw_reset(raw_t *r)
{
    raw_pull(r, 0);
    raw_idle_handshake(r);
    if (!r->sim->clk) {
        raw_clk(r, 1);
    }
    raw_pull(r, 1);
    raw_pull(r, 0);
    raw_clk(r, 0);
}

static inline void raw_send_bit(raw_t *r, int bit)
{
    raw_pull(r, bit ? 1 : 0);
    raw_clk(r, 1);
    raw_clk(r, 0);
}

/* Returns 1 if DATA was low at the rising edge. */
static inline int raw_recv_bit(raw_t *r)
{
    int bit;

    raw_clk(r, 1);
    bit = raw_line(r) ? 0 : 1;
    raw_clk(r, 0);
    return bit;
}

static inline void raw_tick(raw_t *r)
{
    raw_clk(r, 1);
    raw_clk(r, 0);
}

static inline void raw_send_byte(raw_t *r, uint8_t b)
{
    int i;

    for (i = 7; i >= 0; i--) {
        raw_send_bit(r, (b >> i) & 1);
    }
}

/* Sends header + payload + CRC (the CRC byte can be corrupted with `crc_xor`). */
static inline void raw_send_frame_x(raw_t *r, uint8_t type, uint8_t field, uint8_t mcid, const uint8_t *payload,
                                    uint8_t len, uint8_t crc_xor)
{
    uint8_t buf[HUBUS_MAX_FRAME_BYTES];
    size_t n = hubus_frame_encode(buf, type, field, mcid, payload, len);
    size_t i;

    buf[n - 1] ^= crc_xor;
    for (i = 0; i < n; i++) {
        raw_send_byte(r, buf[i]);
    }
}

static inline void raw_send_frame(raw_t *r, uint8_t type, uint8_t field, uint8_t mcid, const uint8_t *payload,
                                  uint8_t len)
{
    raw_send_frame_x(r, type, field, mcid, payload, len, 0);
}

/* Turnaround slot: everybody releases DATA. */
static inline void raw_turnaround(raw_t *r)
{
    raw_pull(r, 0);
    raw_tick(r);
}

/*
 * Receives a slave frame after the turnaround slot. Returns the number of
 * bytes (header + payload + CRC) or 0 if the first bit was released (no response).
 */
static inline size_t raw_recv_frame(raw_t *r, uint8_t *buf)
{
    size_t total_bits = 24;
    size_t nbits = 1;
    uint8_t cur = 1;

    if (!raw_recv_bit(r)) {
        return 0;
    }
    while (nbits < total_bits) {
        cur = (uint8_t)((cur << 1) | raw_recv_bit(r));
        nbits++;
        if ((nbits & 7u) == 0u) {
            buf[nbits / 8 - 1] = cur;
            cur = 0;
            if (nbits == 16) {
                total_bits = (size_t)(3 + (buf[1] & 0x1F)) * 8;
            }
        }
    }
    return total_bits / 8;
}

/* Receives the 12 arbitration slots; slot 0 ends up in bit 0 (1 = DATA low). */
static inline uint16_t raw_recv_arb(raw_t *r)
{
    uint16_t slots = 0;
    unsigned i;

    for (i = 0; i < HUBUS_ARB_SLOTS; i++) {
        if (raw_recv_bit(r)) {
            slots |= (uint16_t)(1u << i);
        }
    }
    return slots;
}

/* One complete REQUEST_xxx transaction up to (excluding) the closing condition. */
static inline size_t raw_request(raw_t *r, uint8_t type, uint8_t mcid, uint8_t *buf)
{
    raw_start(r);
    raw_send_frame(r, type, HUBUS_TARGET_LEN_EXACT, mcid, NULL, 0);
    raw_turnaround(r);
    return raw_recv_frame(r, buf);
}

#endif /* HUBUS_RAWMASTER_H */
