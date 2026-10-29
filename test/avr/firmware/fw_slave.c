/*
 * Test firmware for the AVR simulation tests (test/avr/test_avr_slave.c): the slave library as it runs on an ATtiny85,
 * controlled over the bus.
 *
 *   PB2 = CLK, PB3 = DATA (library defaults, slave/hubus_slave_config.h)
 *
 * Build options: FW_MCID, HUBUS_ENABLE_EVENTS, HUBUS_ENABLE_STATE.
 *
 * State, 2 bytes: [0] value of the last SET, [1] number of finished jobs (EVENT writes and blind sections).
 *
 *   0x10 SET   v            state[0] = v, in the command callback
 *   0x11 EVENT data[1..4]   the main loop writes data to the event buffer
 *   0x12 BLIND n            the main loop runs a blind section of n * 100 us (n = 1..FW_BLIND_MAX)
 *
 * After power-up the main loop writes one event, FW_BOOT_EVENT. STATUS_CAPTURE writes state[0].
 */
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stdint.h>
#include <util/delay.h>

#include "hubus_slave.h"

#ifndef FW_MCID
#error "FW_MCID must be set"
#endif

#define FW_BOOT_EVENT 0xB0
#define FW_EVENT_MAX 4
#define FW_BLIND_MAX 8 /* 0.8 ms plus the wait for the slave to be free: blind class 3 (1 ms) */
#define FW_BLIND_CLASS 3
#define FW_SPEED_CLASS 9 /* 64 us at 8 MHz, checked by test_avr_slave */

#define OP_SET 0x10
#define OP_EVENT 0x11
#define OP_BLIND 0x12

/* SLAVE_INFO application bytes: device type, firmware version, MCID as serial number. */
static const uint8_t info[] PROGMEM = {0x7E, 0x01, FW_MCID};

static volatile uint8_t state[2];

static volatile uint8_t event_req;
static uint8_t event_data[FW_EVENT_MAX], event_len;
static volatile uint8_t blind_req;

static void on_command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    uint8_t i;

    (void)ctx;
    if (opcode == OP_SET && len == 1) {
        state[0] = args[0];
    } else if (opcode == OP_EVENT && len >= 1 && len <= FW_EVENT_MAX && !event_req) {
        for (i = 0; i < len; i++) {
            event_data[i] = args[i];
        }
        event_len = len;
        event_req = 1;
    } else if (opcode == OP_BLIND && len == 1 && args[0] >= 1 && args[0] <= FW_BLIND_MAX) {
        blind_req = args[0];
    }
}

#if HUBUS_ENABLE_EVENTS
static void on_status_capture(void *ctx)
{
    uint8_t v = state[0];

    (void)ctx;
    (void)hubus_slave_write(&v, 1, 0);
}
#endif

int main(void)
{
    static const hubus_slave_config_t cfg = {
        .mcid = FW_MCID,
        .info = info,
        .info_len = sizeof info,
#if HUBUS_ENABLE_STATE
        .state = state,
        .state_len = sizeof state,
#endif
#if HUBUS_ENABLE_EVENTS
        .on_status_capture = on_status_capture,
#endif
        .on_command = on_command,
        .ctx = 0,
        .speed_class = FW_SPEED_CLASS,
        .blind_class = FW_BLIND_CLASS,
    };

    hubus_slave_init(&cfg);
    sei();
#if HUBUS_ENABLE_EVENTS
    {
        static const uint8_t boot = FW_BOOT_EVENT;

        (void)hubus_slave_write(&boot, 1, 0);
    }
#endif
    for (;;) {
#if HUBUS_ENABLE_EVENTS
        if (event_req && hubus_slave_write(event_data, event_len, 0) == HUBUS_WRITE_OK) {
            event_req = 0;
            state[1]++;
        }
#endif
        if (blind_req) {
            uint8_t n = blind_req;

            while (!hubus_slave_blind_begin()) {
            }
            while (n--) {
                _delay_us(100);
            }
            hubus_slave_blind_end();
            blind_req = 0;
            state[1]++;
        }
    }
}
