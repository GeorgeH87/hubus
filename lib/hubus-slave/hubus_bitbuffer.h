/**
 * HuBus Slave Library - Bit Buffer for RX Frame Parsing
 * 
 * Accumulates bits from CLK-synchronized DATA line reads.
 * Assembles bits into bytes and validates frames.
 */

#ifndef HUBUS_BITBUFFER_H
#define HUBUS_BITBUFFER_H

#include <stdint.h>
#include "hubus_slave_config.h"

/* ====================================================================
   Bit Buffer Structure
   ==================================================================== */

typedef struct {
    uint8_t bits[HUBUS_MAX_PAYLOAD + 4];  // Buffer for entire frame (SOF+TYPE+TARGET_LEN+MCID+LEN+PAYLOAD+CRC8)
    uint16_t bit_count;                    // Total bits received so far
    uint8_t complete;                      // 1 if frame is complete and valid
} hubus_bitbuffer_t;

/* ====================================================================
   Bit Buffer Operations
   ==================================================================== */

/**
 * Initialize bit buffer for new frame reception
 * 
 * @param buf  pointer to bitbuffer
 */
void hubus_bitbuffer_init(hubus_bitbuffer_t *buf);

/**
 * Add a single bit to the buffer (LSB to MSB during reception)
 * 
 * @param buf  pointer to bitbuffer
 * @param bit  bit value (0 or 1)
 * @return     1 if buffer is now full (frame complete), 0 otherwise
 */
uint8_t hubus_bitbuffer_add_bit(hubus_bitbuffer_t *buf, uint8_t bit);

/**
 * Get byte at a specific byte offset from accumulated bits
 * Converts bits to big-endian byte format (MSB first)
 * 
 * @param buf   pointer to bitbuffer
 * @param byte_offset  which byte (0 = first byte of frame)
 * @return      the assembled byte
 */
uint8_t hubus_bitbuffer_get_byte(const hubus_bitbuffer_t *buf, uint8_t byte_offset);

/**
 * Get total number of complete bytes in buffer
 * 
 * @param buf  pointer to bitbuffer
 * @return     number of complete bytes
 */
uint8_t hubus_bitbuffer_byte_count(const hubus_bitbuffer_t *buf);

/**
 * Get total number of bits received so far
 * 
 * @param buf  pointer to bitbuffer
 * @return     bit count
 */
uint16_t hubus_bitbuffer_bit_count(const hubus_bitbuffer_t *buf);

/**
 * Check if buffer is complete (frame received)
 * 
 * @param buf  pointer to bitbuffer
 * @return     1 if complete, 0 if still receiving
 */
uint8_t hubus_bitbuffer_is_complete(const hubus_bitbuffer_t *buf);

/**
 * Reset buffer to empty state
 * 
 * @param buf  pointer to bitbuffer
 */
void hubus_bitbuffer_reset(hubus_bitbuffer_t *buf);

#endif // HUBUS_BITBUFFER_H
