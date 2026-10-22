/*
 * Relay without feedback (ATtiny85), built without event buffer and state.
 *
 *   PB2 = CLK, PB3 = DATA (library, see slave/hubus_slave_config.h)
 *   PB0 = relay driver / LED, active high
 *
 * Build options: HUBUS_ENABLE_EVENTS=0, HUBUS_ENABLE_STATE=0 (docs/spec/07-slave.md
 * section 7.4), the smallest build of the library. The slave executes
 * commands and answers REQUEST_INFO, nothing else:
 *
 *   0x10 <0|1>   switch the output off / on
 *   0x11         toggle the output
 *
 * Commands are never acknowledged, and this slave has nothing the master
 * could read back. The master therefore keeps the state it commanded in its
 * cache: hubusd sees in SLAVE_INFO that the slave has neither a state nor
 * STATUS_CAPTURE and publishes the commanded state (readback `cache`). A
 * command that did not arrive stays unnoticed; that is the price of this build.
 * Use slave-relay (state readable with REQUEST_STATE) where it matters.
 *
 * Everything runs in the ISR; the main loop sleeps.
 */
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/sleep.h>

#include "hubus_slave.h"

#if HUBUS_ENABLE_EVENTS || HUBUS_ENABLE_STATE
#error "build the simple relay with HUBUS_ENABLE_EVENTS=0 and HUBUS_ENABLE_STATE=0"
#endif

#define RELAY_BIT PB0

#define OP_SET 0x10
#define OP_TOGGLE 0x11

/* SLAVE_INFO application bytes: device type, firmware version, serial number. */
#define DEVICE_TYPE_RELAY 0x01
static const uint8_t info[] PROGMEM = {DEVICE_TYPE_RELAY, 0x01, 0x00, 0x00, 0x00, 0x04};

static void on_command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    (void)ctx;
    if (opcode == OP_SET && len >= 1) {
        if (args[0]) {
            PORTB |= _BV(RELAY_BIT);
        } else {
            PORTB &= (uint8_t)~_BV(RELAY_BIT);
        }
    } else if (opcode == OP_TOGGLE) {
        PORTB ^= _BV(RELAY_BIT);
    }
}

int main(void)
{
    static const hubus_slave_config_t cfg = {
        .mcid = 0x04,
        .info = info,
        .info_len = sizeof info,
        .on_command = on_command,
        .ctx = 0,
    };

    DDRB |= _BV(RELAY_BIT);
    hubus_slave_init(&cfg);
    sei();

    set_sleep_mode(SLEEP_MODE_IDLE);
    for (;;) {
        sleep_mode();
    }
}
