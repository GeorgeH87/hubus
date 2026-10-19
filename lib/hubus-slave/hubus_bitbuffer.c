/**
 * HuBus Slave Library - Bit Buffer Implementation
 */

#include "hubus_bitbuffer.h"

void hubus_bitbuffer_init(hubus_bitbuffer_t *buf)
{
    buf->bit_count = 0;
    buf->complete = 0;
}

uint8_t hubus_bitbuffer_add_bit(hubus_bitbuffer_t *buf, uint8_t bit)
{
    // Don't overflow the buffer
    // Maximum frame size: 4 (SOF+TYPE+TARGET_LEN+MCID) + 5 (LEN) + 31*8 (PAYLOAD) + 8 (CRC8)
    // = 4 + 5 + 248 + 8 = 265 bits max
    if (buf->bit_count >= (sizeof(buf->bits) * 8)) {
        return 0;  // Buffer overflow
    }
    
    uint8_t byte_offset = buf->bit_count / 8;
    uint8_t bit_offset = buf->bit_count % 8;
    
    // Store bit: MSB first within each byte
    if (bit) {
        buf->bits[byte_offset] |= (1 << (7 - bit_offset));
    } else {
        buf->bits[byte_offset] &= ~(1 << (7 - bit_offset));
    }
    
    buf->bit_count++;
    
    // Frame is complete when we have at least the header
    // Header: SOF(4) + TYPE(3) + TARGET_LEN(3) + MCID(6) + LEN(5) = 21 bits
    // Then: LEN tells us payload size
    // Then: CRC8(8)
    // Minimum frame: 21 + 0 (no payload) + 8 = 29 bits
    
    // Simplified: check if we have enough bits to extract LEN, then check payload
    if (buf->bit_count >= 21) {
        uint8_t len_byte = hubus_bitbuffer_get_byte(buf, 2);  // LEN is in byte 2
        uint8_t len_value = (len_byte >> 3) & 0x1F;           // LEN is 5 bits in byte 2
        uint16_t expected_bits = 21 + (len_value * 8) + 8;    // SOF+TYPE+TARGET_LEN+MCID+LEN + PAYLOAD + CRC8
        
        if (buf->bit_count >= expected_bits) {
            buf->complete = 1;
            return 1;
        }
    }
    
    return 0;  // Frame not yet complete
}

uint8_t hubus_bitbuffer_get_byte(const hubus_bitbuffer_t *buf, uint8_t byte_offset)
{
    uint8_t result = 0;
    uint16_t start_bit = byte_offset * 8;
    
    for (uint8_t i = 0; i < 8; i++) {
        if ((start_bit + i) < buf->bit_count) {
            uint16_t bit_pos = start_bit + i;
            uint8_t byte_idx = bit_pos / 8;
            uint8_t bit_idx = bit_pos % 8;
            
            uint8_t bit_val = (buf->bits[byte_idx] >> (7 - bit_idx)) & 1;
            result |= (bit_val << (7 - i));
        }
    }
    
    return result;
}

uint8_t hubus_bitbuffer_byte_count(const hubus_bitbuffer_t *buf)
{
    return (buf->bit_count + 7) / 8;  // Round up
}

uint16_t hubus_bitbuffer_bit_count(const hubus_bitbuffer_t *buf)
{
    return buf->bit_count;
}

uint8_t hubus_bitbuffer_is_complete(const hubus_bitbuffer_t *buf)
{
    return buf->complete;
}

void hubus_bitbuffer_reset(hubus_bitbuffer_t *buf)
{
    buf->bit_count = 0;
    buf->complete = 0;
    for (uint8_t i = 0; i < sizeof(buf->bits); i++) {
        buf->bits[i] = 0;
    }
}
