#include "hubus_gen.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t A[] = {1, 2, 3};
static const uint8_t B[] = {1, 2, 4};

static void test_first_frame_is_baseline(void)
{
    hubus_gen_tracker_t t;

    hubus_gen_reset(&t);
    TEST_ASSERT_EQUAL(HUBUS_GEN_BASELINE, hubus_gen_eval(&t, 2, 0, A, 3));
    TEST_ASSERT_TRUE(t.valid);
    TEST_ASSERT_EQUAL_UINT8(2, t.gen);
}

static void test_normal_sequence_including_wrap(void)
{
    hubus_gen_tracker_t t;
    uint8_t gen;

    hubus_gen_reset(&t);
    TEST_ASSERT_EQUAL(HUBUS_GEN_BASELINE, hubus_gen_eval(&t, 1, 0, A, 3));
    for (gen = 2; gen < 12; gen++) {
        TEST_ASSERT_EQUAL(HUBUS_GEN_NORMAL, hubus_gen_eval(&t, (uint8_t)(gen & 3u), 0, B, 3)); /* 3 -> 0 wraps */
    }
}

static void test_duplicate_equal_gen_same_payload(void)
{
    hubus_gen_tracker_t t;

    hubus_gen_reset(&t);
    hubus_gen_eval(&t, 1, 0, A, 3);
    TEST_ASSERT_EQUAL(HUBUS_GEN_DUPLICATE, hubus_gen_eval(&t, 1, 0, A, 3));
    TEST_ASSERT_EQUAL(HUBUS_GEN_DUPLICATE, hubus_gen_eval(&t, 1, 0, A, 3));
    /* a duplicate does not disturb the sequence */
    TEST_ASSERT_EQUAL(HUBUS_GEN_NORMAL, hubus_gen_eval(&t, 2, 0, B, 3));
}

static void test_equal_gen_different_payload_is_new_content(void)
{
    hubus_gen_tracker_t t;

    hubus_gen_reset(&t);
    hubus_gen_eval(&t, 1, 0, A, 3);
    TEST_ASSERT_EQUAL(HUBUS_GEN_WRAPPED, hubus_gen_eval(&t, 1, 0, B, 3));
    /* different length counts as different payload */
    TEST_ASSERT_EQUAL(HUBUS_GEN_WRAPPED, hubus_gen_eval(&t, 1, 0, B, 2));
}

static void test_gap_reports_overwritten_contents(void)
{
    hubus_gen_tracker_t t;

    hubus_gen_reset(&t);
    hubus_gen_eval(&t, 0, 0, A, 3);
    TEST_ASSERT_EQUAL(HUBUS_GEN_GAP, hubus_gen_eval(&t, 2, 0, B, 3)); /* GEN 1 was overwritten */
    TEST_ASSERT_EQUAL_UINT8(1, hubus_gen_lost(0, 2));
    TEST_ASSERT_EQUAL(HUBUS_GEN_GAP, hubus_gen_eval(&t, 1, 0, A, 3)); /* 2 -> 1: GEN 3 and 0 overwritten */
    TEST_ASSERT_EQUAL_UINT8(2, hubus_gen_lost(2, 1));
}

static void test_zero_length_payloads(void)
{
    hubus_gen_tracker_t t;

    hubus_gen_reset(&t);
    hubus_gen_eval(&t, 3, 0, NULL, 0);
    TEST_ASSERT_EQUAL(HUBUS_GEN_DUPLICATE, hubus_gen_eval(&t, 3, 0, NULL, 0));
    TEST_ASSERT_EQUAL(HUBUS_GEN_NORMAL, hubus_gen_eval(&t, 0, 0, NULL, 0));
}

static void test_independent_trackers(void)
{
    hubus_gen_tracker_t a, b;

    hubus_gen_reset(&a);
    hubus_gen_reset(&b);
    hubus_gen_eval(&a, 1, 0, A, 3);
    TEST_ASSERT_EQUAL(HUBUS_GEN_BASELINE, hubus_gen_eval(&b, 1, 0, A, 3));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_frame_is_baseline);
    RUN_TEST(test_normal_sequence_including_wrap);
    RUN_TEST(test_duplicate_equal_gen_same_payload);
    RUN_TEST(test_equal_gen_different_payload_is_new_content);
    RUN_TEST(test_gap_reports_overwritten_contents);
    RUN_TEST(test_zero_length_payloads);
    RUN_TEST(test_independent_trackers);
    return UNITY_END();
}
