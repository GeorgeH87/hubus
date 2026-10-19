/**
 * HuBus Slave Library - State Machine (PCINT-Based)
 * 
 * Implements the slave-side state machine per PROTOCOL.md:
 * IDLE -> MASTER_INTENT -> SLAVE_INTENT -> ARBITRATION -> TRANSFER -> RECOVERY
 * 
 * Driven by CLK line interrupts (PCINT); event-based, not polling.
 */

#ifndef HUBUS_STATE_H
#define HUBUS_STATE_H

#include <stdint.h>
#include "hubus_frame.h"
#include "hubus_bitbuffer.h"

/* ====================================================================
   State Definitions
   ==================================================================== */

typedef enum {
    HUBUS_STATE_IDLE,           // Waiting for master activity or own pending event
    HUBUS_STATE_MASTER_INTENT,  // Master is sending command frame (receiving bits)
    HUBUS_STATE_SLAVE_INTENT,   // Slave signals pending data on DATA
    HUBUS_STATE_ARBITRATION,    // Multiple slaves arbitrating (MCID comparison)
    HUBUS_STATE_TRANSFER,       // Selected slave sending response
    HUBUS_STATE_RECOVERY        // Error/timeout handling
} hubus_state_t;

/* ====================================================================
   Event Types (for Callback System)
   ==================================================================== */

typedef enum {
    HUBUS_EVENT_IDLE,              // Returned to IDLE (no action)
    HUBUS_EVENT_REQUEST_DATA,      // Master requests data (POLL_PENDING, REQUEST_INFO, or REQUEST_DATA)
    HUBUS_EVENT_COMMAND,           // Master sends command (SEND_COMMAND)
    HUBUS_EVENT_TRANSFER_COMPLETE, // Data transmission complete
    HUBUS_EVENT_TRANSFER_FAILED,   // Transfer failed or timed out
    HUBUS_EVENT_ARBITRATION_WON,   // Won arbitration (multi-slave)
    HUBUS_EVENT_ARBITRATION_LOST   // Lost arbitration to higher MCID
} hubus_event_t;

/* ====================================================================
   Callback Function Type
   ==================================================================== */

/**
 * User-provided callback for events
 * 
 * @param event      the event that occurred
 * @param payload    pointer to payload data (NULL if no payload)
 * @param len        payload length (0 if no payload)
 */
typedef void (*hubus_callback_t)(hubus_event_t event, const uint8_t *payload, uint8_t len);

/* ====================================================================
   State Machine Structure
   ==================================================================== */

typedef struct {
    // Current state
    hubus_state_t state;
    
    // Local slave ID (6 bits)
    uint8_t mcid;
    
    // Optional callback
    hubus_callback_t callback;
    
    // Last received frame (when in MASTER_INTENT)
    hubus_rx_frame_t rx_frame;
    
    // Transmit frame (when in TRANSFER state)
    hubus_tx_frame_t tx_frame;
    
    // Bit buffer for CLK-synchronized frame reception (PCINT mode)
    hubus_bitbuffer_t rx_bitbuffer;
    
    // Internal timing: count CLK ticks for timeout detection
    uint16_t clk_tick_count;
    
    // Arbitration state: current bit being transmitted (0..5)
    uint8_t arb_bit_index;
    
    // Arbitration result: did we win? (1=won, 0=lost)
    uint8_t arb_won;
    
    // Transfer state: current byte/bit being transmitted
    uint8_t tx_byte_index;
    uint8_t tx_bit_index;
    
    // Pending event flag: set when slave detects local event (wants to signal)
    uint8_t pending_event;
    
} hubus_state_machine_t;

/* ====================================================================
   State Machine Initialization and Control
   ==================================================================== */

/**
 * Initialize state machine
 * 
 * @param mcid      own MCID (6 bits)
 * @param callback  optional callback for events (can be NULL)
 * @return          pointer to state machine instance (singleton)
 */
hubus_state_machine_t *hubus_state_init(uint8_t mcid, hubus_callback_t callback);

/**
 * Get current state machine instance
 * 
 * @return  pointer to state machine
 */
hubus_state_machine_t *hubus_state_get(void);

/**
 * Set pending event flag (slave wants to signal master)
 * Triggers transition from IDLE to SLAVE_INTENT
 */
void hubus_state_set_pending(void);

/**
 * Clear pending event flag
 */
void hubus_state_clear_pending(void);

/**
 * Main processing function: call from main loop when CLK edge detected
 * 
 * Processes one bit of incoming frame or transitions state based on current activity.
 * Non-blocking, runs quickly.
 * 
 * @return  1 if state changed, 0 if still in same state
 */
uint8_t hubus_state_process_clk_edge(void);

/**
 * Process periodic tasks (timeout checks, idle maintenance, etc.)
 * Call periodically even when no CLK activity (e.g., every 1ms)
 * 
 * @return  1 if state changed, 0 otherwise
 */

/**
 * Get current state name (for debugging)
 * 
 * @param state  the state to describe
 * @return       pointer to state name string
 */
const char *hubus_state_name(hubus_state_t state);

#endif // HUBUS_STATE_H
