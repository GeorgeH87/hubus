/**
 * HuBus Slave Library - Public API (ISR-Only Architecture)
 * 
 * Completely interrupt-driven interface.
 * 
 * Usage:
 *   1. Call hubus_slave_init(mcid, callback) once during setup
 *      - Automatically enables CLK interrupt (PCINT)
 *      - ISR runs on every CLK edge
 *   2. Main loop can be empty or do application tasks
 *   3. Use hubus_slave_set_buffered_data() to prepare responses
 *   4. Call hubus_slave_signal_pending() when slave has data
 *   5. All HuBus events handled in callbacks
 */

#ifndef HUBUS_SLAVE_H
#define HUBUS_SLAVE_H

#include <stdint.h>
#include <stddef.h>
#include "hubus_state.h"

/* ====================================================================
   Initialization
   ==================================================================== */

/**
 * Initialize the HuBus slave library with ISR support
 * 
 * @param mcid      own slave ID (6 bits, 0..63)
 * @param callback  optional callback for events (NULL if not using callbacks)
 * 
 * Call this once during setup. Automatically enables PCINT ISR on CLK line.
 * After this, all HuBus communication is interrupt-driven; main loop not involved.
 */
void hubus_slave_init(uint8_t mcid, hubus_callback_t callback);

/**
 * Get the current MCID of this slave
 * 
 * @return  own MCID (6 bits, 0..63)
 */
uint8_t hubus_slave_get_mcid(void);

/* ====================================================================
   Data Management (Manual)
   ==================================================================== */

/**
 * Set buffered data to send on data request
 * 
 * @param data  pointer to data to buffer (can be NULL if len=0)
 * @param len   number of bytes to buffer (0..HUBUS_BUFFER_SIZE)
 * @return      1 on success, 0 if len exceeds buffer size
 * 
 * Data is copied into internal buffer. Multiple calls overwrite previous data.
 * Typical use: call in callback after HUBUS_EVENT_REQUEST_DATA,
 * or proactively before signaling pending.
 */
uint8_t hubus_slave_set_buffered_data(const uint8_t *data, uint8_t len);

/**
 * Set status data to send on next REQUEST_INFO or STATUS_CAPTURE
 * 
 * @param status  pointer to status data (can be NULL if len=0)
 * @param len     number of bytes (0..HUBUS_STATUS_SIZE)
 * @return        1 on success, 0 if len exceeds buffer size
 * 
 * Status is copied into internal buffer. Multiple calls overwrite previous data.
 */
uint8_t hubus_slave_set_status(const uint8_t *status, uint8_t len);

/**
 * Check if buffered data is ready to send
 * 
 * @return  1 if data pending, 0 if empty
 */
uint8_t hubus_slave_has_buffered_data(void);

/**
 * Check if status is ready to send
 * 
 * @return  1 if status pending, 0 if empty
 */
uint8_t hubus_slave_has_status(void);

/* ====================================================================
   Event Signaling
   ==================================================================== */

/**
 * Signal the master that this slave has pending data
 * 
 * Pulls DATA line low (open-drain) to announce availability.
 * Master will respond with CLK ticks, then issue a POLL_PENDING command.
 * 
 * Non-blocking; sets internal flag. Actual transition happens on next CLK edge.
 */
void hubus_slave_signal_pending(void);

/**
 * Clear the pending event flag (typically done automatically after transfer)
 */
void hubus_slave_clear_pending(void);

/**
 * Check if slave currently has a pending event signaled
 * 
 * @return  1 if pending event is active, 0 otherwise
 */
uint8_t hubus_slave_is_pending(void);

/* ====================================================================
   Debugging / Status
   ==================================================================== */

/**
 * Get the current state of the slave (for debugging)
 * 
 * @return  current hubus_state_t value (IDLE, MASTER_INTENT, etc.)
 */
hubus_state_t hubus_slave_get_state(void);

/**
 * Get human-readable state name (for debugging output)
 * 
 * @return  pointer to const string describing current state
 *          (e.g., "IDLE", "MASTER_INTENT", "TRANSFER")
 */
const char *hubus_slave_get_state_name(void);

#endif // HUBUS_SLAVE_H
