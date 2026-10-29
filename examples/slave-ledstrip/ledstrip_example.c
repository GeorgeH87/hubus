/*
 * LED strip and push button slave (ATtiny85 at 8 MHz).
 *
 *   PB2 = CLK, PB3 = DATA (library, see slave/hubus_slave_config.h)
 *   PB0 = WS2812B data input (through a 5 V buffer, docs/example-ledstrip.md)
 *   PB1 = push button to ground (internal pull-up)
 *
 * Build options: HUBUS_ENABLE_EVENTS=1, HUBUS_ENABLE_STATE=1, HUBUS_TX_BUF_SIZE=3.
 * LED_BYTES sets the frame buffer (default 180 bytes: 60 RGB LEDs).
 *
 * The slave keeps a raw frame buffer. The master either fills it and shows it once, or sends a program in the
 * bytecode of ledvm.h that the slave runs for every LED of every frame (an animation):
 *
 *   0x40 WRITE       off_hi off_lo data[1..28]               copy data to buffer offset off
 *   0x41 FILL        off_hi off_lo cnt_hi cnt_lo pat[1..4]   repeat pat over cnt bytes from off
 *   0x42 SHOW        [seq]                                   stop the program, send the buffer to the strip once
 *   0x43 PROG_WRITE  off code[1..29]                         stop the program, copy code to program offset off
 *   0x44 PROG_RUN    len crc layout period                   check and start the program; no arguments: stop
 *   0x45 PARAM       k value[1..8]                           set the parameters k, k+1, ...
 *   0x46 TIME        t_hi t_lo                               set the time of the program
 *
 * Offsets and counts are byte positions; whatever lies beyond a buffer is dropped. WRITE, PROG_WRITE, PARAM, and
 * TIME run in the command callback. FILL and PROG_RUN are too long for the interrupt handler and run in the main
 * loop. SHOW sends the frame in a blind section after the STOP of its transaction (docs/spec/07-slave.md section
 * 7.8): the strip needs interrupts disabled for about 12.5 us per byte; the slave announces the blind class that covers
 * it, and the master does not address the slave during that time.
 *
 * The frames of a program go out in an interruptible section instead (slave/hubus_slave.h): byte by byte, and only
 * while the bus is quiet. An edge on the bus ends the frame after the current byte, so the slave never misses a
 * transaction; the frame is sent again once the bus is between transactions, or the next one is computed. The
 * animation pauses while the master is busy on the bus.
 *
 * State and events, 3 bytes: [0] button (1 = pressed), [1] seq of the last executed SHOW, 0 after power-up, [2]
 * program: 0 none, 1 running, 2 refused by PROG_RUN. A button change writes the three bytes to the event buffer, and
 * so does the power-up: the master learns that the strip is dark and can send the frame or the program again.
 */
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stdint.h>
#include <util/atomic.h>

#include "hubus_crc.h"
#include "hubus_slave.h"
#include "ledvm.h"

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
 * Blind time of a SHOW: about 12.5 us per byte (8 bits of 1.25 us and the loop), plus the reset time of the strip
 * after an earlier frame (at most 512 us, see strip_ready()), the wait for the slave to be free (at most one clock
 * period of an idle event handshake), and margin. The blind class is the smallest whose time (0.25 ms * 2^(class - 1))
 * covers it: class 5 (4 ms) for the default 180 bytes.
 */
#define SHOW_BLIND_US ((uint32_t)LED_BYTES * 25u / 2u + 1500u)
#define BLIND_CLASS_FOR(us) \
    ((us) <= 250u ? 1u : (us) <= 500u ? 2u : (us) <= 1000u ? 3u : (us) <= 2000u ? 4u : (us) <= 4000u ? 5u : 6u)
#define SHOW_BLIND_CLASS BLIND_CLASS_FOR(SHOW_BLIND_US)
#if LED_BYTES * 25 / 2 + 1500 > 8000
#error "a larger strip needs a higher blind class: extend BLIND_CLASS_FOR"
#endif

#define OP_WRITE 0x40
#define OP_FILL 0x41
#define OP_SHOW 0x42
#define OP_PROG_WRITE 0x43
#define OP_PROG_RUN 0x44
#define OP_PARAM 0x45
#define OP_TIME 0x46
#define FILL_PATTERN_MAX 4

#define PROG_NONE 0
#define PROG_RUNNING 1
#define PROG_REFUSED 2

/* SLAVE_INFO application bytes: device type, firmware version, serial number. */
#define DEVICE_TYPE_LEDSTRIP 0x04
static const uint8_t info[] PROGMEM = {DEVICE_TYPE_LEDSTRIP, 0x02, 0x00, 0x00, 0x00, 0x04};

static uint8_t frame[LED_BYTES];
static volatile uint8_t state[3]; /* button, seq of the last SHOW, program */

/* Jobs for the main loop, set by the command callback. */
static volatile uint8_t fill_req;
static uint16_t fill_off, fill_cnt;
static uint8_t fill_pat[FILL_PATTERN_MAX], fill_pat_len;
static volatile uint8_t show_req;
static uint8_t show_seq;
static volatile uint8_t run_req;
static uint8_t run_args[4]; /* len, crc, layout, period */
static volatile uint8_t time_req;
static uint16_t time_value;

