/*
 * Temperature and humidity slave with a DHT11 module (ATtiny85), read only.
 *
 *   PB2 = CLK, PB3 = DATA (library, see slave/hubus_slave_config.h)
 *   PB0 = data pin of the DHT11 (external pull-up of 4.7 to 10 kOhm to 3.3 V; most modules have one)
 *
 * Build options: HUBUS_ENABLE_EVENTS=0, HUBUS_ENABLE_STATE=1 (docs/spec/07-slave.md section 7.4), and
 * HUBUS_ENABLE_UPDATE=1 for the firmware update. The slave never speaks on its own; the master reads the state with
 * REQUEST_STATE (hubusd: every `poll_interval_s`). The state is three bytes:
 *
 *   0  relative humidity in percent
 *   1  temperature in degrees Celsius
 *   2  fault: 0 the last reading was good, 1 no good reading yet, or the last two attempts failed (the values are then
 *      the last good ones, or 0)
 *
 * A reading takes the sensor about 4 ms in which it sends 40 bits, and the pulse widths decide the bits, so the slave
 * must not be interrupted in between. It therefore reads in an interruptible section of the library
 * (docs/slave-library.md): the pin is sampled with all interrupts disabled, and before every bit the slave checks whether
 * the bus has an edge waiting. If it has, the reading is dropped and repeated after a pause, so the slave never misses an
 * edge for longer than one bit (about 120 us). A reading also fails when the sensor does not answer, when the checksum is
 * wrong, or when a value is out of range.
 *
 * The bit is told from the length of its high phase against the length of the low phase before it (a 1 is longer), so
 * the result does not depend on the clock of the device.
 *
 * MCID: the last EEPROM byte, written when flashing (tools/flash-avr.sh), or 13.
 */
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stdint.h>
#include <string.h>
#include <util/atomic.h>
#include <util/delay.h>

#include "hubus_slave.h"

#if HUBUS_ENABLE_EVENTS || !HUBUS_ENABLE_STATE
#error "build the DHT11 sensor with HUBUS_ENABLE_EVENTS=0 and HUBUS_ENABLE_STATE=1"
#endif

#define DHT_BIT PB0
#ifndef DHT_FIRST_MS
#define DHT_FIRST_MS 1000u /* the DHT11 needs one second after power-up */
#endif
#ifndef DHT_PERIOD_MS
#define DHT_PERIOD_MS 2000u /* the DHT11 may be read once per second at most */
#endif
#define DHT_RETRY_MS 100u
#define DHT_BUSY_MS 500u /* until the next try when the bus was too busy */
#define DHT_ATTEMPTS 3u
#define DHT_FAULT_AFTER 2u /* failed readings in a row */

/* Hardware identifier (docs/hwid-types.md): type, variant, version, hardware revision, serial number. */
static const uint8_t hwid[] PROGMEM = {0x86, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x0A};

static volatile uint8_t state[3]; /* live state for REQUEST_STATE: humidity, temperature, fault */

enum { DHT_OK, DHT_BUSY, DHT_INTERRUPTED, DHT_ERROR };

static uint8_t aborted; /* the bus had an edge waiting: the reading is dropped */

/* Loops while the pin has `level` and returns how many it counted: 1 to 254, 0 if the pin did not change within about
 * 200 us, or if the bus has an edge waiting (`aborted`). A change that is already there counts 1. The bus is looked at
 * every 16 counts, about every 12 us, so no edge waits longer than that. */
static uint8_t wait_while(uint8_t level)
{
    uint8_t n = 0;

    while (((PINB & _BV(DHT_BIT)) != 0) == level) {
        if (++n == 255) {
            return 0;
        }
        if ((n & 15u) == 0 && hubus_slave_short_interrupted()) {
            aborted = 1;
            return 0;
        }
    }
    return n ? n : 1;
}

/* The answer of the sensor after the start signal. Called in an interruptible section, right after the pin was
 * released. */
