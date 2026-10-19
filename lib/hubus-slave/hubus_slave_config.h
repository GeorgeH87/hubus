/**
 * HuBus Slave Library - Configuration Header
 * 
 * Compile-time configuration for ATtiny and compatible devices.
 * Adapt GPIO pins, buffer sizes, and CRC8 parameters as needed.
 */

#ifndef HUBUS_SLAVE_CONFIG_H
#define HUBUS_SLAVE_CONFIG_H

#include <stdint.h>
#include <stddef.h>

/* ====================================================================
   GPIO Configuration (for ATtiny85 by default)
   
   On ATtiny85:
   - PORTB is the only port
   - Pins: PB0, PB1, PB2, PB3, PB4
   
   Adapt for ATtiny88, ATtiny1616, etc. by changing PORT/DDR/PIN names.
   ==================================================================== */

// Clock line (input from master)
#define HUBUS_CLK_PORT      PORTB
#define HUBUS_CLK_PIN       PB2
#define HUBUS_CLK_DDR       DDRB
#define HUBUS_CLK_BIT       2

// Data line (open-drain, input or pull-low)
#define HUBUS_DATA_PORT     PORTB
#define HUBUS_DATA_PIN      PB3
#define HUBUS_DATA_DDR      DDRB
#define HUBUS_DATA_BIT      3

/* ====================================================================
   Buffer Configuration
   ==================================================================== */

// Maximum payload bytes per frame (v0: up to 31)
#define HUBUS_MAX_PAYLOAD   31

// Buffered data size (for SLAVE_DATA responses)
#define HUBUS_BUFFER_SIZE   32

// Status data size (for SLAVE_STATUS responses)
#define HUBUS_STATUS_SIZE   16

/* ====================================================================
   CRC8 Configuration
   
   Standard parameters for CRC8. Adjust if protocol specifies differently.
   ==================================================================== */

// CRC8 polynomial (without implicit leading 1 bit; used in lookup table)
#define HUBUS_CRC8_POLY     0xD5

// Initial value for CRC8 calculation
#define HUBUS_CRC8_INIT     0x00

// XOR-out value (applied after last bit)
#define HUBUS_CRC8_XOROUT   0x00

// CRC8 calculation method:
//   0 = Lookup table (fast, 256 B ROM, good for larger devices)
//   1 = Bit-by-bit calculation (slow, ~40 B ROM, best for ATtiny85)
#define HUBUS_CRC8_USE_BITWISE 1

/* ====================================================================
   Interrupt Mode (PCINT for bit-level synchronisation)
   ==================================================================== */

// Enable Pin Change Interrupt for CLK line (highly recommended)
// Set to 1 for interrupt-driven, event-based processing
// Set to 0 for polling-based (legacy, less accurate)
#define HUBUS_USE_PCINT 1

// CLK pin change type:
//   0 = trigger on both edges (rising and falling)
//   1 = trigger on rising edge only
//   2 = trigger on falling edge only
#define HUBUS_CLK_TRIGGER_EDGE 0

/* ====================================================================
   Timing and Constraints
   ==================================================================== */

// Maximum number of clock ticks to wait for a complete frame
// Helps detect timeouts and line faults
#define HUBUS_RX_TIMEOUT_TICKS   1000

// Minimum delay (in CPU cycles or busy-wait iterations) between
// checking CLK/DATA lines to avoid sampling errors
#define HUBUS_SAMPLE_DELAY_CYCLES 2

// CLK synchronisation: sample DATA a few cycles after CLK edge
// to allow signal settling (in CPU cycles)
#define HUBUS_CLK_SAMPLE_DELAY    5

/* ====================================================================
   Debugging / Optional Features
   ==================================================================== */

// Enable generation counter in status buffer (tracks overwrites)
#define HUBUS_ENABLE_GENERATION_COUNTER 1

// Maximum retries for failed frame parsing before giving up
#define HUBUS_MAX_RX_RETRIES 3

#endif // HUBUS_SLAVE_CONFIG_H
