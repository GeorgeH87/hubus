/*
 * The LED strip example (examples/slave-ledstrip) as it runs on an ATtiny85, simulated by simavr against the real
 * master: the WS2812B signal on PB0 is decoded bit by bit, so the tests see the frames the strip would show. Covers
 * the fixed frame (SHOW in a blind section), the animation bytecode (PROG_WRITE, PROG_RUN, PARAM, TIME), and the
 * interruptible section: animation frames give way to the bus, and no transaction is lost meanwhile.
 */
#include <stdio.h>
#include <string.h>

#include "avr_ioport.h"
#include "avr_slave.h"
#include "fixture.h"
#include "hubus_crc.h"
#include "ledvm.h"
#include "sim_avr.h"

#define MCU "attiny85"
#define F_CPU_HZ 8000000u
#define FW(name) HUBUS_AVR_FW_DIR "/" name ".elf"
#define NS_PER_CYCLE (1000000000u / F_CPU_HZ)

#define STRIP 11u
#define LED_BYTES 180u
#define BLIND_CLASS 5u
#define BLIND_NS (HUBUS_BLIND_US(BLIND_CLASS) * 1000u)
#define BOOT_NS 5000000u /* C runtime start, the dark frame, hubus_slave_init(), the power-up event */

#define OP_SET 0x10 /* test firmware */
#define OP_FILL 0x41
#define OP_SHOW 0x42
#define OP_PROG_WRITE 0x43
#define OP_PROG_RUN 0x44
#define OP_PARAM 0x45
#define OP_TIME 0x46

/* WS2812B: a high phase of at least 5 cycles (625 ns) is a 1; a low phase of 50 us or more ends a frame. */
#define WS_ONE_CYCLES 5u
#define WS_RESET_CYCLES (50000u / NS_PER_CYCLE)

typedef struct {
    avr_t *avr;
    uint64_t rise, fall;
    uint8_t high;
    uint8_t cur, nbits;
    uint8_t bytes[LED_BYTES + 8];
    unsigned n;
    uint8_t last[LED_BYTES]; /* last complete frame */
    unsigned frames;         /* complete frames */
    unsigned partial;        /* frames given up before their end */
    uint32_t high0_max, high1_min, gap_max; /* cycles */
} ws_t;

static fx_t fx;
static avr_bus_t bus;
static ws_t ws;

static void ws_end_frame(ws_t *w)
{
    if (w->n == LED_BYTES && w->nbits == 0) {
        memcpy(w->last, w->bytes, LED_BYTES);
        w->frames++;
    } else {
        w->partial++;
    }
    w->n = 0;
    w->nbits = 0;
}

static void ws_pin(struct avr_irq_t *irq, uint32_t value, void *param)
{
    ws_t *w = param;
    uint64_t c = w->avr->cycle;

    (void)irq;
    if (!value == !w->high) {
        return; /* no change of the level */
    }
    w->high = value ? 1u : 0u;
    if (value) {
        if (w->n || w->nbits) {
            uint64_t gap = c - w->fall;

            if (gap >= WS_RESET_CYCLES) {
                ws_end_frame(w);
            } else if (gap > w->gap_max) {
                w->gap_max = (uint32_t)gap;
            }
        }
        w->rise = c;
        return;
    }
    {
        uint32_t high = (uint32_t)(c - w->rise);
        uint8_t bit = high >= WS_ONE_CYCLES;

        if (bit && high < w->high1_min) {
            w->high1_min = high;
        }
        if (!bit && high > w->high0_max) {
            w->high0_max = high;
        }
        w->fall = c;
        w->cur = (uint8_t)(w->cur << 1 | bit);
        if (++w->nbits == 8) {
            w->nbits = 0;
            if (w->n < sizeof w->bytes) {
                w->bytes[w->n++] = w->cur;
            }
        }
    }
}

/* Ends a frame whose reset time has passed (the decoder only sees edges). */
static void ws_settle(ws_t *w)
{
    if ((w->n || w->nbits) && w->avr->cycle - w->fall >= WS_RESET_CYCLES) {
        ws_end_frame(w);
    }
}

