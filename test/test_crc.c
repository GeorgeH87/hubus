#include <string.h>

#include "hubus_crc.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_check_value(void)
{
    TEST_ASSERT_EQUAL_HEX8(0xDF, hubus_crc8((const uint8_t *)"123456789", 9));
    TEST_ASSERT_EQUAL_HEX8(HUBUS_CRC_CHECK, hubus_crc8((const uint8_t *)"123456789", 9));
}

static void test_incremental_equals_oneshot(void)
{
    uint8_t data[40];
    size_t len;
    unsigned i;

    for (i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)(i * 37u + 11u);
    }
    for (len = 0; len <= sizeof data; len++) {
        uint8_t crc = HUBUS_CRC_INIT;
        size_t k;

        for (k = 0; k < len; k++) {
            crc = hubus_crc8_update(crc, data[k]);
        }
        TEST_ASSERT_EQUAL_HEX8(hubus_crc8(data, len), hubus_crc8_final(crc));
    }
}

static void test_empty_input(void)
{
    /* init 0xFF, xorout 0xFF: the CRC of nothing is 0x00 */
    TEST_ASSERT_EQUAL_HEX8(0x00, hubus_crc8(NULL, 0));
}

/* Independent reference: textbook MSB-first bit loop. */
static uint8_t reference(const uint8_t *d, size_t n)
{
    uint8_t crc = 0xFF;
    size_t i;
    int b;

    for (i = 0; i < n; i++) {
        crc ^= d[i];
        for (b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x2F) : (uint8_t)(crc << 1);
        }
    }
    return crc ^ 0xFF;
}

static void test_matches_reference(void)
{
    uint8_t data[34];
    unsigned seed = 12345;
    int round;

    for (round = 0; round < 200; round++) {
        size_t n = (size_t)(round % 34) + 1;
        size_t i;

        for (i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            data[i] = (uint8_t)(seed >> 16);
        }
        TEST_ASSERT_EQUAL_HEX8(reference(data, n), hubus_crc8(data, n));
    }
}

static void test_degenerate_frames_are_not_valid(void)
{
    /* The non-zero init/xorout makes all-released (0x00) and all-low (0xFF)
     * streams invalid frames (docs/spec/04-frames.md section 4.12). */
    uint8_t zeros[4] = {0, 0, 0, 0};
    uint8_t ones[4] = {0xFF, 0xFF, 0xFF, 0xFF};

    TEST_ASSERT_NOT_EQUAL(0x00, hubus_crc8(zeros, 3));
    TEST_ASSERT_NOT_EQUAL(0xFF, hubus_crc8(ones, 3));
    TEST_ASSERT_NOT_EQUAL(zeros[3], hubus_crc8(zeros, 3));
    TEST_ASSERT_NOT_EQUAL(ones[3], hubus_crc8(ones, 3));
}

static void test_single_bit_errors_detected(void)
{
    uint8_t data[34];
    size_t n;

    for (n = 3; n <= sizeof data; n++) {
        uint8_t good;
        size_t bit;

        memset(data, 0x5A, sizeof data);
        good = hubus_crc8(data, n);
        for (bit = 0; bit < n * 8; bit++) {
            data[bit / 8] ^= (uint8_t)(0x80u >> (bit % 8));
            TEST_ASSERT_NOT_EQUAL(good, hubus_crc8(data, n));
            data[bit / 8] ^= (uint8_t)(0x80u >> (bit % 8));
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_value);
    RUN_TEST(test_incremental_equals_oneshot);
    RUN_TEST(test_empty_input);
    RUN_TEST(test_matches_reference);
    RUN_TEST(test_degenerate_frames_are_not_valid);
    RUN_TEST(test_single_bit_errors_detected);
    return UNITY_END();
}
