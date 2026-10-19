/**
 * HuBus Slave Library - Frame Implementation
 */

#include "hubus_frame.h"
#include "hubus_crc.h"

uint8_t hubus_frame_is_addressed(uint8_t slave_mcid, uint8_t target_mcid, uint8_t target_len)
{
    // No bits to match (target_len = 0) means broadcast to all
    if (target_len == 0) {
        return 1;
    }
    
    // target_len = 6 means exact match on full 6-bit MCID
    if (target_len >= 6) {
        return (slave_mcid == target_mcid) ? 1 : 0;
    }
    
    // For target_len < 6, match the top target_len bits
    // Extract top target_len bits from both slave and target
    uint8_t shift = 6 - target_len;
    uint8_t slave_prefix = slave_mcid >> shift;
    uint8_t target_prefix = target_mcid >> shift;
    
    return (slave_prefix == target_prefix) ? 1 : 0;
}

uint8_t hubus_frame_build_tx(hubus_tx_frame_t *frame,
                             uint8_t type, uint8_t mcid,
                             const uint8_t *payload, uint8_t len)
{
    if (type > 0x7 || mcid > 0x3F || len > HUBUS_MAX_PAYLOAD) {
        return 0;  // Invalid parameters
    }
    
    frame->type = type & 0x7;
    frame->mcid = mcid & 0x3F;
    frame->len = len & 0x1F;
    
    // Copy payload
    if (len > 0 && payload != NULL) {
        for (uint8_t i = 0; i < len; i++) {
            frame->payload[i] = payload[i];
        }
    }
    
    // Calculate CRC8 over TYPE, MCID, LEN, PAYLOAD (not CRC itself)
    // Build a temporary buffer for CRC calculation
    uint8_t temp_crc_buf[2 + HUBUS_MAX_PAYLOAD];
    uint8_t crc_idx = 0;
    
    // TYPE (3 bits, shifted to upper bits) + MCID (6 bits lower)
    // For CRC, we treat this as a byte: TYPE in bits 7:5, MCID in bits 5:0
    // Actually: SOF not included in CRC per MESSAGE_FORMAT.md
    // Frame structure for CRC: TYPE -> TARGET_LEN -> MCID -> LEN -> PAYLOAD
    // But in slave response: TYPE -> MCID -> LEN -> PAYLOAD
    
    // Pack: TYPE (3 bits) + MCID (6 bits) into bytes
    temp_crc_buf[crc_idx++] = (frame->type << 5) | frame->mcid;
    temp_crc_buf[crc_idx++] = frame->len;
    
    for (uint8_t i = 0; i < frame->len; i++) {
        temp_crc_buf[crc_idx++] = frame->payload[i];
    }
    
    frame->crc = hubus_crc8(temp_crc_buf, crc_idx);
    
    return 1;
}

uint8_t hubus_frame_tx_size(const hubus_tx_frame_t *frame)
{
    // SOF(4) + TYPE(3) + MCID(6) + LEN(5) + PAYLOAD(len*8) + CRC8(8)
    // In bytes: ceil((4+3+6+5+len*8+8)/8) = 4 + len
    return 4 + frame->len;
}
