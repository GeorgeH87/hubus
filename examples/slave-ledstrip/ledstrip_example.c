/*
 * LED strip and push button slave (ATtiny85 at 8 MHz).
 *
 *   PB2 = CLK, PB3 = DATA (library, see slave/hubus_slave_config.h)
 *   PB0 = WS2812B data input (through a 5 V buffer, docs/example-ledstrip.md)
 *   PB1 = push button to ground (internal pull-up)
 *
 * Build options: HUBUS_ENABLE_EVENTS=1, HUBUS_ENABLE_STATE=1, HUBUS_TX_BUF_SIZE=2.
 * LED_BYTES sets the frame buffer (default 180 bytes: 60 RGB LEDs).
 *
 * The slave keeps a raw frame buffer and knows no colours: the master sends
 * the bytes in the order the strip expects (GRB for the WS2812B, GRBW for an
 * SK6812 RGBW strip). The strip cannot be read back.
 *
 *   0x40 WRITE  off_hi off_lo data[1..28]               copy data to buffer offset off
 *   0x41 FILL   off_hi off_lo cnt_hi cnt_lo pat[1..4]   repeat pat over cnt bytes from off
 *   0x42 SHOW   [seq]                                   send the buffer to the strip
 *
 * Offsets and counts are byte positions; whatever lies beyond the buffer is
 * dropped. WRITE runs in the command callback. FILL and SHOW are too long for
 * the interrupt handler and run in the main loop: FILL at once (about 1 us per
 * byte, done long before the next command frame is complete), SHOW in a blind
 * section after the STOP of its transaction (docs/spec/07-slave.md section
 * 7.8). The strip needs interrupts disabled for about 11 us per byte; the
 * slave announces the blind class that covers it, and the master does not
 * address the slave during that time. The rest of the bus goes on.
 *
 * State and events, 2 bytes: [0] button (1 = pressed), [1] seq of the last
 * executed SHOW, 0 after power-up. A button change writes both bytes to the
 * event buffer, and so does the power-up: the master learns that the strip
 * is dark and can send the frame again.
 */
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stdint.h>
#include <util/atomic.h>

#include "hubus_slave.h"

#if F_CPU != 8000000UL
#error "The WS2812B timing below needs F_CPU = 8 MHz (clear the CKDIV8 fuse)"
#endif

#ifndef LED_BYTES
#define LED_BYTES 180
#endif

#define LED_BIT PB0
#define BUTTON_BIT PB1
#define DEBOUNCE_MS 10

/*
 * Blind time of a SHOW: about 11 us per byte (8 bits of 1.25 us and the loop), plus the wait for the slave to be free
 * (at most one clock period of an idle event handshake) and margin. The blind class is the smallest whose time
 * (0.25 ms * 2^(class - 1)) covers it: class 5 (4 ms) for the default 180 bytes.
 */
#define SHOW_BLIND_US ((uint32_t)LED_BYTES * 11u + 1000u)
#define BLIND_CLASS_FOR(us) \
    ((us) <= 250u ? 1u : (us) <= 500u ? 2u : (us) <= 1000u ? 3u : (us) <= 2000u ? 4u : (us) <= 4000u ? 5u : 6u)
#define SHOW_BLIND_CLASS BLIND_CLASS_FOR(SHOW_BLIND_US)
#if LED_BYTES * 11 + 1000 > 8000
#error "a larger strip needs a higher blind class: extend BLIND_CLASS_FOR"
#endif

#define OP_WRITE 0x40
#define OP_FILL 0x41
#define OP_SHOW 0x42
#define FILL_PATTERN_MAX 4

/* SLAVE_INFO application bytes: device type, firmware version, serial number. */
#define DEVICE_TYPE_LEDSTRIP 0x04
static const uint8_t info[] PROGMEM = {DEVICE_TYPE_LEDSTRIP, 0x01, 0x00, 0x00, 0x00, 0x04};

static uint8_t frame[LED_BYTES];
static volatile uint8_t state[2]; /* button, seq of the last SHOW */

/* Jobs for the main loop, set by the command callback. */
static volatile uint8_t fill_req;
static uint16_t fill_off, fill_cnt;
static uint8_t fill_pat[FILL_PATTERN_MAX], fill_pat_len;
static volatile uint8_t show_req;
static uint8_t show_seq;

/*
 * Sends n > 0 bytes to the strip on PORTB bit `mask`, MSB first, 10 cycles
 * per bit at 8 MHz: high for 3 cycles (375 ns) for a 0 and 7 cycles (875 ns)
 * for a 1. The low phase between bytes is about 1 us longer, which the
 * WS2812B tolerates. Call with interrupts disabled: before sei() at power-up,
 * and in the blind section of a SHOW.
 */
