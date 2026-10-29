/*
 * HUBUS slave library - public API.
 *
 * The library is fully interrupt driven: init() enables the pin change
 * interrupts for CLK and DATA, and everything else happens in the ISR. The
 * main loop only produces data and consumes commands.
 *
 *   hubus_slave_init(&cfg);              // pin change interrupts for CLK and DATA
 *   sei();                               // the application enables global interrupts when it is ready
 *   ...
 *   hubus_slave_write(data, len, 0);     // buffer data, announce it to the master (HUBUS_ENABLE_EVENTS)
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
 *   info, info_len     application bytes of SLAVE_INFO, in flash (PROGMEM) on AVR, 0..27 bytes;
 *                      include a device-unique value (serial number)
 *   state, state_len   live state for REQUEST_STATE (RAM, read while transmitting, 0..31 bytes); NULL: no state.
 *                      A longer state_len is an invalid configuration.
 *                      The application keeps it up to date; no copy is made. Needs HUBUS_ENABLE_STATE.
 *   on_status_capture  writes the current status via hubus_slave_write(); NULL: not supported.
 *                      Needs HUBUS_ENABLE_EVENTS.
 *   on_command         application commands (opcode 0x10..0xFF); may be NULL
 *   ctx                passed to the callbacks
 *   speed_class        0..15: edge response time this firmware guarantees, announced in SLAVE_INFO
 *                      (docs/spec/01-physical-layer.md section 1.5); 0 = not declared
 *   blind_class        0..15: how long the firmware may be blind after a command addressed to it, announced in
 *                      SLAVE_INFO (docs/spec/07-slave.md section 7.8); 0 = never blind
 */
typedef hubus_core_config_t hubus_slave_config_t;

/*
 * Configures the pins, starts the protocol engine, and enables the pin change
 * interrupts. Global interrupts stay as they are: the application calls sei()
 * once its own setup is done. Returns 1 on success, 0 for an invalid
 * configuration (MCID outside 1..63, info or state too long, speed or blind class above 15); the slave then
 * stays silent.
 */
uint8_t hubus_slave_init(const hubus_slave_config_t *cfg);

#if HUBUS_ENABLE_EVENTS
/*
 * Writes the outgoing buffer (event data or captured status): overwrites
 * unread content, increments GEN, sets the pending flag and announces the
 * data to the master. `more` = 1 marks a fragment that is followed by more.
 * Safe to call from the main loop and from callbacks.
 *
 * Returns HUBUS_WRITE_OK, HUBUS_WRITE_TOO_LONG (len > HUBUS_TX_BUF_SIZE),
 * HUBUS_WRITE_BUSY (the buffer is being transmitted right now; try again), or
 * HUBUS_WRITE_DISABLED (hubus_slave_init() failed or was not called).
 */
uint8_t hubus_slave_write(const uint8_t *data, uint8_t len, uint8_t more);

/* 1 while buffered data has not been read by the master. Fragmented transfers write the next fragment when this is 0. */
uint8_t hubus_slave_is_pending(void);
#endif /* HUBUS_ENABLE_EVENTS: a slave without the event buffer never speaks on its own */

/*
 * Blind section (docs/spec/07-slave.md section 7.8): timing-critical code that has to run with all interrupts
 * disabled for longer than the speed class allows, for example a WS2812 frame. Call from the main loop with interrupts
 * enabled, typically right after a command that needs it:
 *
 *   while (!hubus_slave_blind_begin()) { }  // waits until the slave neither drives DATA nor owes an answer
 *   send_frame();                            // all interrupts disabled
 *   hubus_slave_blind_end();                 // resynchronizes and enables interrupts again
 *
 * hubus_slave_blind_begin() returns 1 with all interrupts disabled, or 0 with them enabled when the slave is busy on
 * the bus (try again). hubus_slave_blind_end() takes the current pin levels as the reference and ignores the bus until
 * the next START, STOP, or RESET, so the frames it missed cannot be misread. The section has to end within the blind
 * time announced with blind_class, counted from the STOP of the command; the master does not address the slave
 * before that.
 */
uint8_t hubus_slave_blind_begin(void);
void hubus_slave_blind_end(void);

/*
 * Interruptible section (docs/slave-library.md): long timing-critical work that runs in short pieces with all
 * interrupts disabled, gives way to the bus, and is repeated later, for example a WS2812 frame of an animation. The
 * slave is never blind; a piece only delays the reaction to an edge, so the longest piece counts towards the speed
 * class.
 *
 *   if (hubus_slave_short_begin()) {        // 1: bus between transactions, interrupts disabled
 *       for (each piece) {
 *           if (hubus_slave_short_interrupted()) {
 *               break;                       // an edge waits: give up the rest
 *           }
 *           send_piece();                    // interrupts still disabled
 *       }
 *       hubus_slave_short_end();             // interrupts on; the ISR handles the edge
 *   }
 *
 * hubus_slave_short_begin() returns 1 with all interrupts disabled if the bus is between transactions and no edge
 * waits, else 0 with interrupts enabled (try again later). hubus_slave_short_interrupted() returns 1 once a CLK or
 * DATA change waits for the ISR: the caller then ends the section without delay. Unlike a blind section it needs no
 * announcement to the master, and nothing the master sends is missed.
 */
uint8_t hubus_slave_short_begin(void);
uint8_t hubus_slave_short_interrupted(void);
void hubus_slave_short_end(void);

/* Pin change handler; called by the HAL vectors (and by host tests). */
void hubus_slave_isr(void);

#endif /* HUBUS_SLAVE_H */
