/*
 * Reference firmware for the flash/RAM budget: uses the complete slave API
 * of the configured build (application command, SLAVE_INFO from flash; with
 * HUBUS_ENABLE_EVENTS status capture and buffered writes with fragments; with
 * HUBUS_ENABLE_STATE the output exposed as live state). The budget check in
 * cmake/check-size.cmake runs on this image.
 */
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>

#include "hubus_slave.h"

static const uint8_t info[] PROGMEM = {0x01, 0x00, 0xA5, 0x5A};

static volatile uint8_t output_state;

#if HUBUS_ENABLE_EVENTS
static void on_status_capture(void *ctx)
{
    uint8_t v = output_state;

    (void)ctx;
    (void)hubus_slave_write(&v, 1, 0);
}
#endif

static void on_command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    (void)ctx;
    if (opcode == 0x10 && len >= 1) {
        output_state = args[0];
    }
}

int main(void)
{
    static const hubus_slave_config_t cfg = {
        .mcid = 5,
        .info = info,
        .info_len = sizeof info,
#if HUBUS_ENABLE_STATE
        .state = &output_state,
        .state_len = 1,
#endif
#if HUBUS_ENABLE_EVENTS
        .on_status_capture = on_status_capture,
#endif
        .on_command = on_command,
        .ctx = 0,
    };
#if HUBUS_ENABLE_EVENTS
    uint8_t fragment[2] = {0, 0};
#endif

    hubus_slave_init(&cfg);
    sei();
    for (;;) {
#if HUBUS_ENABLE_EVENTS
        if (!hubus_slave_is_pending()) {
            fragment[0]++;
            (void)hubus_slave_write(fragment, sizeof fragment, (uint8_t)(fragment[0] & 1u));
        }
#endif
    }
}
