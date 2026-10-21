#include <stdlib.h>
#include <string.h>

#include "hubus_arb.h"
#include "hubus_frame.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/*
 * Model of what the slaves put on DATA during one arbitration (written
 * straight from docs/ARBITRATION.md section 4, independent of the slave
 * core). Returns the 12 slots, slot 0 in bit 0.
 */
static uint16_t model_slots(const uint8_t *pending, unsigned n, uint8_t prefix, uint8_t len)
{
    uint8_t active[64];
    unsigned n_active = 0;
    uint16_t slots = 0;
    unsigned i;
    int k;

    for (i = 0; i < n; i++) {
        if (hubus_mcid_match(pending[i], prefix, len)) {
            active[n_active++] = pending[i];
        }
    }
    for (k = 5; k >= 0; k--) {
        unsigned slot = (unsigned)(2 * (5 - k));
        int low = 0;
        unsigned keep = 0;
        int handoff = 0;

        for (i = 0; i < n_active; i++) {
            if ((active[i] >> k) & 1) {
                low = 1;
            }
        }
        for (i = 0; i < n_active; i++) {
            if (low && !((active[i] >> k) & 1)) {
                handoff = 1; /* lost at exactly this bit */
            } else {
                active[keep++] = active[i];
            }
        }
        n_active = keep;
        if (low) {
            slots |= (uint16_t)(1u << slot);
        }
        if (handoff) {
            slots |= (uint16_t)(1u << (slot + 1));
        }
    }
    return slots;
}

static void test_spec_example_first_poll(void)
{
    const uint8_t pending[] = {42, 40, 25, 15};
    uint16_t slots = model_slots(pending, 4, 0, 0);
    hubus_arb_result_t r;

    /* docs/ARBITRATION.md section 7, poll 1 */
    TEST_ASSERT_TRUE(hubus_arb_decode(slots, 0, 0, &r));
    TEST_ASSERT_TRUE(r.contender);
    TEST_ASSERT_EQUAL_UINT8(42, r.winner);
    TEST_ASSERT_EQUAL_UINT8(2, r.n_masks);
    TEST_ASSERT_EQUAL_UINT8(0, r.masks[0].prefix); /* bit 5: "0" / len 1 */
    TEST_ASSERT_EQUAL_UINT8(1, r.masks[0].len);
    TEST_ASSERT_EQUAL_UINT8(40, r.masks[1].prefix); /* bit 1: "10100" / len 5 */
    TEST_ASSERT_EQUAL_UINT8(5, r.masks[1].len);
}

static void test_spec_example_slot_pattern(void)
{
    const uint8_t pending[] = {42, 40, 25, 15};
    uint16_t slots = model_slots(pending, 4, 0, 0);
    /* arb5 low, ho5 low, arb4 high, ho4 high, arb3 low, ho3 high, arb2 high, ho2 high,
     * arb1 low, ho1 low, arb0 high, ho0 high */
    const unsigned expect_low[12] = {1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0};
    unsigned i;

    for (i = 0; i < 12; i++) {
        TEST_ASSERT_EQUAL_UINT(expect_low[i], (slots >> i) & 1u);
    }
}

static void test_spec_example_full_burst(void)
{
    const uint8_t pending[] = {42, 40, 25, 15};
    const uint8_t order[] = {42, 40, 25, 15};
    hubus_mask_stack_t stack;
    hubus_mask_t mask = {0, 0};
    unsigned served = 0;
    unsigned polls = 0;

    hubus_mask_stack_clear(&stack);
    for (;;) {
        hubus_arb_result_t r;
        uint16_t slots = model_slots(pending, 4, mask.prefix, mask.len);
        unsigned i;

        polls++;
        hubus_arb_decode(slots, mask.prefix, mask.len, &r);
        if (r.contender) {
            TEST_ASSERT_EQUAL_UINT8(order[served], r.winner);
            served++;
            for (i = 0; i < r.n_masks; i++) {
                TEST_ASSERT_TRUE(hubus_mask_stack_push(&stack, r.masks[i]));
            }
        }
        if (!hubus_mask_stack_pop(&stack, &mask)) {
            break;
        }
    }
    TEST_ASSERT_EQUAL_UINT(4, served);
    TEST_ASSERT_EQUAL_UINT(4, polls);
}

static void test_no_contender(void)
{
    hubus_arb_result_t r;

    TEST_ASSERT_TRUE(hubus_arb_decode(0, 0, 0, &r));
    TEST_ASSERT_FALSE(r.contender);
    TEST_ASSERT_EQUAL_UINT8(0, r.n_masks);
}

static void test_single_slave_every_mcid(void)
{
    unsigned mcid;

    for (mcid = 1; mcid < 64; mcid++) {
        uint8_t m = (uint8_t)mcid;
        hubus_arb_result_t r;

        TEST_ASSERT_TRUE(hubus_arb_decode(model_slots(&m, 1, 0, 0), 0, 0, &r));
        TEST_ASSERT_TRUE(r.contender);
        TEST_ASSERT_EQUAL_UINT8(mcid, r.winner);
        TEST_ASSERT_EQUAL_UINT8(0, r.n_masks); /* nobody lost */
    }
}

/* Complete burst on the model for random pending sets: every pending slave is
 * served exactly once, in strictly descending MCID order, with at most one
 * stack entry per prefix length (docs/ARBITRATION.md section 6.2). */
