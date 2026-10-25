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

/* Byte-wise without a table or a bit loop: the updater checks the whole application at every boot. */
uint16_t hubus_crc16_update(uint16_t crc, uint8_t byte)
{
    uint8_t x = (uint8_t)((crc >> 8) ^ byte);

    x ^= (uint8_t)(x >> 4);
    return (uint16_t)((uint16_t)(crc << 8) ^ (uint16_t)((uint16_t)x << 12) ^ (uint16_t)((uint16_t)x << 5) ^ x);
}

uint16_t hubus_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = HUBUS_CRC16_INIT;

    while (len--) {
        crc = hubus_crc16_update(crc, *data++);
    }
    return crc;
}