/* The program. `running` is cleared by every command that changes the code or the frame on its own; the main loop
 * checks it before each LED and before showing a frame. */
static ledvm_t vm;
static volatile uint8_t running;
static volatile uint8_t code_gen; /* counts the PROG_WRITEs, so that a check of older code does not count */
static uint8_t period;     /* ms per frame */
static uint16_t t0;        /* `now` at program time 0 */
static uint16_t next_at;   /* `now` of the next frame */
static uint8_t frame_new;  /* a computed frame waits to be shown */

/*
 * Milliseconds since power-up. Timer 0 runs freely at F_CPU / 1024 (128 us per count), without an interrupt, which would
 * add to the response time on the bus; tick() adds the counts since its last call. It has to run at least once per
 * timer period (32 ms): in the main loop and after each LED of a frame.
 */
static uint16_t now;
static uint8_t tcnt_last;
static uint8_t ms_rest; /* counts of 1/125 ms not yet added to `now` */

static void tick(void)
{
    uint8_t c = TCNT0;
    uint16_t a;

    if (c == tcnt_last) {
        return;
    }
    a = (uint16_t)(ms_rest + (uint16_t)(uint8_t)(c - tcnt_last) * 16u); /* 128 us = 16/125 ms */
    tcnt_last = c;
    now = (uint16_t)(now + a / 125u);
    ms_rest = (uint8_t)(a % 125u);
}

/*
 * Sends the frame buffer to the strip on PORTB with interrupts disabled, MSB first, 10 cycles per bit at 8 MHz: high for
 * 3 cycles (375 ns) for a 0 and 7 cycles (875 ns) for a 1. Between two bytes the line stays low about 1 us longer, which
 * the WS2812B tolerates (it latches only after a much longer low phase).
 *
 * `yield`: an interruptible section (slave/hubus_slave.h). Before each byte the loop checks the pin change flag of CLK
 * and DATA, the flag hubus_slave_short_interrupted() reads, here inline because a function call would stretch the low
 * phase. Once an edge waits on the bus the frame is given up; returns 0 then, 1 for a complete frame.
 */
static uint16_t out_ms;  /* `now` at the end of the last frame */
static uint8_t out_tcnt; /* TCNT0 at the end of the last frame */

static uint8_t ws2812_send(uint8_t yield)
{
    const uint8_t *p = frame;
    uint16_t n = LED_BYTES;
    uint8_t hi = (uint8_t)(PORTB | _BV(LED_BIT));
    uint8_t lo = (uint8_t)(PORTB & ~_BV(LED_BIT));
    uint8_t mask = yield ? _BV(PCIF) : 0;
    uint8_t b, bits, f;

    __asm__ volatile("1:  in   %[f], %[gifr]   \n\t"
                     "    and  %[f], %[mask]   \n\t"
                     "    brne 3f              \n\t" /* an edge waits on the bus: give up */
                     "    ld   %[b], %a[p]+    \n\t"
                     "    ldi  %[bits], 8      \n\t"
                     "2:  out  %[port], %[hi]  \n\t" /* 0: high */
                     "    nop                  \n\t" /* 1 */
                     "    sbrs %[b], 7         \n\t" /* 2 (2-3 for a 1) */
                     "    out  %[port], %[lo]  \n\t" /* 3: low after 3 cycles for a 0 */
                     "    lsl  %[b]            \n\t" /* 4 */
                     "    dec  %[bits]         \n\t" /* 5 */
                     "    nop                  \n\t" /* 6 */
                     "    out  %[port], %[lo]  \n\t" /* 7: low after 7 cycles for a 1 */
                     "    brne 2b              \n\t" /* 8-9 */
                     "    sbiw %[n], 1         \n\t"
                     "    brne 1b              \n\t"
                     "3:                       \n\t"
                     : [p] "+e"(p), [n] "+w"(n), [b] "=&r"(b), [bits] "=&d"(bits), [f] "=&r"(f)
                     : [port] "I"(_SFR_IO_ADDR(PORTB)), [gifr] "I"(_SFR_IO_ADDR(GIFR)), [hi] "r"(hi), [lo] "r"(lo),
                       [mask] "r"(mask)
                     : "memory");
    out_ms = now;
    out_tcnt = TCNT0;
    return n == 0;
}

/* The strip takes a frame only after a reset: the line low for at least 280 us (WS2812B-V5; older parts 50 us) after
 * the previous one, also after a frame given up. Waits 4 timer counts, 384..512 us. */
static uint8_t strip_ready(void)
{
    return (uint16_t)(now - out_ms) >= 2u || (uint8_t)(TCNT0 - out_tcnt) >= 4u;
}