void setUp(void)
{
    avr_slave_t *d;

    fx_init(&fx);
    avr_bus_init(&bus, &fx.sim);
    memset(&ws, 0, sizeof ws);
    ws.high1_min = 0xFFFFFFFFu;
    d = avr_bus_add(&bus, FW("fw_ledstrip"), MCU, F_CPU_HZ);
    ws.avr = d->avr;
    avr_irq_register_notify(avr_io_getirq(d->avr, AVR_IOCTL_IOPORT_GETIRQ('B'), 0), ws_pin, &ws);
}

void tearDown(void)
{
    fx_check(&fx);
    avr_bus_free(&bus);
}

static void idle(uint64_t ns)
{
    avr_bus_idle(&bus, ns);
    ws_settle(&ws);
}

static void boot(void)
{
    idle(BOOT_NS);
    fx_startup(&fx);
}

/* A command to the strip, followed by its blind time, as the master keeps it (docs/spec/06-operation.md section 6.9). */
static void strip_command(uint8_t op, const uint8_t *args, uint8_t len)
{
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, STRIP, op, args, len));
    idle(BLIND_NS);
}

static void strip_state(uint8_t out[3])
{
    hubus_slave_frame_t f;

    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_state(&fx.master, STRIP, &f));
    TEST_ASSERT_EQUAL_UINT8(STRIP, f.mcid);
    TEST_ASSERT_EQUAL_UINT8(3, f.len);
    memcpy(out, f.payload, 3);
}

static void params(uint8_t k, const uint8_t *values, uint8_t n)
{
    uint8_t args[1 + LEDVM_PARAMS];

    args[0] = k;
    memcpy(&args[1], values, n);
    strip_command(OP_PARAM, args, (uint8_t)(1 + n));
}

/* PROG_WRITE in pieces, then PROG_RUN with the CRC (`crc_xor` != 0 spoils it). */
static void program(const uint8_t *code, uint8_t len, uint8_t layout, uint8_t period, uint8_t crc_xor)
{
    uint8_t args[29];
    uint8_t off;

    for (off = 0; off < len; off = (uint8_t)(off + 28u)) {
        uint8_t n = (uint8_t)(len - off > 28 ? 28 : len - off);

        args[0] = off;
        memcpy(&args[1], &code[off], n);
        strip_command(OP_PROG_WRITE, args, (uint8_t)(1 + n));
    }
    args[0] = len;
    args[1] = (uint8_t)(hubus_crc8(code, len) ^ crc_xor);
    args[2] = layout;
    args[3] = period;
    strip_command(OP_PROG_RUN, args, 4);
}

static void report_signal(const char *what)
{
    printf("%s: %u frames, %u given up; WS2812B high 0: %u ns, high 1: %u ns, longest low inside a frame: %u ns\n", what,
           ws.frames, ws.partial, (unsigned)(ws.high0_max * NS_PER_CYCLE), (unsigned)(ws.high1_min * NS_PER_CYCLE),
           (unsigned)(ws.gap_max * NS_PER_CYCLE));
}

/* The WS2812B timing (datasheet: T0H 220..380 ns, T1H 580..1000 ns, low phases well below the reset time). */
static void check_signal(void)
{
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(380u / NS_PER_CYCLE, ws.high0_max);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(580u / NS_PER_CYCLE + 1u, ws.high1_min);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(5000u / NS_PER_CYCLE, ws.gap_max);
}

/* ---- fixed frame ------------------------------------------------------------------------------------------------- */

static void test_power_up_frame_and_event(void)
{
    hubus_burst_stats_t st;
    uint8_t event = 0;
    uint8_t zero[LED_BYTES] = {0};

    boot();
    TEST_ASSERT_EQUAL_UINT(1, ws.frames); /* the dark strip */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(zero, ws.last, LED_BYTES);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_check_event(&fx.master, &event));
    TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
    TEST_ASSERT_EQUAL_UINT(1, fx.n_log);
    TEST_ASSERT_EQUAL_UINT8(STRIP, fx.log[0].mcid);
    TEST_ASSERT_EQUAL_UINT8(3, fx.log[0].len);
}

