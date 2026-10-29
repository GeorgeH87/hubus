/*
 * The animation bytecode of the LED strip example (examples/slave-ledstrip/ledvm.h): the program check, the
 * instructions and their 16-bit semantics, the outputs and layouts, and robustness against arbitrary code.
 */
#include <stdlib.h>
#include <string.h>

#include "hubus_crc.h"
#include "ledvm.h"
#include "unity.h"

static ledvm_t vm;

void setUp(void)
{
    memset(&vm, 0, sizeof vm);
}

void tearDown(void) {}

/* Loads and checks a program; fails the test if the check refuses it. */
static void load(const uint8_t *code, uint8_t len, uint8_t layout)
{
    TEST_ASSERT_TRUE_MESSAGE(ledvm_check(code, len), "program refused");
    memcpy(vm.code, code, len);
    ledvm_start(&vm, len, layout);
}

/* Evaluates the expression `expr` (one value on the stack) and asserts that it equals `expected` (16 bits). */
static void expect_value(const uint8_t *expr, uint8_t len, int16_t expected)
{
    uint8_t code[LEDVM_CODE_MAX];
    uint8_t led[3] = {0xAA, 0xAA, 0xAA};
    uint16_t e = (uint16_t)expected;
    uint8_t k = 0;

    memcpy(code, expr, len);
    k = len;
    code[k++] = LEDVM_K16;
    code[k++] = (uint8_t)(e & 0xFFu);
    code[k++] = (uint8_t)(e >> 8);
    code[k++] = LEDVM_EQ;
    code[k++] = LEDVM_K8;
    code[k++] = 0;
    code[k++] = LEDVM_K8;
    code[k++] = 0;
    code[k++] = LEDVM_RGB;
    load(code, k, LEDVM_LAYOUT_RGB);
    ledvm_led(&vm, led, 0, 1, 0);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, led[0], "value differs");
}

#define EXPECT(expected, ...)                                \
    do {                                                     \
        static const uint8_t expr_[] = {__VA_ARGS__};        \
        expect_value(expr_, sizeof expr_, (int16_t)(expected)); \
    } while (0)

#define K16(v) LEDVM_K16, (uint8_t)((uint16_t)(v) & 0xFFu), (uint8_t)((uint16_t)(v) >> 8)
#define K8(v) LEDVM_K8, (v)

/* ---- check --------------------------------------------------------------------------------------------------------- */

static void test_check_accepts_a_complete_expression(void)
{
    static const uint8_t ok[] = {LEDVM_I, K8(4), LEDVM_MUL, LEDVM_T, K8(3), LEDVM_SHR, LEDVM_ADD,
                                 K8(255), K8(128), LEDVM_HSV};

    TEST_ASSERT_TRUE(ledvm_check(ok, sizeof ok));
}

static void test_check_refuses_broken_programs(void)
{
    static const uint8_t no_output[] = {K8(1)};
    static const uint8_t output_not_last[] = {K8(1), K8(2), K8(3), LEDVM_RGB, K8(1)};
    static const uint8_t underflow[] = {K8(1), K8(2), LEDVM_RGB};
    static const uint8_t left_over[] = {K8(9), K8(1), K8(2), K8(3), LEDVM_RGB};
    static const uint8_t overflow[] = {K8(1), K8(1), K8(1), K8(1), K8(1), K8(1), K8(1), K8(1), K8(1), LEDVM_RGB};
    static const uint8_t truncated[] = {K8(1), K8(2), LEDVM_K16, 0x01};
    static const uint8_t unknown[] = {K8(1), K8(2), K8(3), 0x3F};
    static const uint8_t gap[] = {K8(1), 0x1D, K8(3), LEDVM_RGB};
    static const uint8_t two_outputs[] = {K8(1), K8(2), K8(3), LEDVM_RGB, K8(1), K8(2), K8(3), LEDVM_RGB};
    uint8_t big[LEDVM_CODE_MAX + 1];

    TEST_ASSERT_FALSE(ledvm_check(no_output, 0));
    TEST_ASSERT_FALSE(ledvm_check(no_output, sizeof no_output));
    TEST_ASSERT_FALSE(ledvm_check(output_not_last, sizeof output_not_last));
    TEST_ASSERT_FALSE(ledvm_check(underflow, sizeof underflow));
    TEST_ASSERT_FALSE(ledvm_check(left_over, sizeof left_over));
    TEST_ASSERT_FALSE(ledvm_check(overflow, sizeof overflow));
    TEST_ASSERT_FALSE(ledvm_check(truncated, sizeof truncated));
    TEST_ASSERT_FALSE(ledvm_check(unknown, sizeof unknown));
    TEST_ASSERT_FALSE(ledvm_check(gap, sizeof gap));
    TEST_ASSERT_FALSE(ledvm_check(two_outputs, sizeof two_outputs));
    memset(big, LEDVM_DROP, sizeof big);
    TEST_ASSERT_FALSE(ledvm_check(big, sizeof big));
}

