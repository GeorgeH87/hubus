/*
 * Pin and buffer configuration of the slave library.
 *
 * Define these macros on the compiler command line (-DHUBUS_CLK_PORT=B ...)
 * or in a header included before hubus_slave.h:
 *
 *   HUBUS_CLK_PORT   port letter of CLK   (A, B, C, D)
 *   HUBUS_CLK_PIN    bit number of CLK    (0..7)
 *   HUBUS_DATA_PORT  port letter of DATA
 *   HUBUS_DATA_PIN   bit number of DATA
 *
 * Both pins need pin change / edge interrupts. On classic devices, putting
 * both on the same port keeps them on one interrupt vector.
 *
 * Further compile-time options:
 *
 *   HUBUS_TX_BUF_SIZE  outgoing buffer (SLAVE_DATA payload), bytes 1..31, default 31
 *   HUBUS_RX_BUF_SIZE  largest accepted command payload (announced in SLAVE_INFO), bytes 1..31, default 31
 *                      (the CMake build uses 20 for both on devices with 128 B RAM that have the event buffer)
 *   HUBUS_ENABLE_EVENTS 1 (default): event buffer with hubus_slave_write(), idle event signaling, REQUEST_DATA,
 *                      and STATUS_CAPTURE; 0 leaves it out for slaves that never speak on their own
 *                      (saves about 500 bytes of flash and 39 bytes of RAM)
 *   HUBUS_ENABLE_STATE 1 (default): answer REQUEST_STATE from the state the application exposes; 0 leaves it out
 *                      (saves about 100 bytes of flash and 3 bytes of RAM)
 *   Every combination is valid (docs/spec/07-slave.md section 7.4).
 *
 * DATA needs an external pull-up to 3.3 V; the library never enables
 * internal pull-ups.
 */
#ifndef HUBUS_SLAVE_CONFIG_H
#define HUBUS_SLAVE_CONFIG_H

/* Reference defaults for the ATtiny25/45/85: CLK = PB2, DATA = PB3. */
#if !defined(HUBUS_CLK_PORT) && !defined(HUBUS_CLK_PIN) && !defined(HUBUS_DATA_PORT) && !defined(HUBUS_DATA_PIN) && \
    (defined(__AVR_ATtiny25__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny85__))
#define HUBUS_CLK_PORT B
#define HUBUS_CLK_PIN 2
#define HUBUS_DATA_PORT B
#define HUBUS_DATA_PIN 3
#endif

#if !defined(HUBUS_CLK_PORT) || !defined(HUBUS_CLK_PIN) || !defined(HUBUS_DATA_PORT) || !defined(HUBUS_DATA_PIN)
#error "Define HUBUS_CLK_PORT, HUBUS_CLK_PIN, HUBUS_DATA_PORT and HUBUS_DATA_PIN (see hubus_slave_config.h)"
#endif

#endif /* HUBUS_SLAVE_CONFIG_H */
