/*
 * Switch slave (ATtiny85).
 *
 *   PB2 = CLK, PB3 = DATA (library, see slave/hubus_slave_config.h)
 *   PB1 = push button to ground (internal pull-up)
 *
 * Every debounced state change is written to the buffer (one byte, 1 =
 * pressed) and thereby announced to the master via the idle event handshake
 * (event, REQUEST_DATA). The current state is also exposed live: the master
 * can read it at any time with REQUEST_STATE, independent of events. A slave
 * with several buttons would expose one bit per button in the same way.
 */
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stdint.h>
#include <util/delay.h>

#include "hubus_slave.h"

#define BUTTON_BIT PB1
#define DEBOUNCE_SAMPLES 10 /* samples of 1 ms */

/* SLAVE_INFO application bytes: device type, firmware version, serial number. */
#define DEVICE_TYPE_SWITCH 0x02
static const uint8_t info[] PROGMEM = {DEVICE_TYPE_SWITCH, 0x01, 0x00, 0x00, 0x00, 0x02};

static volatile uint8_t pressed;

static void on_status_capture(void *ctx)
{
    uint8_t state = pressed;

    (void)ctx;
    (void)hubus_slave_write(&state, 1, 0);
}

int main(void)
{
    static const hubus_slave_config_t cfg = {
        .mcid = 0x05,
        .info = info,
        .info_len = sizeof info,
        .state = &pressed,
        .state_len = 1,
        .on_status_capture = on_status_capture,
        .on_command = 0,
        .ctx = 0,
    };
    uint8_t stable = 0;
    uint8_t last_reported = 0;

    DDRB &= (uint8_t)~_BV(BUTTON_BIT);
    PORTB |= _BV(BUTTON_BIT); /* pull-up */
    hubus_slave_init(&cfg);

    for (;;) {
        uint8_t now = (PINB & _BV(BUTTON_BIT)) ? 0 : 1; /* active low */

        if (now == pressed) {
            stable = 0;
        } else if (++stable >= DEBOUNCE_SAMPLES) {
            pressed = now;
            stable = 0;
        }
        if (pressed != last_reported) {
            uint8_t state = pressed;
            /* Retry until the write is accepted (it is refused while the buffer is being transmitted). */
            if (hubus_slave_write(&state, 1, 0) == HUBUS_WRITE_OK) {
                last_reported = state;
            }
        }
        _delay_ms(1);
    }
}