static void test_check_allows_the_full_stack(void)
{
    static const uint8_t full[] = {K8(1), K8(1), K8(1), K8(1), K8(1), K8(1), K8(1), K8(1), LEDVM_ADD, LEDVM_ADD,
                                   LEDVM_ADD, LEDVM_ADD, LEDVM_ADD, LEDVM_RGB};

    TEST_ASSERT_TRUE(ledvm_check(full, sizeof full));
}

/* ---- instructions -------------------------------------------------------------------------------------------------- */

static void test_arithmetic_wraps_at_16_bits(void)
{
    EXPECT(-32768, K16(0x7FFF), K8(1), LEDVM_ADD);
    EXPECT(-2, K8(3), K8(5), LEDVM_SUB);
    EXPECT(24464, K16(300), K16(300), LEDVM_MUL); /* 90000 mod 65536 */
    EXPECT(-6, K8(3), K16(-2), LEDVM_MUL);
    EXPECT(-3, K16(-7), K8(2), LEDVM_DIV);
    EXPECT(-1, K16(-7), K8(3), LEDVM_MOD);
    EXPECT(-32768, K16(-32768), K16(-1), LEDVM_DIV);
    EXPECT(0, K16(-32768), K16(-1), LEDVM_MOD);
}

static void test_division_by_zero_gives_zero(void)
{
    EXPECT(0, K8(7), K8(0), LEDVM_DIV);
    EXPECT(0, K8(7), K8(0), LEDVM_MOD);
}

static void test_bits_and_shifts(void)
{
    EXPECT(0x0C, K8(0x0E), K8(0x3C), LEDVM_AND);
    EXPECT(0x3E, K8(0x0E), K8(0x3C), LEDVM_OR);
    EXPECT(0x32, K8(0x0E), K8(0x3C), LEDVM_XOR);
    EXPECT(-32768, K8(1), K8(15), LEDVM_SHL);
    EXPECT(1, K8(1), K8(16), LEDVM_SHL); /* count & 15 */
    EXPECT(15, K16(-1), K8(12), LEDVM_SHR); /* logical */
}

static void test_comparisons_are_signed(void)
{
    EXPECT(1, K16(-1), K8(1), LEDVM_LT);
    EXPECT(0, K16(-1), K8(1), LEDVM_GT);
    EXPECT(1, K8(5), K8(5), LEDVM_EQ);
    EXPECT(-1, K16(-1), K8(1), LEDVM_MIN);
    EXPECT(1, K16(-1), K8(1), LEDVM_MAX);
}

static void test_unary_instructions(void)
{
    EXPECT(-5, K8(5), LEDVM_NEG);
    EXPECT(1, K8(0), LEDVM_NOT);
    EXPECT(0, K8(9), LEDVM_NOT);
    EXPECT(5, K16(-5), LEDVM_ABS);
    EXPECT(-32768, K16(-32768), LEDVM_ABS);
}

static void test_stack_instructions_and_select(void)
{
    EXPECT(14, K8(7), LEDVM_DUP, LEDVM_ADD);
    EXPECT(-2, K8(5), K8(3), LEDVM_SWAP, LEDVM_SUB); /* 3 - 5 */
    EXPECT(5, K8(5), K8(3), LEDVM_DROP);
    EXPECT(10, K8(1), K8(10), K8(20), LEDVM_SEL);
    EXPECT(20, K8(0), K8(10), K8(20), LEDVM_SEL);
}

static void test_waves(void)
{
    TEST_ASSERT_EQUAL_UINT8(128, ledvm_sin8(0));
    TEST_ASSERT_EQUAL_UINT8(255, ledvm_sin8(64));
    TEST_ASSERT_EQUAL_UINT8(128, ledvm_sin8(128));
    TEST_ASSERT_EQUAL_UINT8(1, ledvm_sin8(192));
    TEST_ASSERT_EQUAL_UINT8(128 + 90, ledvm_sin8(32));
    TEST_ASSERT_EQUAL_UINT8(128 - 90, ledvm_sin8(160));
    EXPECT(255, K16(64 + 256), LEDVM_SIN8); /* only the low byte counts */
    EXPECT(0, K8(0), LEDVM_TRI8);
    EXPECT(254, K8(127), LEDVM_TRI8);
    EXPECT(254, K8(128), LEDVM_TRI8);
    EXPECT(0, K8(255), LEDVM_TRI8);
}

