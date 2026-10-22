/*
 * Passive temperature sensor slave (ATtiny85), built without the event buffer.
 *
 *   PB2 = CLK, PB3 = DATA (library, see slave/hubus_slave_config.h)
 *
 * Build options: HUBUS_ENABLE_EVENTS=0, HUBUS_ENABLE_STATE=1 (docs/spec/07-slave.md
 * section 7.4). The slave never speaks on its own: no idle event signaling, no
 * REQUEST_DATA, no STATUS_CAPTURE. It exposes the latest raw value of the
 * internal temperature sensor (ADC channel 15, 1.1 V reference) as its live
 * state, two bytes, big endian, and the master reads it whenever it needs it
 * with REQUEST_STATE: hubusd does so every `poll_interval_s` seconds. A group
 * REQUEST_STATE reads several such sensors, one transaction each.
 *
 * Compared with the event-based sensor (slave-sensor) the library needs about
 * 500 bytes less flash and 39 bytes less RAM.
 *
 * The library reads the state while it transmits, without a copy. If a new
 * measurement changes the high byte while a frame is on the bus, the master
 * can receive a mixed value once (valid CRC, docs/spec/08-faults.md section
 * 8.6); the next read is correct again.
 */
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stdint.h>
#include <util/atomic.h>
#include <util/delay.h>

#include "hubus_slave.h"

#if HUBUS_ENABLE_EVENTS || !HUBUS_ENABLE_STATE
#error "build the passive sensor with HUBUS_ENABLE_EVENTS=0 and HUBUS_ENABLE_STATE=1"
#endif

/* SLAVE_INFO application bytes: device type, firmware version, serial number. */
#define DEVICE_TYPE_TEMPERATURE 0x03
static const uint8_t info[] PROGMEM = {DEVICE_TYPE_TEMPERATURE, 0x01, 0x00, 0x00, 0x00, 0x08};

static volatile uint8_t state[2]; /* live state for REQUEST_STATE: raw ADC value, big endian */

static uint16_t measure(void)
{
    ADMUX = _BV(REFS1) | 0x0F;                    /* 1.1 V reference, temperature sensor */
    ADCSRA = _BV(ADEN) | _BV(ADPS2) | _BV(ADPS1); /* clk/64 */
    _delay_ms(1);                                 /* sensor settling */
    ADCSRA |= _BV(ADSC);
    while (ADCSRA & _BV(ADSC)) {
    }
    ADCSRA |= _BV(ADSC); /* the first conversion after switching the channel is discarded */
    while (ADCSRA & _BV(ADSC)) {
    }
    return ADC;
}

static void publish(uint16_t value)
{
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
        state[0] = (uint8_t)(value >> 8);
        state[1] = (uint8_t)value;
    }
}

int main(void)
{
    static const hubus_slave_config_t cfg = {
        .mcid = 0x08,
        .info = info,
        .info_len = sizeof info,
        .state = state,
        .state_len = sizeof state,
        .on_command = 0,
        .ctx = 0,
    };

    publish(measure()); /* a valid value before the first read */
    hubus_slave_init(&cfg);
    sei();

    for (;;) {
        _delay_ms(1000);
        publish(measure());
    }
}
