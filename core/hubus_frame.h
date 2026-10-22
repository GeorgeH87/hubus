/*
 * Frame codec: 16-bit header, payload, CRC8 (docs/spec/04-frames.md).
 *
 *   Bit:     15 14 | 13 12 11   | 10  9  8  7  6  5 |  4  3  2  1  0
 *   Master:  TYPE  | TARGET_LEN |       MCID        |      LEN
 *   Slave:   TYPE  |MORE| GEN  |       MCID        |      LEN
 */
#ifndef HUBUS_FRAME_H
#define HUBUS_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "hubus_proto.h"

/* Decoded header. `field` is TARGET_LEN (master) or MORE<<2 | GEN (slave). */
typedef struct {
    uint8_t type;
    uint8_t field;
    uint8_t mcid;
    uint8_t len;
} hubus_header_t;

void hubus_header_pack(uint8_t out[2], uint8_t type, uint8_t field, uint8_t mcid, uint8_t len);
void hubus_header_unpack(hubus_header_t *h, const uint8_t in[2]);

#define HUBUS_HDR_TARGET_LEN(h) ((h)->field)
#define HUBUS_HDR_MORE(h) ((uint8_t)(((h)->field >> 2) & 1u))
#define HUBUS_HDR_GEN(h) ((uint8_t)((h)->field & 3u))
#define HUBUS_SLAVE_FIELD(more, gen) ((uint8_t)((((more) ? 1u : 0u) << 2) | ((gen) & 3u)))

/*
 * Prefix match: true if the first `target_len` bits of `own_mcid` equal the
 * first `target_len` bits of `prefix`. `target_len` above 6 never matches.
 * MCID 0 (reserved) matches nothing, not even a broadcast.
 */
uint8_t hubus_mcid_match(uint8_t own_mcid, uint8_t prefix, uint8_t target_len);

/* Mask for a prefix of `target_len` bits (MSB-aligned within 6 bits). */
static inline uint8_t hubus_prefix_mask(uint8_t target_len)
{
    return (uint8_t)((0x3Fu << (HUBUS_MCID_BITS - target_len)) & 0x3Fu);
}

/*
 * Encodes header + payload + CRC8 into `out` (at least 3 + len bytes).
 * Returns the total frame length in bytes, or 0 if `len` exceeds 31.
 */
size_t hubus_frame_encode(uint8_t *out, uint8_t type, uint8_t field, uint8_t mcid, const uint8_t *payload, uint8_t len);

/*
 * Checks a complete frame in `buf` (length `n`): length consistent with LEN
 * and CRC8 valid. On success fills `h` (if not NULL) and returns 1.
 */
uint8_t hubus_frame_decode(const uint8_t *buf, size_t n, hubus_header_t *h);

#endif /* HUBUS_FRAME_H */