static uint8_t receive(uint8_t data[5])
{
    uint8_t i;

    /* the line rises, stays high for 20 to 40 us, the sensor pulls it low for 80 us and releases it for 80 us */
    if (!wait_while(0) || !wait_while(1) || !wait_while(0) || !wait_while(1)) {
        return aborted ? DHT_INTERRUPTED : DHT_ERROR;
    }
    for (i = 0; i < 40; i++) {
        uint8_t low = wait_while(0);
        uint8_t high = low ? wait_while(1) : 0;

        if (!low || !high) {
            return aborted ? DHT_INTERRUPTED : DHT_ERROR;
        }
        data[i >> 3] = (uint8_t)((data[i >> 3] << 1) | (high > low));
    }
    return DHT_OK;
}

static uint8_t read_once(uint8_t data[5])
{
    uint8_t tries = 0;
    uint8_t result;

    aborted = 0;
    memset(data, 0, 5);
    PORTB &= (uint8_t)~_BV(DHT_BIT);
    DDRB |= _BV(DHT_BIT); /* start signal: at least 18 ms low, with the interrupts on */
    _delay_ms(20);
    /* The section starts when the bus is between two transactions. Until then the line stays low, which the sensor
     * tolerates for some milliseconds. */
    while (!hubus_slave_short_begin()) {
        if (++tries >= 10) {
            DDRB &= (uint8_t)~_BV(DHT_BIT);
            return DHT_BUSY;
        }
        _delay_ms(1);
    }
    DDRB &= (uint8_t)~_BV(DHT_BIT); /* released: the pull-up raises it */
    result = receive(data);
    hubus_slave_short_end();
    return result;
}

static uint8_t good(const uint8_t data[5])
{
    return (uint8_t)(data[0] + data[1] + data[2] + data[3]) == data[4] && data[0] <= 100 && data[2] <= 60;
}

static void publish(uint8_t humidity, uint8_t temperature, uint8_t fault)
{
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
        state[0] = humidity;
        state[1] = temperature;
        state[2] = fault;
    }
}

static void wait_ms(uint16_t ms)
{
    while (ms--) {
        _delay_ms(1);
    }
}

int main(void)
{
    static hubus_slave_config_t cfg = {
        .hwid = hwid,
        .hwid_len = sizeof hwid,
        .state = state,
        .state_len = sizeof state,
        .on_command = 0,
        .ctx = 0,
    };
    uint16_t wait = DHT_FIRST_MS;
    uint8_t failed = 0;

    state[2] = 1; /* no good reading yet */
    DDRB &= (uint8_t)~_BV(DHT_BIT);
    PORTB &= (uint8_t)~_BV(DHT_BIT);
    cfg.mcid = hubus_slave_mcid_from_eeprom(0x0D); /* written when flashing (tools/flash-avr.sh), else 13 */
    hubus_slave_init(&cfg);
    sei();

    for (;;) {
        uint8_t data[5];
        uint8_t attempt, sensor_failed = 0;

        wait_ms(wait);
        for (attempt = 0; attempt < DHT_ATTEMPTS; attempt++) {
            uint8_t r = read_once(data);

            if (r == DHT_OK && good(data)) {
                break;
            }
            if (r == DHT_ERROR || r == DHT_OK) {
                sensor_failed = 1; /* the sensor said something wrong, or nothing */
            }
            wait_ms(DHT_RETRY_MS);
        }
        wait = DHT_PERIOD_MS;
        if (attempt < DHT_ATTEMPTS) {
            publish(data[0], data[2], 0);
            failed = 0;
        } else if (sensor_failed) {
            if (++failed >= DHT_FAULT_AFTER) {
                ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
                {
                    state[2] = 1;
                }
            }
        } else {
            wait = DHT_BUSY_MS; /* the bus kept the slave from reading: no fault of the sensor, try again soon */
        }
    }
}