/* SIN8 of T >> 8 continues smoothly when T wraps at 65536. */
static void test_time_wraps_without_a_jump(void)
{
    static const uint8_t code[] = {LEDVM_T, K8(8), LEDVM_SHR, LEDVM_SIN8, K8(0), K8(0), LEDVM_RGB};
    uint8_t before[3], after[3];

    load(code, sizeof code, LEDVM_LAYOUT_RGB);
    ledvm_led(&vm, before, 0, 1, 0xFF00u);
    ledvm_led(&vm, after, 0, 1, 0x0000u);
    TEST_ASSERT_UINT8_WITHIN(4, before[0], after[0]);
}

/* ---- inputs -------------------------------------------------------------------------------------------------------- */

static void test_index_count_time_and_parameters(void)
{
    static const uint8_t code[] = {LEDVM_I, LEDVM_N, LEDVM_T, LEDVM_P(7), LEDVM_ADD, LEDVM_RGB};
    uint8_t led[3];

    load(code, sizeof code, LEDVM_LAYOUT_RGB);
    vm.p[7] = 100;
    ledvm_led(&vm, led, 3, 60, 50);
    TEST_ASSERT_EQUAL_UINT8(3, led[0]);
    TEST_ASSERT_EQUAL_UINT8(60, led[1]);
    TEST_ASSERT_EQUAL_UINT8(150, led[2]);
}

static void test_prev_reads_the_led_in_its_layout(void)
{
    static const uint8_t decay[] = {LEDVM_PREV(0), K8(10), LEDVM_SUB, LEDVM_PREV(1), LEDVM_PREV(3), LEDVM_RGB};
    uint8_t led[3] = {0x50, 0x05, 0x77}; /* GRB: G = 0x50, R = 0x05, B = 0x77 */

    load(decay, sizeof decay, LEDVM_LAYOUT_GRB);
    ledvm_led(&vm, led, 0, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(0x50, led[0]); /* G stays */
    TEST_ASSERT_EQUAL_UINT8(0, led[1]);    /* R - 10, clamped */
    TEST_ASSERT_EQUAL_UINT8(0, led[2]);    /* B = W, which GRB does not have */
}

static void test_variables_persist_until_the_next_start(void)
{
    static const uint8_t count[] = {LEDVM_LOAD(2), K8(1), LEDVM_ADD, LEDVM_DUP, LEDVM_STORE(2), K8(0), K8(0),
                                    LEDVM_RGB};
    uint8_t led[3];

    load(count, sizeof count, LEDVM_LAYOUT_RGB);
    ledvm_led(&vm, led, 0, 2, 0);
    ledvm_led(&vm, led, 1, 2, 0);
    ledvm_led(&vm, led, 0, 2, 1);
    TEST_ASSERT_EQUAL_UINT8(3, led[0]);
    ledvm_start(&vm, sizeof count, LEDVM_LAYOUT_RGB);
    ledvm_led(&vm, led, 0, 2, 0);
    TEST_ASSERT_EQUAL_UINT8(1, led[0]);
}

static void test_random_numbers_repeat_after_a_start(void)
{
    static const uint8_t code[] = {LEDVM_RAND, LEDVM_RAND, LEDVM_RAND, LEDVM_RGB};
    uint8_t a[3], b[3], c[3];

    load(code, sizeof code, LEDVM_LAYOUT_RGB);
    ledvm_led(&vm, a, 0, 1, 0);
    ledvm_led(&vm, b, 0, 1, 0);
    TEST_ASSERT_FALSE(memcmp(a, b, 3) == 0);
    TEST_ASSERT_FALSE(a[0] == a[1] && a[1] == a[2]);
    ledvm_start(&vm, sizeof code, LEDVM_LAYOUT_RGB);
    ledvm_led(&vm, c, 0, 1, 0);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, c, 3);
}

/* ---- outputs ------------------------------------------------------------------------------------------------------- */