static void test_show_sends_the_frame_once(void)
{
    static const uint8_t fill[] = {0, 0, 0, LED_BYTES, 0x11, 0x22, 0x33};
    const uint8_t seq = 7;
    uint8_t expect[LED_BYTES];
    uint8_t s[3];
    unsigned k;

    boot();
    strip_command(OP_FILL, fill, sizeof fill);
    strip_command(OP_SHOW, &seq, 1);
    idle(20000000u);
    for (k = 0; k < LED_BYTES; k++) {
        expect[k] = (uint8_t)(0x11 * (k % 3 + 1));
    }
    TEST_ASSERT_EQUAL_UINT(2, ws.frames);
    TEST_ASSERT_EQUAL_UINT(0, ws.partial);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, ws.last, LED_BYTES);
    strip_state(s);
    TEST_ASSERT_EQUAL_UINT8(0, s[0]);
    TEST_ASSERT_EQUAL_UINT8(seq, s[1]);
    TEST_ASSERT_EQUAL_UINT8(0, s[2]);
    report_signal("SHOW");
    check_signal();
}

/* ---- animation --------------------------------------------------------------------------------------------------- */

/* rgb(i + 10, p0, p1) */
static const uint8_t prog_index[] = {LEDVM_I, LEDVM_K8, 10, LEDVM_ADD, LEDVM_P(0), LEDVM_P(1), LEDVM_RGB};

static void expect_index_frame(uint8_t p0, uint8_t p1)
{
    unsigned i;

    for (i = 0; i < LED_BYTES / 3u; i++) { /* GRB */
        TEST_ASSERT_EQUAL_HEX8(p0, ws.last[3 * i]);
        TEST_ASSERT_EQUAL_HEX8(i + 10u, ws.last[3 * i + 1]);
        TEST_ASSERT_EQUAL_HEX8(p1, ws.last[3 * i + 2]);
    }
}

static void test_program_runs_on_the_strip(void)
{
    static const uint8_t p[] = {0x20, 0x30};
    uint8_t s[3];
    unsigned before;

    boot();
    params(0, p, 2);
    program(prog_index, sizeof prog_index, LEDVM_LAYOUT_GRB, 10, 0);
    strip_state(s);
    TEST_ASSERT_EQUAL_UINT8(1, s[2]);
    before = ws.frames;
    idle(100000000u); /* 100 ms at 10 ms per frame */
    TEST_ASSERT_UINT_WITHIN(2, before + 10u, ws.frames);
    expect_index_frame(0x20, 0x30);
    report_signal("program, quiet bus");
    check_signal();
}

/* R = T >> 4: the program time runs from PROG_RUN on, and TIME sets it. */
static void test_program_time(void)
{
    static const uint8_t prog_time[] = {LEDVM_T, LEDVM_K8, 4, LEDVM_SHR, LEDVM_K8, 0, LEDVM_K8, 0, LEDVM_RGB};
    static const uint8_t t[] = {0x0F, 0x00}; /* 3840 ms: R = 240 */

    boot();
    program(prog_time, sizeof prog_time, LEDVM_LAYOUT_GRB, 5, 0);
    idle(150000000u);
    TEST_ASSERT_UINT8_WITHIN(2, 160u / 16u, ws.last[1]); /* about 160 ms after the start (blind time included) */
    strip_command(OP_TIME, t, 2);
    idle(20000000u);
    TEST_ASSERT_UINT8_WITHIN(2, (3840u + 24u) / 16u, ws.last[1]);
}

static void test_refused_programs(void)
{
    static const uint8_t no_output[] = {LEDVM_I, LEDVM_K8, 1, LEDVM_ADD};
    uint8_t s[3];
    unsigned before;

    boot();
    before = ws.frames;
    program(prog_index, sizeof prog_index, LEDVM_LAYOUT_GRB, 10, 0x01); /* wrong CRC */
    strip_state(s);
    TEST_ASSERT_EQUAL_UINT8(2, s[2]);
    program(no_output, sizeof no_output, LEDVM_LAYOUT_GRB, 10, 0);
    strip_state(s);
    TEST_ASSERT_EQUAL_UINT8(2, s[2]);
    program(prog_index, sizeof prog_index, 4, 10, 0); /* unknown layout */
    strip_state(s);
    TEST_ASSERT_EQUAL_UINT8(2, s[2]);
    idle(30000000u);
    TEST_ASSERT_EQUAL_UINT(before, ws.frames);
}

