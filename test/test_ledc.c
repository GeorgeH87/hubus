/*
 * The compiler from formulas to LED bytecode (daemon/hubusd_ledc.h). What it produces runs in the interpreter of the slave here,
 * so a test states the result of a formula, not only its bytes.
 */
#include <stdio.h>
#include <string.h>

#include "hubusd_ledc.h"
#include "ledvm.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t code[LEDVM_CODE_MAX];
static char err[120];

static uint8_t compile_ok(const char *formula)
{
    uint8_t len = hubusd_ledc_compile(formula, code, err, sizeof err);

    TEST_ASSERT_TRUE_MESSAGE(len > 0u, err);
    TEST_ASSERT_TRUE(ledvm_check(code, len));
    return len;
}

static void assert_refused(const char *formula, const char *reason)
{
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, hubusd_ledc_compile(formula, code, err, sizeof err), formula);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, reason), err);
}

/* Runs a formula for one LED; the red channel holds the value (layout RGB), so `rgb(<expression>, 0, 0)` shows an expression. */
static uint8_t red_of(const char *expression, uint16_t i, uint16_t n, uint16_t t)
{
    static ledvm_t vm;
    char formula[120];
    uint8_t led[3] = {11, 22, 33};
    uint8_t len;

    snprintf(formula, sizeof formula, "rgb(%s, 0, 0)", expression);
    len = compile_ok(formula);
    memset(&vm, 0, sizeof vm);
    memcpy(vm.code, code, len);
    vm.p[1] = 77;
    ledvm_start(&vm, len, LEDVM_LAYOUT_RGB);
    ledvm_led(&vm, led, i, n, t);
    return led[0];
}

#define ASSERT_VALUE(expected, expression) TEST_ASSERT_EQUAL_UINT8_MESSAGE((expected), red_of((expression), 3, 60, 100), (expression))

