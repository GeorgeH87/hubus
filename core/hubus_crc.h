/*
 * CRC-8/AUTOSAR: poly 0x2F, init 0xFF, xorout 0xFF, not reflected.
 * Check value for "123456789" is 0xDF (docs/spec/04-frames.md section 4.12).
 *
 * The register is kept without the final XOR so frames can be processed
 * byte by byte while they are received or transmitted:
 *
 *   crc = HUBUS_CRC_INIT;
 *   crc = hubus_crc8_update(crc, byte);  ... for every covered byte
 *   crc = hubus_crc8_final(crc);
 */
#ifndef HUBUS_CRC_H
#define HUBUS_CRC_H

#include <stddef.h>
#include <stdint.h>

#define HUBUS_CRC_INIT 0xFFu
#define HUBUS_CRC_POLY 0x2Fu
#define HUBUS_CRC_XOROUT 0xFFu
#define HUBUS_CRC_CHECK 0xDFu

uint8_t hubus_crc8_update(uint8_t crc, uint8_t byte);

static inline uint8_t hubus_crc8_final(uint8_t crc)
{
    return (uint8_t)(crc ^ HUBUS_CRC_XOROUT);
}

/* One-shot CRC over a buffer. */
uint8_t hubus_crc8(const uint8_t *data, size_t len);

/*
 * CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no xorout, not reflected; check value 0x29B1. Protects firmware
 * pages and images during an update (docs/spec/06-operation.md section 6.8), which are longer than what the CRC8
 * of a frame covers well.
 */
#define HUBUS_CRC16_INIT 0xFFFFu
#define HUBUS_CRC16_CHECK 0x29B1u

uint16_t hubus_crc16_update(uint16_t crc, uint8_t byte);
uint16_t hubus_crc16(const uint8_t *data, size_t len);

#endif /* HUBUS_CRC_H */
