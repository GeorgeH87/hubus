/*
 * Distance slave with an HC-SR04 ultrasonic module (ATtiny85).
 *
 *   PB2 = CLK, PB3 = DATA (library, see slave/hubus_slave_config.h)
 *   PB0 = TRIG of the HC-SR04
 *   PB1 = ECHO of the HC-SR04, through a divider (the module answers with 5 V, the slave runs at 3.3 V)
 *
 * Build options: HUBUS_ENABLE_EVENTS=1, HUBUS_ENABLE_STATE=1 (docs/spec/07-slave.md section 7.4), and
 * HUBUS_ENABLE_UPDATE=1 for the firmware update. The slave measures about every 60 ms. The master reads the distance
 * at any time, and the slave announces an event when an object comes closer than a threshold the master has set, and
 * again when it leaves. State and event are the same five bytes:
 *
 *   0    1 the object is nearer than the threshold, else 0 (also 0 while no threshold is set)
 *   1-2  distance in mm, big endian; 0: no echo (nothing in range, or closer than 2 cm)
 *   3-4  threshold in mm, big endian; 0: not set
 *
 * Commands: opcode 0x40, two bytes: the threshold in mm (0 to 4000, 0 switches the events off). The threshold lives
 * in RAM only: after a power-up it is 0, and the slave announces that in an event, so the master sends it again. The
 * master keeps the value, and a default (docs/example-distance.md).
 *
 * An object counts as near after two measurements in a row below the threshold, and as gone after two in a row above
 * the threshold plus 20 mm (or without an echo), so a single wrong echo or a distance on the edge does not make events.
 *
 * Time comes from timer 0 (F_CPU / 64, 8 us per tick), polled; the echo pulse is measured against it, so interrupts of
 * the bus add at most their duration to the result (about 1 cm in the worst case) and never lose time. The speed of
 * sound is taken as 343 m/s: 1 tick = 1.375 mm of distance.
 *
 * MCID: the last EEPROM byte, written when flashing (tools/flash-avr.sh), or 14.
 */
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stdint.h>
#include <util/atomic.h>
#include <util/delay.h>

#include "hubus_slave.h"

#if !HUBUS_ENABLE_EVENTS || !HUBUS_ENABLE_STATE
#error "build the distance sensor with HUBUS_ENABLE_EVENTS=1 and HUBUS_ENABLE_STATE=1"
#endif
#if HUBUS_TX_BUF_SIZE < 5
#error "the event is five bytes"
#endif

#define TRIG_BIT PB0
#define ECHO_BIT PB1
#define OP_SET_THRESHOLD 0x40
#define THRESHOLD_MAX_MM 4000u
#define SAMPLE_MS 60u         /* the module wants 60 ms between two measurements */
#define HYSTERESIS_MM 20u
#define CONFIRM 2u
#define ECHO_START_TICKS 250u  /* 2 ms: the module starts its echo within 0.5 ms */
#define ECHO_MAX_TICKS 3750u   /* 30 ms: about 5 m; further objects do not count */
#define MIN_MM 20u

/* Hardware identifier (docs/hwid-types.md): type, variant, version, hardware revision, serial number. */
static const uint8_t hwid[] PROGMEM = {0x87, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x0B};

static volatile uint8_t state[5]; /* near, distance (2), threshold (2), see above */
static volatile uint16_t cmd_threshold;
static volatile uint8_t cmd_new;

/* Runs in the interrupt handler. */
static void on_command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    uint16_t mm;

    (void)ctx;
    if (opcode == OP_SET_THRESHOLD && len == 2) {
        mm = (uint16_t)(args[0] << 8 | args[1]);
        if (mm <= THRESHOLD_MAX_MM) {
            cmd_threshold = mm;
            cmd_new = 1;
        }
    }
}

static void on_status_capture(void *ctx)
{
    uint8_t snapshot[5];
    uint8_t i;

    (void)ctx;
    for (i = 0; i < 5; i++) {
        snapshot[i] = state[i];
    }
    (void)hubus_slave_write(snapshot, sizeof snapshot, 0);
}