static void test_show_and_prog_run_without_arguments_stop_the_program(void)
{
    const uint8_t seq = 3;
    uint8_t s[3];
    unsigned frames;

    boot(); /* the power-up event stays unread: the SHOW must not wait for its announcement */
    program(prog_index, sizeof prog_index, LEDVM_LAYOUT_GRB, 5, 0);
    idle(30000000u);
    strip_command(OP_SHOW, &seq, 1);
    idle(5000000u);
    strip_state(s);
    TEST_ASSERT_EQUAL_UINT8(seq, s[1]);
    TEST_ASSERT_EQUAL_UINT8(0, s[2]);
    frames = ws.frames;
    idle(30000000u);
    TEST_ASSERT_EQUAL_UINT(frames, ws.frames);

    program(prog_index, sizeof prog_index, LEDVM_LAYOUT_GRB, 5, 0);
    idle(30000000u);
    strip_command(OP_PROG_RUN, NULL, 0);
    strip_state(s);
    TEST_ASSERT_EQUAL_UINT8(0, s[2]);
    frames = ws.frames;
    idle(30000000u);
    TEST_ASSERT_EQUAL_UINT(frames, ws.frames);
}

/* Traffic on the bus all the time, to the strip and to another device: every transaction succeeds, frames are given
 * up instead, and the strip still shows the latest parameters once the bus is quiet. The response time stays within
 * one byte of the strip plus the interrupt handler. */
static void test_animation_gives_way_to_the_bus(void)
{
    avr_slave_t *other;
    avr_slave_t *strip = &bus.dev[0];
    hubus_slave_frame_t f;
    uint8_t p[2] = {0x40, 0x50};
    uint8_t s[3];
    unsigned round;

    other = avr_bus_add(&bus, FW("fw_slave_5"), MCU, F_CPU_HZ);
    boot();
    /* The power-up announcement of the other device reaches the strip before its library runs; the response time
     * counts from here on. */
    strip->response_max_cycles = 0;
    program(prog_index, sizeof prog_index, LEDVM_LAYOUT_GRB, 1, 0);
    for (round = 0; round < 300u; round++) {
        uint8_t v = (uint8_t)round;

        strip_state(s);
        TEST_ASSERT_EQUAL_UINT8(1, s[2]);
        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, 5, OP_SET, &v, 1));
        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_state(&fx.master, 5, &f));
        TEST_ASSERT_EQUAL_HEX8(v, f.payload[0]);
        if (round % 50u == 49u) {
            p[1]++;
            params(0, p, 2);
        }
    }
    idle(10000000u);
    expect_index_frame(p[0], p[1]);
    report_signal("program, busy bus");
    printf("strip: longest ISR %u ns, longest response %u ns, stack %u bytes; other device: longest response %u ns\n",
           (unsigned)(strip->isr_max_cycles * NS_PER_CYCLE), (unsigned)(strip->response_max_cycles * NS_PER_CYCLE),
           avr_slave_stack_depth(strip), (unsigned)(other->response_max_cycles * NS_PER_CYCLE));
    TEST_ASSERT_TRUE(ws.partial > 0u);
    check_signal();
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(64000u / NS_PER_CYCLE, strip->response_max_cycles);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_power_up_frame_and_event);
    RUN_TEST(test_show_sends_the_frame_once);
    RUN_TEST(test_program_runs_on_the_strip);
    RUN_TEST(test_program_time);
    RUN_TEST(test_refused_programs);
    RUN_TEST(test_show_and_prog_run_without_arguments_stop_the_program);
    RUN_TEST(test_animation_gives_way_to_the_bus);
    return UNITY_END();
}
