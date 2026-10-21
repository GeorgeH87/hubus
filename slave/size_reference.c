/*
 * Reference firmware for the flash/RAM budget: uses the complete slave API
 * (status capture, application command, buffered writes with fragments,
 * SLAVE_INFO from flash). The budget check in cmake/check-size.cmake runs on
 * this image.
 */
#include <avr/io.h>
#include <avr/pgmspace.h>

#include "hubus_slave.h"

static const uint8_t info[] PROGMEM = {0x01, 0x00, 0xA5, 0x5A};

static uint8_t output_state;

static void on_status_capture(void *ctx)
{
    (void)ctx;
    (void)hubus_slave_write(&output_state, 1, 0);
}

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
        .on_status_capture = on_status_capture,
        .on_command = on_command,
        .ctx = 0,
    };
    uint8_t fragment[2] = {0, 0};

    hubus_slave_init(&cfg);
    for (;;) {
        if (!hubus_slave_is_pending()) {
            fragment[0]++;
            (void)hubus_slave_write(fragment, sizeof fragment, (uint8_t)(fragment[0] & 1u));
        }
    }
}