/* Time in ticks of 8 us, summed up from the counter; wraps after 0.5 s. Call at least every 2 ms. */
static uint16_t ticks(void)
{
    static uint8_t last;
    static uint16_t total;
    uint8_t c = TCNT0;

    total = (uint16_t)(total + (uint8_t)(c - last));
    last = c;
    return total;
}

/* Distance in mm, 0 without an echo. */
static uint16_t measure(void)
{
    uint16_t t0, width;

    PORTB |= _BV(TRIG_BIT);
    _delay_us(10);
    PORTB &= (uint8_t)~_BV(TRIG_BIT);
    t0 = ticks();
    while (!(PINB & _BV(ECHO_BIT))) {
        if ((uint16_t)(ticks() - t0) > ECHO_START_TICKS) {
            return 0;
        }
    }
    t0 = ticks();
    while (PINB & _BV(ECHO_BIT)) {
        if ((uint16_t)(ticks() - t0) > ECHO_MAX_TICKS) {
            return 0;
        }
    }
    width = (uint16_t)(ticks() - t0);
    width = (uint16_t)((uint32_t)width * 11u >> 3);
    return width < MIN_MM ? 0 : width;
}

static void publish(uint8_t near, uint16_t distance)
{
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
        state[0] = near;
        state[1] = (uint8_t)(distance >> 8);
        state[2] = (uint8_t)distance;
    }
}

/* Takes a threshold from the master: published at once, so that a read right after the command shows it. */
static uint16_t take_threshold(void)
{
    uint16_t mm;

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
        mm = cmd_threshold;
        cmd_new = 0;
        state[3] = (uint8_t)(mm >> 8);
        state[4] = (uint8_t)mm;
    }
    return mm;
}

int main(void)
{
    static hubus_slave_config_t cfg = {
        .hwid = hwid,
        .hwid_len = sizeof hwid,
        .state = state,
        .state_len = sizeof state,
        .on_status_capture = on_status_capture,
        .on_command = on_command,
        .ctx = 0,
    };
    uint16_t threshold = 0;
    uint8_t near = 0;
    uint8_t pending = 0;
    uint8_t report = 1; /* power-up event: it says that the threshold is not set */

    DDRB |= _BV(TRIG_BIT);
    PORTB &= (uint8_t)~_BV(TRIG_BIT);
    DDRB &= (uint8_t)~_BV(ECHO_BIT);
    TCCR0B = _BV(CS01) | _BV(CS00); /* timer 0: F_CPU / 64, free running, polled (ticks()) */
    cfg.mcid = hubus_slave_mcid_from_eeprom(0x0E); /* written when flashing (tools/flash-avr.sh), else 14 */
    hubus_slave_init(&cfg);
    sei();

    for (;;) {
        uint16_t distance = measure();
        uint8_t want;
        uint8_t i;

        if (threshold == 0) {
            want = 0;
        } else if (near) {
            want = distance != 0 && distance <= threshold + HYSTERESIS_MM;
        } else {
            want = distance != 0 && distance < threshold;
        }
        if (want != near) {
            if (++pending >= CONFIRM) {
                near = want;
                pending = 0;
                report = 1;
            }
        } else {
            pending = 0;
        }
        publish(near, distance);

        for (i = 0; i < SAMPLE_MS; i++) {
            _delay_ms(1);
            if (cmd_new) {
                threshold = take_threshold();
                pending = 0;
            }
            if (report) {
                uint8_t snapshot[5];
                uint8_t k;

                ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
                {
                    for (k = 0; k < 5; k++) {
                        snapshot[k] = state[k];
                    }
                }
                /* Retry until the write is accepted (it is refused while the buffer is being transmitted). */
                if (hubus_slave_write(snapshot, sizeof snapshot, 0) == HUBUS_WRITE_OK) {
                    report = 0;
                }
            }
        }
    }
}
