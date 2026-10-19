/**
 * HuBus Slave Library - Frame Structure and Parsing
 * 
 * Defines frame layout per MESSAGE_FORMAT.md v0:
 * - SOF (4 bits)
 * - TYPE (3 bits)
 * - TARGET_LEN (3 bits) [master-to-slave only]
 * - MCID (6 bits)
 * - LEN (5 bits)
 * - PAYLOAD (variable, 0..31 bytes)
 * - CRC8 (8 bits)
 */

#ifndef HUBUS_FRAME_H
#define HUBUS_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include "hubus_slave_config.h"

/* ====================================================================
   Frame Type Definitions (v0)
   ==================================================================== */

// Master command types
#define HUBUS_TYPE_POLL_PENDING      0x0  // TYPE = 000
#define HUBUS_TYPE_REQUEST_INFO      0x1  // TYPE = 001
#define HUBUS_TYPE_REQUEST_DATA      0x2  // TYPE = 010
#define HUBUS_TYPE_SEND_COMMAND      0x3  // TYPE = 011

// Slave response types
#define HUBUS_TYPE_SLAVE_ACK         0x4  // TYPE = 100
#define HUBUS_TYPE_SLAVE_DATA        0x5  // TYPE = 101
#define HUBUS_TYPE_SLAVE_STATUS      0x6  // TYPE = 110
#define HUBUS_TYPE_SLAVE_ERROR       0x7  // TYPE = 111

// SOF marker (4 bits, all 1s)
#define HUBUS_SOF_MARKER             0xF

/* ====================================================================
   Received Frame Structure (Master → Slave)
   ==================================================================== */

typedef struct {
    uint8_t type;           // TYPE field (3 bits)
    uint8_t target_len;     // TARGET_LEN field (3 bits) - significant prefix bits
    uint8_t mcid;           // MCID (6 bits) - target prefix
    uint8_t len;            // LEN (5 bits) - payload length
    uint8_t payload[HUBUS_MAX_PAYLOAD];
    uint8_t crc;            // CRC8 byte
    uint8_t is_valid;       // Set to 1 if frame parsed successfully
} hubus_rx_frame_t;

/* ====================================================================
   Transmit Frame Structure (Slave → Master)
   ==================================================================== */

typedef struct {
    uint8_t type;           // TYPE field (3 bits)
    uint8_t mcid;           // Source MCID (6 bits)
    uint8_t len;            // LEN (5 bits) - payload length
    uint8_t payload[HUBUS_MAX_PAYLOAD];
    uint8_t crc;            // CRC8 calculated
} hubus_tx_frame_t;

/* ====================================================================
   Frame Parsing and Building
   ==================================================================== */

/**
 * Check if a received MCID matches the target prefix in a master frame
 * 
 * @param slave_mcid   own MCID (6 bits)
 * @param target_mcid  target prefix from frame
 * @param target_len   number of significant bits in target_mcid
 * @return             1 if addressed, 0 otherwise
 */
uint8_t hubus_frame_is_addressed(uint8_t slave_mcid, uint8_t target_mcid, uint8_t target_len);

/**
 * Build a slave transmit frame with given type, payload
 * Automatically calculates CRC8
 * 
 * @param frame        pointer to tx frame structure to fill
 * @param type         HUBUS_TYPE_SLAVE_* value
 * @param mcid         source (own) MCID
 * @param payload      data to send (can be NULL if len=0)
 * @param len          payload length
 * @return             1 on success, 0 on error
 */
uint8_t hubus_frame_build_tx(hubus_tx_frame_t *frame, 
                             uint8_t type, uint8_t mcid,
                             const uint8_t *payload, uint8_t len);

/**
 * Serialize a transmit frame to wire format (bit-level)
 * Caller must provide output buffer and manage bit-stream writing
 * 
 * @param frame        frame to serialize
 * @return             number of bytes needed (frame size in bytes)
 * 
 * Note: Actual bit-level transmission is handled by state machine
 */
uint8_t hubus_frame_tx_size(const hubus_tx_frame_t *frame);

#endif // HUBUS_FRAME_H