static void test_the_formulas_of_the_documentation_give_its_bytes(void)
{
    static const uint8_t rainbow[] = {0x02, 0x00, 0x04, 0x22, 0x04, 0x00, 0x03, 0x29, 0x20, 0x00, 0xFF,
                                      0x04, 0x00, 0x02, 0x29, 0x02, 0x00, 0x10, 0x22, 0x20, 0x33, 0x3E};
    static const uint8_t wave[] = {8, 9, 4, 0, 3, 41, 2, 0, 8, 34, 32, 51, 34, 0, 8, 41, 0, 0, 60};
    uint8_t len;

    len = compile_ok("hsv(i * 4 + (t >> 3), 255, sin8((t >> 2) + i * 16))");
    TEST_ASSERT_EQUAL_UINT8(sizeof rainbow, len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rainbow, code, len);

    len = compile_ok("rgb(p0, (p1 * sin8((t >> 3) + i * 8)) >> 8, 0)");
    TEST_ASSERT_EQUAL_UINT8(sizeof wave, len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(wave, code, len);
}

static void test_arithmetic_and_precedence_work_as_in_c(void)
{
    ASSERT_VALUE(7, "1 + 2 * 3");
    ASSERT_VALUE(9, "(1 + 2) * 3");
    ASSERT_VALUE(2, "7 - 5");
    ASSERT_VALUE(3, "10 / 3");
    ASSERT_VALUE(1, "10 % 3");
    ASSERT_VALUE(0, "5 / 0");
    ASSERT_VALUE(8, "1 << 1 + 2");
    ASSERT_VALUE(1, "1 + 2 < 4");
    ASSERT_VALUE(6, "2 | 4 & 6 ^ 0");
    ASSERT_VALUE(3, "0x0F & 3");
    ASSERT_VALUE(16, "4 * 0x4");
    ASSERT_VALUE(10, "010"); /* no octal numbers */
    ASSERT_VALUE(44, "300 - 256 + 0");
}

static void test_comparisons_and_their_negations(void)
{
    ASSERT_VALUE(1, "2 < 3");
    ASSERT_VALUE(0, "3 < 3");
    ASSERT_VALUE(1, "3 <= 3");
    ASSERT_VALUE(0, "4 <= 3");
    ASSERT_VALUE(1, "4 > 3");
    ASSERT_VALUE(1, "3 >= 3");
    ASSERT_VALUE(0, "2 >= 3");
    ASSERT_VALUE(1, "3 == 3");
    ASSERT_VALUE(1, "3 != 4");
    ASSERT_VALUE(0, "3 != 3");
}

static void test_unary_operators(void)
{
    ASSERT_VALUE(3, "5 + -2");
    ASSERT_VALUE(0, "!4");
    ASSERT_VALUE(1, "!0");
    ASSERT_VALUE(5, "- -5");
    ASSERT_VALUE(0xF0, "~0x0F & 0xF0");
}

static void test_selection_goes_right_to_left(void)
{
    ASSERT_VALUE(1, "1 ? 1 : 2");
    ASSERT_VALUE(2, "0 ? 1 : 2");
    ASSERT_VALUE(3, "0 ? 1 : 0 ? 2 : 3");
}

static void test_inputs_and_functions(void)
{
    TEST_ASSERT_EQUAL_UINT8(3, red_of("i", 3, 60, 100));
    TEST_ASSERT_EQUAL_UINT8(60, red_of("n", 3, 60, 100));
    TEST_ASSERT_EQUAL_UINT8(100, red_of("t", 3, 60, 100));
    TEST_ASSERT_EQUAL_UINT8(77, red_of("p1", 3, 60, 100));
    TEST_ASSERT_EQUAL_UINT8(22, red_of("prev(1)", 3, 60, 100));
    TEST_ASSERT_EQUAL_UINT8(ledvm_sin8(40), red_of("sin8(i * 10 + 10)", 3, 60, 100));
    TEST_ASSERT_EQUAL_UINT8(2, red_of("min(2, 5)", 3, 60, 100));
    TEST_ASSERT_EQUAL_UINT8(5, red_of("max(2, 5)", 3, 60, 100));
    TEST_ASSERT_EQUAL_UINT8(4, red_of("abs(0 - 4)", 3, 60, 100));
    TEST_ASSERT_TRUE(compile_ok("rgb(rand, tri8(t), 0)") > 0u);
}

static void test_assignments_keep_a_value_for_the_next_led(void)
{
    static ledvm_t vm;
    uint8_t led[3] = {0, 0, 0};
    uint8_t len = compile_ok("v1 = v1 + 5; v2 = v1 * 2; rgb(v1, v2, 0)");

    memset(&vm, 0, sizeof vm);
    memcpy(vm.code, code, len);
    ledvm_start(&vm, len, LEDVM_LAYOUT_RGB);
    ledvm_led(&vm, led, 0, 2, 0);
    ledvm_led(&vm, led, 1, 2, 0);
    TEST_ASSERT_EQUAL_UINT8(10, led[0]);
    TEST_ASSERT_EQUAL_UINT8(20, led[1]);
}

static void test_all_three_outputs(void)
{
    TEST_ASSERT_EQUAL_UINT8(LEDVM_RGB, code[compile_ok("rgb(1, 2, 3)") - 1u]);
    TEST_ASSERT_EQUAL_UINT8(LEDVM_RGBW, code[compile_ok("rgbw(1, 2, 3, 4)") - 1u]);
    TEST_ASSERT_EQUAL_UINT8(LEDVM_HSV, code[compile_ok(" hsv ( 1 , 2 , 3 ) ") - 1u]);
}

static void test_what_is_refused_says_why(void)
{
    assert_refused("", "ends with");
    assert_refused("1 + 2", "ends with");
    assert_refused("red(1, 2, 3)", "ends with");
    assert_refused("rgb(1, 2)", "','");
    assert_refused("rgb(1, 2, 3, 4)", "')'");
    assert_refused("rgb(1, 2, 3) x", "after the colour");
    assert_refused("rgb(1, 2, 3); rgb(1, 2, 3)", "after the colour");
    assert_refused("rgb(1, 2, ", "ends too early");
    assert_refused("rgb(1, 2, foo)", "unknown name 'foo'");
    assert_refused("rgb(1, 2, p8)", "unknown name 'p8'");
    assert_refused("rgb(1, 2, v4)", "unknown name 'v4'");
    assert_refused("rgb(1, 2, 65536)", "65535");
    assert_refused("rgb(1, 2, prev(4))", "0..3");
    assert_refused("rgb(1, 2, 3 && 4)", "')'");
    assert_refused("rgb(1, 2, 3 || 4)", "')'");
    assert_refused("rgb(1, 2, 3 ? 4)", "':'");
    assert_refused("v0 == 1; rgb(1, 2, 3)", "ends with");
    assert_refused("v0 = 1 rgb(1, 2, 3)", "';'");
    assert_refused("rgb(1, 2, )", "unexpected ')'");
}

static void test_a_position_comes_with_the_reason(void)
{
    assert_refused("rgb(1, 2, foo)", "at 11:");
}

static void test_programs_the_slave_could_not_hold_are_refused(void)
{
    /* the bytes run out: 48 are all a slave has */
    assert_refused("rgb(1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1, 2, 3)", "48 bytes");
    /* the stack runs out: eight values at most */
    assert_refused("rgb(1+(2+(3+(4+(5+(6+(7+(8+9))))))), 0, 0)", "stack");
}

static void test_deep_nesting_ends_with_an_error(void)
{
    char formula[400];
    size_t k = 0;

    k += (size_t)snprintf(formula, sizeof formula, "rgb(");
    memset(&formula[k], '(', 200);
    formula[k + 200u] = '\0';
    assert_refused(formula, "nested too deep");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_formulas_of_the_documentation_give_its_bytes);
    RUN_TEST(test_arithmetic_and_precedence_work_as_in_c);
    RUN_TEST(test_comparisons_and_their_negations);
    RUN_TEST(test_unary_operators);
    RUN_TEST(test_selection_goes_right_to_left);
    RUN_TEST(test_inputs_and_functions);
    RUN_TEST(test_assignments_keep_a_value_for_the_next_led);
    RUN_TEST(test_all_three_outputs);
    RUN_TEST(test_what_is_refused_says_why);
    RUN_TEST(test_a_position_comes_with_the_reason);
    RUN_TEST(test_programs_the_slave_could_not_hold_are_refused);
    RUN_TEST(test_deep_nesting_ends_with_an_error);
    return UNITY_END();
}