static void ws2812_send(const uint8_t *p, uint16_t n, uint8_t mask)
{
    uint8_t b, bits;
    uint8_t hi = PORTB | mask;
    uint8_t lo = PORTB & (uint8_t)~mask;

    __asm__ volatile("1:  ld   %[b], %a[p]+   \n\t"
                     "    ldi  %[bits], 8     \n\t"
                     "2:  out  %[port], %[hi] \n\t" /* 0: high */
                     "    nop                 \n\t" /* 1 */
                     "    sbrs %[b], 7        \n\t" /* 2 (2-3 for a 1) */
                     "    out  %[port], %[lo] \n\t" /* 3: low after 3 cycles for a 0 */
                     "    lsl  %[b]           \n\t" /* 4 */
                     "    dec  %[bits]        \n\t" /* 5 */
                     "    nop                 \n\t" /* 6 */
                     "    out  %[port], %[lo] \n\t" /* 7: low after 7 cycles for a 1 */
                     "    brne 2b             \n\t" /* 8-9 */
                     "    sbiw %[n], 1        \n\t"
                     "    brne 1b             \n\t"
                     : [p] "+e"(p), [n] "+w"(n), [b] "=&r"(b), [bits] "=&d"(bits)
                     : [port] "I"(_SFR_IO_ADDR(PORTB)), [hi] "r"(hi), [lo] "r"(lo)
                     : "memory");
}

/* Runs in the interrupt handler: WRITE is a short copy, FILL and SHOW are handed to the main loop. */
static void on_command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    uint16_t off;
    uint8_t i;

    (void)ctx;
    if (opcode == OP_SHOW) {
        show_seq = len ? args[0] : state[1];
        show_req = 1;
        return;
    }
    if (len < 2) {
        return;
    }
    off = (uint16_t)((uint16_t)args[0] << 8 | args[1]);
    if (opcode == OP_WRITE) {
        for (i = 2; i < len && off < LED_BYTES; i++) {
            frame[off++] = args[i];
        }
    } else if (opcode == OP_FILL && len >= 5 && len <= 4 + FILL_PATTERN_MAX) {
        fill_off = off;
        fill_cnt = (uint16_t)((uint16_t)args[2] << 8 | args[3]);
        fill_pat_len = (uint8_t)(len - 4);
        for (i = 0; i < fill_pat_len; i++) {
            fill_pat[i] = args[4 + i];
        }
        fill_req = 1;
    }
}

static void on_status_capture(void *ctx)
{
    uint8_t snapshot[2] = {state[0], state[1]};

    (void)ctx;
    (void)hubus_slave_write(snapshot, 2, 0);
}

static void run_fill(void)
{
    uint16_t off, end;
    uint8_t pat[FILL_PATTERN_MAX], pat_len, i, k = 0;

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
        fill_req = 0;
        off = fill_off;
        end = fill_cnt > LED_BYTES ? LED_BYTES : fill_cnt; /* count first, so off + count cannot overflow */
        end = off < LED_BYTES - end ? off + end : LED_BYTES;
        pat_len = fill_pat_len;
        for (i = 0; i < pat_len; i++) {
            pat[i] = fill_pat[i];
        }
    }
    while (off < end) {
        frame[off++] = pat[k];
        if (++k == pat_len) {
            k = 0;
        }
    }
}

int main(void)
{
    static const hubus_slave_config_t cfg = {
        .mcid = 0x0B,
        .info = info,
        .info_len = sizeof info,
        .state = state,
        .state_len = sizeof state,
        .on_status_capture = on_status_capture,
        .on_command = on_command,
        .ctx = 0,
        .blind_class = SHOW_BLIND_CLASS,
    };
    uint8_t stable = 0;
    uint8_t report = 1; /* power-up event */

    PORTB &= (uint8_t)~_BV(LED_BIT);
    DDRB |= _BV(LED_BIT);
    DDRB &= (uint8_t)~_BV(BUTTON_BIT);
    PORTB |= _BV(BUTTON_BIT); /* pull-up */
    OCR0A = F_CPU / 64 / 1000 - 1; /* timer 0: 1 ms period, polled, no interrupt */
    TCCR0A = _BV(WGM01);
    TCCR0B = _BV(CS01) | _BV(CS00);
    ws2812_send(frame, LED_BYTES, _BV(LED_BIT)); /* dark strip; the bus is not running yet */
    hubus_slave_init(&cfg);
    sei();

    for (;;) {
        if (fill_req) {
            run_fill();
        }
        if (show_req && hubus_slave_blind_begin()) { /* 0 while the slave is busy on the bus: try again */
            show_req = 0;
            ws2812_send(frame, LED_BYTES, _BV(LED_BIT));
            state[1] = show_seq;
            hubus_slave_blind_end();
        }
        if (TIFR & _BV(OCF0A)) { /* every millisecond */
            uint8_t now = (PINB & _BV(BUTTON_BIT)) ? 0 : 1; /* active low */

            TIFR = _BV(OCF0A);
            if (now == state[0]) {
                stable = 0;
            } else if (++stable >= DEBOUNCE_MS) {
                state[0] = now;
                stable = 0;
                report = 1;
            }
        }
        if (report) {
            uint8_t snapshot[2] = {state[0], state[1]};
            /* Retry until the write is accepted (it is refused while the buffer is being transmitted). */
            if (hubus_slave_write(snapshot, 2, 0) == HUBUS_WRITE_OK) {
                report = 0;
            }
        }
    }
}