static void test_rgb_in_every_layout(void)
{
    static const uint8_t rgb[] = {K8(10), K8(20), K8(30), LEDVM_RGB};
    static const uint8_t rgbw[] = {K8(10), K8(20), K8(30), K8(40), LEDVM_RGBW};
    uint8_t led[5];

    memset(led, 0xEE, sizeof led);
    load(rgb, sizeof rgb, LEDVM_LAYOUT_GRB);
    ledvm_led(&vm, led, 0, 1, 0);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((const uint8_t[]){20, 10, 30, 0xEE}), led, 4);

    load(rgb, sizeof rgb, LEDVM_LAYOUT_GRBW);
    ledvm_led(&vm, led, 0, 1, 0);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((const uint8_t[]){20, 10, 30, 0, 0xEE}), led, 5);

    load(rgbw, sizeof rgbw, LEDVM_LAYOUT_RGBW);
    ledvm_led(&vm, led, 0, 1, 0);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((const uint8_t[]){10, 20, 30, 40, 0xEE}), led, 5);

    memset(led, 0xEE, sizeof led);
    load(rgbw, sizeof rgbw, LEDVM_LAYOUT_RGB); /* W dropped */
    ledvm_led(&vm, led, 0, 1, 0);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((const uint8_t[]){10, 20, 30, 0xEE}), led, 4);
}

static void test_outputs_are_clamped(void)
{
    static const uint8_t code[] = {K16(300), K16(-4), K8(255), LEDVM_RGB};
    uint8_t led[3];

    load(code, sizeof code, LEDVM_LAYOUT_RGB);
    ledvm_led(&vm, led, 0, 1, 0);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((const uint8_t[]){255, 0, 255}), led, 3);
}

static void test_hsv(void)
{
    static const uint8_t code[] = {LEDVM_P(0), K8(255), K8(255), LEDVM_HSV};
    uint8_t rgb[3];
    uint8_t led[4];

    ledvm_hsv(0, 255, 255, rgb);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((const uint8_t[]){255, 0, 0}), rgb, 3);
    ledvm_hsv(171, 255, 255, rgb); /* blue, with a trace of green */
    TEST_ASSERT_EQUAL_UINT8(0, rgb[0]);
    TEST_ASSERT_LESS_THAN_UINT8(8, rgb[1]);
    TEST_ASSERT_EQUAL_UINT8(255, rgb[2]);
    ledvm_hsv(99, 0, 200, rgb); /* no saturation: grey */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((const uint8_t[]){200, 200, 200}), rgb, 3);

    load(code, sizeof code, LEDVM_LAYOUT_GRBW);
    vm.p[0] = 85; /* green */
    memset(led, 0xEE, sizeof led);
    ledvm_led(&vm, led, 0, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(255, led[0]); /* G */
    TEST_ASSERT_LESS_THAN_UINT8(8, led[1]);
    TEST_ASSERT_EQUAL_UINT8(0, led[2]);
    TEST_ASSERT_EQUAL_UINT8(0, led[3]); /* W */
}

/* The programs in docs/example-ledstrip.md: valid, and the CRC given there. */
static void test_documented_programs(void)
{
    static const uint8_t rainbow[] = {0x02, 0x00, 0x04, 0x22, 0x04, 0x00, 0x03, 0x29, 0x20, 0x00, 0xFF,
                                      0x04, 0x00, 0x02, 0x29, 0x02, 0x00, 0x10, 0x22, 0x20, 0x33, 0x3E};
    static const uint8_t wave[] = {8, 9, 4, 0, 3, 41, 2, 0, 8, 34, 32, 51, 34, 0, 8, 41, 0, 0, 60};
    uint8_t led[3];

    TEST_ASSERT_TRUE(ledvm_check(rainbow, sizeof rainbow));
    TEST_ASSERT_EQUAL_UINT(19, sizeof wave);
    TEST_ASSERT_EQUAL_UINT8(71, hubus_crc8(wave, sizeof wave));
    load(wave, sizeof wave, LEDVM_LAYOUT_GRB);
    vm.p[0] = 255;
    vm.p[1] = 200;
    ledvm_led(&vm, led, 0, 60, 32 * 8); /* sin8(32 + 0) = 128 + 90 */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((const uint8_t[]){200 * 218 >> 8, 255, 0}), led, 3);
}

/* ---- robustness ---------------------------------------------------------------------------------------------------- */

/* Arbitrary code, as after a PROG_WRITE in the middle of a frame, stays within the stack and the LED (sanitizers). */
static void test_arbitrary_code_is_harmless(void)
{
    uint8_t led[4 + 1] = {0};
    unsigned round;

    srand(1234);
    for (round = 0; round < 20000u; round++) {
        uint8_t k;

        for (k = 0; k < LEDVM_CODE_MAX; k++) {
            vm.code[k] = (uint8_t)(rand() % 0x48);
        }
        vm.len = (uint8_t)(rand() % (LEDVM_CODE_MAX + 1));
        vm.layout = (uint8_t)(rand() & 3);
        led[4] = 0x5A;
        ledvm_led(&vm, led, (uint16_t)rand(), 60, (uint16_t)rand());
        if (ledvm_bpp(vm.layout) == 3) {
            TEST_ASSERT_EQUAL_HEX8(0x5A, led[4]);
        }
    }
    vm.len = LEDVM_CODE_MAX + 1; /* a corrupt length is refused */
    ledvm_led(&vm, led, 0, 1, 0);
}

