#include "hubus_crc.h"

/* Bitwise implementation: a lookup table would cost 256 bytes of flash, and
 * a byte is processed only once per eight clock periods. */
uint8_t hubus_crc8_update(uint8_t crc, uint8_t byte)
{
    uint8_t i;

    crc ^= byte;
    for (i = 0; i < 8u; i++) {
        if (crc & 0x80u) {
            crc = (uint8_t)((crc << 1) ^ HUBUS_CRC_POLY);
        } else {
            crc = (uint8_t)(crc << 1);
        }
    }
    return crc;
}

uint8_t hubus_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = HUBUS_CRC_INIT;

    while (len--) {
        crc = hubus_crc8_update(crc, *data++);
    }
    return hubus_crc8_final(crc);
}