/* Runs in the interrupt handler: short copies here, the rest is handed to the main loop. */
static void on_command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    uint16_t off;
    uint8_t i;

    (void)ctx;
    switch (opcode) {
    case OP_SHOW:
        running = 0;
        state[2] = PROG_NONE;
        show_seq = len ? args[0] : state[1];
        show_req = 1;
        return;
    case OP_PROG_WRITE:
        running = 0;
        state[2] = PROG_NONE;
        code_gen++;
        if (len >= 2) {
            for (i = 1, off = args[0]; i < len && off < LEDVM_CODE_MAX; i++) {
                vm.code[off++] = args[i];
            }
        }
        return;
    case OP_PROG_RUN:
        running = 0;
        state[2] = PROG_NONE;
        for (i = 0; i < 4; i++) {
            run_args[i] = i < len ? args[i] : 0;
        }
        run_req = 1;
        return;
    case OP_PARAM:
        for (i = 1, off = len ? args[0] : LEDVM_PARAMS; i < len && off < LEDVM_PARAMS; i++) {
            vm.p[off++] = args[i];
        }
        return;
    case OP_TIME:
        if (len >= 2) {
            time_value = (uint16_t)((uint16_t)args[0] << 8 | args[1]);
            time_req = 1;
        }
        return;
    default:
        break;
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
    uint8_t snapshot[3] = {state[0], state[1], state[2]};

    (void)ctx;
    (void)hubus_slave_write(snapshot, 3, 0);
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

/* PROG_RUN: checks the program (length, CRC-8 as in the frames, instructions) and starts it at time 0. */
static void run_program(void)
{
    uint8_t len, crc, layout, gen, ok;

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
        run_req = 0;
        gen = code_gen;
        len = run_args[0];
        crc = run_args[1];
        layout = run_args[2];
        period = run_args[3] ? run_args[3] : 1u;
    }
    if (!len) {
        return; /* stop */
    }
    ok = len <= LEDVM_CODE_MAX && layout <= LEDVM_LAYOUT_MAX && hubus_crc8(vm.code, len) == crc &&
         ledvm_check(vm.code, len);
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
        if (run_req || gen != code_gen) {
            return; /* the code changed during the check, or a newer PROG_RUN waits */
        }
        if (!ok) {
            state[2] = PROG_REFUSED;
            return;
        }
        ledvm_start(&vm, len, layout);
        t0 = now;
        next_at = now;
        frame_new = 0;
        running = 1;
        state[2] = PROG_RUNNING;
    }
}

/* Computes the next frame into the frame buffer. Gives up when the program is stopped meanwhile. */
static void render(void)
{
    uint8_t bpp = ledvm_bpp(vm.layout);
    uint16_t n = LED_BYTES / bpp;
    uint16_t t = (uint16_t)(now - t0);
    uint16_t i;

    for (i = 0; i < n; i++) {
        if (!running) {
            return;
        }
        ledvm_led(&vm, &frame[i * bpp], i, n, t);
        tick();
    }
    frame_new = 1;
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
    uint16_t debounced = 0; /* `now` up to which the button is debounced */
    uint8_t stable = 0;
    uint8_t report = 1; /* power-up event */

    PORTB &= (uint8_t)~_BV(LED_BIT);
    DDRB |= _BV(LED_BIT);
    DDRB &= (uint8_t)~_BV(BUTTON_BIT);
    PORTB |= _BV(BUTTON_BIT); /* pull-up */
    TCCR0B = _BV(CS02) | _BV(CS00); /* timer 0: F_CPU / 1024, free running, polled (tick()) */
    (void)ws2812_send(0); /* dark strip; the bus is not running yet */
    hubus_slave_init(&cfg);
    sei();

    for (;;) {
        tick();
        if (fill_req) {
            run_fill();
        }
        if (run_req) {
            run_program();
        }
        if (time_req) {
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
            {
                time_req = 0;
                t0 = (uint16_t)(now - time_value);
            }
        }
        if (show_req && strip_ready() && hubus_slave_blind_begin()) { /* 0 while busy on the bus: try again */
            show_req = 0;
            (void)ws2812_send(0);
            state[1] = show_seq;
            hubus_slave_blind_end();
        }
        if (running && !frame_new && (int16_t)(now - next_at) >= 0) {
            /* Frames that could not be shown in time are dropped; the program time keeps running. */
            next_at = (int16_t)(now - next_at) >= period ? now + period : next_at + period;
            render();
        }
        if (running && frame_new && strip_ready() && hubus_slave_short_begin()) { /* 0 while the bus is busy */
            if (ws2812_send(1)) {
                frame_new = 0;
            }
            hubus_slave_short_end();
        }
        while (debounced != now) { /* every millisecond */
            uint8_t level = (PINB & _BV(BUTTON_BIT)) ? 0 : 1; /* active low */

            debounced++;
            if (level == state[0]) {
                stable = 0;
            } else if (++stable >= DEBOUNCE_MS) {
                state[0] = level;
                stable = 0;
                report = 1;
            }
        }
        if (report) {
            uint8_t snapshot[3] = {state[0], state[1], state[2]};
            /* Retry until the write is accepted (it is refused while the buffer is being transmitted). */
            if (hubus_slave_write(snapshot, 3, 0) == HUBUS_WRITE_OK) {
                report = 0;
            }
        }
    }
}
