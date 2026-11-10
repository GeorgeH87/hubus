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
 * cache: hubusd knows from its module that the slave has neither a state nor
 * STATUS_CAPTURE and publishes the commanded state (readback `cache`). A
 * command that did not arrive stays unnoticed; that is the price of this build.
 * Use slave-relay (state readable with REQUEST_STATE) where it matters.
 *
 * Everything runs in the ISR; the main loop sleeps.
 *
 * MCID: the last EEPROM byte, written when flashing (tools/flash-avr.sh), or 4. Built with MCID_FROM_PINS=1
 * (example_relay_simple_pins) it comes from two jumpers instead, to show that the source is up to the application:
 *
 *   PB1, PB4 = jumpers to ground (internal pull-ups), read once at reset: MCID 0x20 + 0..3
 *
 * The EEPROM byte means nothing to that build. With an updater, it would hand over the MCID of its jumpers at
 * UPDATE_ENTER, and a change of the MCID over the bus would not last: the application reads its jumpers again
 * (docs/slave-library.md#assigning-the-mcid).
 */
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/sleep.h>
#include <util/delay.h>

#include "hubus_slave.h"

#ifndef MCID_FROM_PINS
#define MCID_FROM_PINS 0
#endif
#define DEFAULT_MCID 0x04
#define PINS_MCID_BASE 0x20
#define JUMPER0_BIT PB1
#define JUMPER1_BIT PB4

#if HUBUS_ENABLE_EVENTS || HUBUS_ENABLE_STATE
#error "build the simple relay with HUBUS_ENABLE_EVENTS=0 and HUBUS_ENABLE_STATE=0"
#endif

#define RELAY_BIT PB0

#define OP_SET 0x10
#define OP_TOGGLE 0x11

/* Hardware identifier (docs/hwid-types.md): type, variant, version, hardware revision, serial number. */
static const uint8_t hwid[] PROGMEM = {0x81, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04};

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

static uint8_t read_mcid(void)
{
#if MCID_FROM_PINS
    uint8_t pins;

    PORTB |= _BV(JUMPER0_BIT) | _BV(JUMPER1_BIT); /* pull-ups; a closed jumper reads 0 */
    _delay_us(20);
    pins = (uint8_t)~PINB;
    return (uint8_t)(PINS_MCID_BASE + ((pins >> JUMPER0_BIT) & 1u) + (((pins >> JUMPER1_BIT) & 1u) << 1));
#else
    return hubus_slave_mcid_from_eeprom(DEFAULT_MCID);
#endif
}

int main(void)
{
    static hubus_slave_config_t cfg = {
        .hwid = hwid,
        .hwid_len = sizeof hwid,
        .on_command = on_command,
        .ctx = 0,
    };

    DDRB |= _BV(RELAY_BIT);
    cfg.mcid = read_mcid();
    hubus_slave_init(&cfg);
    sei();

    set_sleep_mode(SLEEP_MODE_IDLE);
    for (;;) {
        sleep_mode();
    }
}