/* Code that never passed the check: an instruction that would take more values than the stack holds, or push beyond
 * it, ends the run and leaves the LED as it is. */
static void test_run_time_stack_check(void)
{
    static const uint8_t underflow[] = {K8(1), LEDVM_RGB};
    static const uint8_t overflow[] = {K8(1), K8(1), K8(1), K8(1), K8(1), K8(1), K8(1), K8(1), K8(1), LEDVM_RGB};
    uint8_t led[3] = {0xA5, 0xA5, 0xA5};

    memcpy(vm.code, underflow, sizeof underflow);
    ledvm_start(&vm, sizeof underflow, LEDVM_LAYOUT_RGB);
    ledvm_led(&vm, led, 0, 1, 0);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((const uint8_t[]){0xA5, 0xA5, 0xA5}), led, 3);
    memcpy(vm.code, overflow, sizeof overflow);
    ledvm_start(&vm, sizeof overflow, LEDVM_LAYOUT_RGB);
    ledvm_led(&vm, led, 0, 1, 0);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((const uint8_t[]){0xA5, 0xA5, 0xA5}), led, 3);
}

/* Whatever the check accepts runs to its output: the colour does not depend on what the LED held before (no PREV in
 * these programs), so an output that was not written would show the old content. */
static void test_checked_programs_always_write_the_led(void)
{
    unsigned round, accepted = 0;

    srand(99);
    for (round = 0; round < 200000u && accepted < 200u; round++) {
        static const uint8_t pool[] = {LEDVM_I, LEDVM_T, LEDVM_N, LEDVM_RAND, LEDVM_P(1), LEDVM_LOAD(0),
                                       LEDVM_ADD, LEDVM_MUL, LEDVM_DIV, LEDVM_SHR, LEDVM_SIN8, LEDVM_DUP,
                                       LEDVM_SWAP, LEDVM_SEL, LEDVM_RGB, LEDVM_HSV, LEDVM_RGBW};
        uint8_t code[12];
        uint8_t len = (uint8_t)(4 + rand() % 8);
        uint8_t a[3] = {7, 7, 7};
        uint8_t b[3] = {200, 200, 200};
        uint8_t k;

        for (k = 0; k < len; k++) {
            code[k] = pool[rand() % sizeof pool];
        }
        if (!ledvm_check(code, len)) {
            continue;
        }
        accepted++;
        memcpy(vm.code, code, len);
        vm.v[0] = 0x1234; /* cleared by the start */
        ledvm_start(&vm, len, LEDVM_LAYOUT_RGB);
        ledvm_led(&vm, a, 5, 60, 1000);
        ledvm_start(&vm, len, LEDVM_LAYOUT_RGB);
        ledvm_led(&vm, b, 5, 60, 1000);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(a, b, 3);
    }
    TEST_ASSERT_TRUE(accepted >= 20u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_accepts_a_complete_expression);
    RUN_TEST(test_check_refuses_broken_programs);
    RUN_TEST(test_check_allows_the_full_stack);
    RUN_TEST(test_arithmetic_wraps_at_16_bits);
    RUN_TEST(test_division_by_zero_gives_zero);
    RUN_TEST(test_bits_and_shifts);
    RUN_TEST(test_comparisons_are_signed);
    RUN_TEST(test_unary_instructions);
    RUN_TEST(test_stack_instructions_and_select);
    RUN_TEST(test_waves);
    RUN_TEST(test_time_wraps_without_a_jump);
    RUN_TEST(test_index_count_time_and_parameters);
    RUN_TEST(test_prev_reads_the_led_in_its_layout);
    RUN_TEST(test_variables_persist_until_the_next_start);
    RUN_TEST(test_random_numbers_repeat_after_a_start);
    RUN_TEST(test_rgb_in_every_layout);
    RUN_TEST(test_outputs_are_clamped);
    RUN_TEST(test_hsv);
    RUN_TEST(test_documented_programs);
    RUN_TEST(test_arbitrary_code_is_harmless);
    RUN_TEST(test_run_time_stack_check);
    RUN_TEST(test_checked_programs_always_write_the_led);
    return UNITY_END();
}
