/*
 * Relay / LED slave (ATtiny85).
 *
 *   PB2 = CLK, PB3 = DATA (library, see slave/hubus_slave_config.h)
 *   PB0 = relay driver / LED, active high
 *
 * Application commands (SEND_COMMAND, opcode and one argument byte):
 *   0x10 <0|1>   switch the output off / on
 *   0x11         toggle the output
 * The state can be read back with STATUS_CAPTURE followed by REQUEST_DATA
 * (or collected in a burst): one byte, 0 = off, 1 = on. Commands are never
 * acknowledged.
 *
 * Everything runs in the ISR; the main loop is idle.
 */
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/sleep.h>

#include "hubus_slave.h"

#define RELAY_BIT PB0

#define OP_SET 0x10
#define OP_TOGGLE 0x11

/* SLAVE_INFO application bytes: device type, firmware version, serial number. */
#define DEVICE_TYPE_RELAY 0x01
static const uint8_t info[] PROGMEM = {DEVICE_TYPE_RELAY, 0x01, 0x00, 0x00, 0x00, 0x01};

static void relay_set(uint8_t on)
{
    if (on) {
        PORTB |= _BV(RELAY_BIT);
    } else {
        PORTB &= (uint8_t)~_BV(RELAY_BIT);
    }
}

static void on_status_capture(void *ctx)
{
    uint8_t state = (PORTB & _BV(RELAY_BIT)) ? 1u : 0u;

    (void)ctx;
    (void)hubus_slave_write(&state, 1, 0);
}

static void on_command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    (void)ctx;
    if (opcode == OP_SET && len >= 1) {
        relay_set(args[0] != 0);
    } else if (opcode == OP_TOGGLE) {
        PORTB ^= _BV(RELAY_BIT);
    }
}

int main(void)
{
    static const hubus_slave_config_t cfg = {
        .mcid = 0x03,
        .info = info,
        .info_len = sizeof info,
        .on_status_capture = on_status_capture,
        .on_command = on_command,
        .ctx = 0,
    };

    DDRB |= _BV(RELAY_BIT);
    hubus_slave_init(&cfg);

    set_sleep_mode(SLEEP_MODE_IDLE);
    for (;;) {
        sleep_mode();
    }
}
