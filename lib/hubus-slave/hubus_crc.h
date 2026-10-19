/**
 * HuBus Slave Library - CRC8 Module
 * 
 * Calculates 8-bit CRC for frame integrity checking.
 * Parameters configurable via hubus_slave_config.h
 */

#ifndef HUBUS_CRC_H
#define HUBUS_CRC_H

#include <stdint.h>
#include <stddef.h>

/**
 * Calculate CRC8 over a data buffer
 * 
 * @param data  pointer to data bytes
 * @param len   number of bytes
 * @return      computed CRC8 value
 * 
 * Uses polynomial, init, and xorout values from hubus_slave_config.h
 */
uint8_t hubus_crc8(const uint8_t *data, uint8_t len);

/**
 * Validate CRC8 by appending the CRC to data and verifying result is 0
 * (assumes last byte is the CRC to check against)
 * 
 * @param data  pointer to data buffer (last byte is CRC)
 * @param len   number of bytes including CRC
 * @return      1 if CRC is valid, 0 if invalid
 */
uint8_t hubus_crc8_validate(const uint8_t *data, uint8_t len);

#endif // HUBUS_CRC_H
