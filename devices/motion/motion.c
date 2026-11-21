/*
 * Motion sensor slave with a HC-SR501 PIR module (ATtiny85).
 *
 *   PB2 = CLK, PB3 = DATA (library, see slave/hubus_slave_config.h)
 *   PB0 = output of the HC-SR501 (3.3 V when it sees motion, no pull-up needed)
 *
 * Build options: HUBUS_ENABLE_EVENTS=1, HUBUS_ENABLE_STATE=0 (docs/spec/07-slave.md section 7.4), and
 * HUBUS_ENABLE_UPDATE=1 for the firmware update (the module `motion` states it). The slave has no other function: it
 * reports every change of the sensor as an event of one byte, 1 for motion and 0 for none, and the master collects it in
 * a burst. The state cannot be read.
 *
 * The HC-SR501 needs 30 to 60 s after power-up before its output means anything. The slave ignores the pin until
 * PIR_WARMUP_MS has passed and then reports the current level once, so the master knows it from then on. The time the
 * output stays high after motion (0.3 s to 5 min) and its retriggering are set on the module, not here; the module needs
 * 4.5 to 20 V, so it sits on its own supply with a common ground, and only its 3.3 V output goes to the slave.
 *
 * MCID: the last EEPROM byte, written when flashing (tools/flash-avr.sh), or 12.
 */
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stdint.h>
#include <util/delay.h>

#include "hubus_slave.h"

#if HUBUS_ENABLE_STATE || !HUBUS_ENABLE_EVENTS
#error "build the motion sensor with HUBUS_ENABLE_EVENTS=1 and HUBUS_ENABLE_STATE=0"
#endif

#define PIR_BIT PB0
#define DEBOUNCE_SAMPLES 10 /* samples of 1 ms */
#ifndef PIR_WARMUP_MS
#define PIR_WARMUP_MS 30000u /* the module needs 30 to 60 s; the slave stays quiet for the shorter time */
#endif

/* Hardware identifier (docs/hwid-types.md): type, variant, version, hardware revision, serial number. */
static const uint8_t hwid[] PROGMEM = {0x88, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x09};

int main(void)
{
    static hubus_slave_config_t cfg = {
        .hwid = hwid,
        .hwid_len = sizeof hwid,
        .on_status_capture = 0,
        .on_command = 0,
        .ctx = 0,
    };
    uint16_t warmup = PIR_WARMUP_MS;
    uint8_t stable = 0;
    uint8_t level = 0;    /* debounced */
    uint8_t reported = 2; /* nothing yet: the first event carries the level after the warm-up */

    DDRB &= (uint8_t)~_BV(PIR_BIT);
    PORTB &= (uint8_t)~_BV(PIR_BIT); /* no pull-up: the module drives the pin */
    cfg.mcid = hubus_slave_mcid_from_eeprom(0x0C); /* written when flashing (tools/flash-avr.sh), else 12 */
    hubus_slave_init(&cfg);
    sei();

    for (;;) {
        _delay_ms(1);
        if (warmup) {
            warmup--;
            level = (PINB & _BV(PIR_BIT)) ? 1 : 0;
            continue;
        }
        {
            uint8_t now = (PINB & _BV(PIR_BIT)) ? 1 : 0;

            if (now == level) {
                stable = 0;
            } else if (++stable >= DEBOUNCE_SAMPLES) {
                level = now;
                stable = 0;
            }
        }
        if (level != reported) {
            uint8_t event = level;

            /* Retry until the write is accepted (it is refused while the buffer is being transmitted). */
            if (hubus_slave_write(&event, 1, 0) == HUBUS_WRITE_OK) {
                reported = level;
            }
        }
    }
}