static void test_random_bursts(void)
{
    unsigned seed = 4242;
    int round;

    for (round = 0; round < 3000; round++) {
        uint8_t pending[63];
        unsigned n = 0;
        unsigned density = 1 + (unsigned)(round % 8);
        unsigned mcid;
        hubus_mask_stack_t stack;
        hubus_mask_t mask = {0, 0};
        int last = 64;
        unsigned served = 0;
        unsigned max_depth = 0;

        for (mcid = 1; mcid < 64; mcid++) {
            seed = seed * 1103515245u + 12345u;
            if (((seed >> 16) % 8u) < density) {
                pending[n++] = (uint8_t)mcid;
            }
        }

        hubus_mask_stack_clear(&stack);
        for (;;) {
            hubus_arb_result_t r;

            if (mask.len == 6) {
                /* exact mask: served without poll */
                TEST_ASSERT_TRUE((int)mask.prefix < last);
                last = mask.prefix;
                served++;
            } else {
                unsigned i;
                TEST_ASSERT_TRUE(hubus_arb_decode(model_slots(pending, n, mask.prefix, mask.len), mask.prefix, mask.len, &r));
                if (r.contender) {
                    TEST_ASSERT_TRUE((int)r.winner < last);
                    last = r.winner;
                    served++;
                    for (i = 0; i < r.n_masks; i++) {
                        TEST_ASSERT_TRUE(hubus_mask_stack_push(&stack, r.masks[i]));
                    }
                    if (stack.n > max_depth) {
                        max_depth = stack.n;
                    }
                }
            }
            if (!hubus_mask_stack_pop(&stack, &mask)) {
                break;
            }
        }
        TEST_ASSERT_EQUAL_UINT(n, served);
        TEST_ASSERT_TRUE(max_depth <= 6);
    }
}

static void test_prefix_poll_only_group_contends(void)
{
    const uint8_t pending[] = {42, 40, 25, 15};
    hubus_arb_result_t r;

    /* group 0xxxxx contains 25 and 15 */
    TEST_ASSERT_TRUE(hubus_arb_decode(model_slots(pending, 4, 0, 1), 0, 1, &r));
    TEST_ASSERT_EQUAL_UINT8(25, r.winner);
    TEST_ASSERT_EQUAL_UINT8(1, r.n_masks);
    TEST_ASSERT_EQUAL_UINT8(0x00, r.masks[0].prefix); /* "00" / len 2 -> 15 */
    TEST_ASSERT_EQUAL_UINT8(2, r.masks[0].len);
}

static void test_noise_handoff_without_arbitration(void)
{
    hubus_arb_result_t r;

    /* handoff slot 3 low, no arbitration bit low */
    TEST_ASSERT_FALSE(hubus_arb_decode((uint16_t)(1u << 7), 0, 0, &r));
    TEST_ASSERT_FALSE(r.contender);
}

static void test_noise_winner_outside_prefix(void)
{
    hubus_arb_result_t r;
    /* polled group 0xxxxx but the sampled winner is 100000 */
    TEST_ASSERT_FALSE(hubus_arb_decode(1u << 0, 0, 1, &r));
    TEST_ASSERT_FALSE(r.contender);
}

static void test_noise_handoff_in_prefix_ignored(void)
{
    hubus_arb_result_t r;
    /* polled group 10xxxx; winner 101010 (arb slots 0,4,8), handoff 5 (slot 1) is inside the prefix */
    uint16_t slots = (uint16_t)((1u << 0) | (1u << 4) | (1u << 8) | (1u << 1));

    TEST_ASSERT_FALSE(hubus_arb_decode(slots, 0x20, 2, &r));
    TEST_ASSERT_TRUE(r.contender);
    TEST_ASSERT_EQUAL_UINT8(0x2A, r.winner);
    TEST_ASSERT_EQUAL_UINT8(0, r.n_masks);
}

static void test_noise_handoff_where_winner_bit_is_zero(void)
{
    hubus_arb_result_t r;
    /* winner 100000 (slot 0 low); handoff 3 (slot 7) low although the winner bit 3 is 0 */
    uint16_t slots = (uint16_t)((1u << 0) | (1u << 7));

    TEST_ASSERT_FALSE(hubus_arb_decode(slots, 0, 0, &r));
    TEST_ASSERT_TRUE(r.contender);
    TEST_ASSERT_EQUAL_UINT8(0x20, r.winner);
    TEST_ASSERT_EQUAL_UINT8(0, r.n_masks);
}

static void test_mask_stack(void)
{
    hubus_mask_stack_t s;
    hubus_mask_t m;
    unsigned i;

    hubus_mask_stack_clear(&s);
    TEST_ASSERT_FALSE(hubus_mask_stack_pop(&s, &m));
    for (i = 0; i < HUBUS_MASK_STACK_MAX; i++) {
        hubus_mask_t in = {(uint8_t)i, (uint8_t)(i % 7)};
        TEST_ASSERT_TRUE(hubus_mask_stack_push(&s, in));
    }
    TEST_ASSERT_FALSE(hubus_mask_stack_push(&s, m)); /* overflow reported */
    for (i = HUBUS_MASK_STACK_MAX; i-- > 0;) {
        TEST_ASSERT_TRUE(hubus_mask_stack_pop(&s, &m));
        TEST_ASSERT_EQUAL_UINT8(i, m.prefix); /* last in, first out */
    }
    TEST_ASSERT_FALSE(hubus_mask_stack_pop(&s, &m));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_spec_example_first_poll);
    RUN_TEST(test_spec_example_slot_pattern);
    RUN_TEST(test_spec_example_full_burst);
    RUN_TEST(test_no_contender);
    RUN_TEST(test_single_slave_every_mcid);
    RUN_TEST(test_random_bursts);
    RUN_TEST(test_prefix_poll_only_group_contends);
    RUN_TEST(test_noise_handoff_without_arbitration);
    RUN_TEST(test_noise_winner_outside_prefix);
    RUN_TEST(test_noise_handoff_in_prefix_ignored);
    RUN_TEST(test_noise_handoff_where_winner_bit_is_zero);
    RUN_TEST(test_mask_stack);
    return UNITY_END();
}
