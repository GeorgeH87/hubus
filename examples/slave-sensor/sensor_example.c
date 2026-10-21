/*
 * Temperature sensor slave (ATtiny85).
 *
 *   PB2 = CLK, PB3 = DATA (library, see slave/hubus_slave_config.h)
 *
 * Uses the internal temperature sensor of the ATtiny85 (ADC channel 15,
 * 1.1 V reference), so no external pin is needed. The main loop measures
 * about ten times per second. STATUS_CAPTURE writes the latest raw ADC value
 * (two bytes, big endian) into the buffer; the master reads it with
 * REQUEST_DATA or collects it in a burst. Converting the raw value to degrees
 * needs a per-device calibration (about 1 LSB per kelvin, see datasheet).
 */
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stdint.h>
#include <util/atomic.h>
#include <util/delay.h>

#include "hubus_slave.h"

/* SLAVE_INFO application bytes: device type, firmware version, serial number. */
#define DEVICE_TYPE_TEMPERATURE 0x03
static const uint8_t info[] PROGMEM = {DEVICE_TYPE_TEMPERATURE, 0x01, 0x00, 0x00, 0x00, 0x03};

static volatile uint16_t latest;

static uint16_t measure(void)
{
    ADMUX = _BV(REFS1) | 0x0F;                /* 1.1 V reference, temperature sensor */
    ADCSRA = _BV(ADEN) | _BV(ADPS2) | _BV(ADPS1); /* clk/64 */
    _delay_ms(1);                             /* sensor settling */
    ADCSRA |= _BV(ADSC);
    while (ADCSRA & _BV(ADSC)) {
    }
    ADCSRA |= _BV(ADSC); /* the first conversion after switching the channel is discarded */
    while (ADCSRA & _BV(ADSC)) {
    }
    return ADC;
}

static void on_status_capture(void *ctx)
{
    uint16_t value = latest;
    uint8_t bytes[2];

    (void)ctx;
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
    (void)hubus_slave_write(bytes, sizeof bytes, 0);
}

int main(void)
{
    static const hubus_slave_config_t cfg = {
        .mcid = 0x07,
        .info = info,
        .info_len = sizeof info,
        .on_status_capture = on_status_capture,
        .on_command = 0,
        .ctx = 0,
    };

    latest = measure();
    hubus_slave_init(&cfg);

    for (;;) {
        uint16_t value = measure();

        ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
        {
            latest = value;
        }
        _delay_ms(100);
    }
}
