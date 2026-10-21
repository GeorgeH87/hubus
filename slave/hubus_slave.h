/*
 * HUBUS slave library - public API.
 *
 * The library is fully interrupt driven: init() enables the pin change
 * interrupts for CLK and DATA, and everything else happens in the ISR. The
 * main loop only produces data and consumes commands.
 *
 *   hubus_slave_init(&cfg);              // also enables global interrupts
 *   ...
 *   hubus_slave_write(data, len, 0);     // buffer data, announce it to the master
 *
 * Callbacks run in the ISR and must be short (no delays, no blocking).
 * Build-time options are described in hubus_slave_config.h.
 */
#ifndef HUBUS_SLAVE_H
#define HUBUS_SLAVE_H

#include <stdint.h>

#include "hubus_slave_core.h"

/*
 * Slave configuration (hubus_core_config_t):
 *
 *   mcid               1..63, unique on the bus
 *   info, info_len     application bytes of SLAVE_INFO, in flash (PROGMEM) on AVR, 0..28 bytes;
 *                      include a device-unique value (serial number)
 *   state, state_len   live state for REQUEST_STATE (RAM, read while transmitting, 0..31 bytes); NULL: no state.
 *                      The application keeps it up to date; no copy is made. Needs HUBUS_ENABLE_STATE.
 *   on_status_capture  writes the current status via hubus_slave_write(); NULL: not supported
 *   on_command         application commands (opcode 0x10..0xFF); may be NULL
 *   ctx                passed to the callbacks
 */
typedef hubus_core_config_t hubus_slave_config_t;

/*
 * Configures the pins, starts the protocol engine, and enables global
 * interrupts. Returns 1 on success, 0 for an invalid configuration (MCID
 * outside 1..63, info too long); the slave then stays silent.
 */
uint8_t hubus_slave_init(const hubus_slave_config_t *cfg);

/*
 * Writes the outgoing buffer (event data or captured status): overwrites
 * unread content, increments GEN, sets the pending flag and announces the
 * data to the master. `more` = 1 marks a fragment that is followed by more.
 * Safe to call from the main loop and from callbacks.
 *
 * Returns HUBUS_WRITE_OK, HUBUS_WRITE_TOO_LONG (len > HUBUS_TX_BUF_SIZE), or
 * HUBUS_WRITE_BUSY (the buffer is being transmitted right now; try again).
 */
uint8_t hubus_slave_write(const uint8_t *data, uint8_t len, uint8_t more);

/* 1 while buffered data has not been read by the master. Fragmented transfers write the next fragment when this is 0. */
uint8_t hubus_slave_is_pending(void);

/* Pin change handler; called by the HAL vectors (and by host tests). */
void hubus_slave_isr(void);

#endif /* HUBUS_SLAVE_H */
