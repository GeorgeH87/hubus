#include <string.h>

#include "hubus_crc.h"
#include "hubus_frame.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_header_bit_layout(void)
{
    uint8_t h[2];

    /* TYPE=11 | TARGET_LEN=110 | MCID=101010 | LEN=10101 */
    hubus_header_pack(h, 3, 6, 42, 21);
    TEST_ASSERT_EQUAL_HEX8(0xF5, h[0]); /* 11 110 101 */
    TEST_ASSERT_EQUAL_HEX8(0x55, h[1]); /* 010 10101 */

    /* slave: TYPE=10 | MORE=1 GEN=11 | MCID=000001 | LEN=00000 */
    hubus_header_pack(h, 2, HUBUS_SLAVE_FIELD(1, 3), 1, 0);
    TEST_ASSERT_EQUAL_HEX8(0xB8, h[0]); /* 10 111 000 */
    TEST_ASSERT_EQUAL_HEX8(0x20, h[1]);        /* 001 00000 */
}

static void test_header_roundtrip_exhaustive(void)
{
    unsigned type, field, mcid, len;

    for (type = 0; type < 4; type++) {
        for (field = 0; field < 8; field++) {
            for (mcid = 0; mcid < 64; mcid++) {
                for (len = 0; len < 32; len++) {
                    uint8_t b[2];
                    hubus_header_t h;

                    hubus_header_pack(b, (uint8_t)type, (uint8_t)field, (uint8_t)mcid, (uint8_t)len);
                    hubus_header_unpack(&h, b);
                    TEST_ASSERT_EQUAL_UINT8(type, h.type);
                    TEST_ASSERT_EQUAL_UINT8(field, h.field);
                    TEST_ASSERT_EQUAL_UINT8(mcid, h.mcid);
                    TEST_ASSERT_EQUAL_UINT8(len, h.len);
                }
            }
        }
    }
}

static void test_slave_field_accessors(void)
{
    hubus_header_t h;
    uint8_t b[2];

    hubus_header_pack(b, HUBUS_S_DATA, HUBUS_SLAVE_FIELD(1, 2), 9, 3);
    hubus_header_unpack(&h, b);
    TEST_ASSERT_EQUAL_UINT8(1, HUBUS_HDR_MORE(&h));
    TEST_ASSERT_EQUAL_UINT8(2, HUBUS_HDR_GEN(&h));
    hubus_header_pack(b, HUBUS_S_DATA, HUBUS_SLAVE_FIELD(0, 3), 9, 3);
    hubus_header_unpack(&h, b);
    TEST_ASSERT_EQUAL_UINT8(0, HUBUS_HDR_MORE(&h));
    TEST_ASSERT_EQUAL_UINT8(3, HUBUS_HDR_GEN(&h));
}

static void test_encode_decode(void)
{
    uint8_t buf[HUBUS_MAX_FRAME_BYTES];
    uint8_t payload[HUBUS_MAX_PAYLOAD];
    hubus_header_t h;
    unsigned len;

    for (len = 0; len <= HUBUS_MAX_PAYLOAD; len++) {
        size_t n;
        unsigned i;

        for (i = 0; i < len; i++) {
            payload[i] = (uint8_t)(0xA0 + i);
        }
        n = hubus_frame_encode(buf, HUBUS_M_SEND_COMMAND, 6, 33, payload, (uint8_t)len);
        TEST_ASSERT_EQUAL_UINT(3 + len, n);
        TEST_ASSERT_TRUE(hubus_frame_decode(buf, n, &h));
        TEST_ASSERT_EQUAL_UINT8(len, h.len);
        TEST_ASSERT_EQUAL_UINT8(33, h.mcid);
        if (len) {
            TEST_ASSERT_EQUAL_MEMORY(payload, buf + 2, len);
        }
        /* CRC covers header and payload */
        TEST_ASSERT_EQUAL_HEX8(hubus_crc8(buf, n - 1), buf[n - 1]);
    }
}

static void test_encode_rejects_oversized_payload(void)
{
    uint8_t buf[64];
    uint8_t payload[32] = {0};

    TEST_ASSERT_EQUAL_UINT(0, hubus_frame_encode(buf, 3, 0, 1, payload, 32));
}

static void test_decode_rejects_bad_length_and_crc(void)
{
    uint8_t buf[HUBUS_MAX_FRAME_BYTES];
    uint8_t payload[4] = {1, 2, 3, 4};
    size_t n = hubus_frame_encode(buf, HUBUS_M_SEND_COMMAND, 6, 5, payload, 4);

    TEST_ASSERT_TRUE(hubus_frame_decode(buf, n, NULL));
    TEST_ASSERT_FALSE(hubus_frame_decode(buf, n - 1, NULL));
    TEST_ASSERT_FALSE(hubus_frame_decode(buf, n + 1, NULL));
    TEST_ASSERT_FALSE(hubus_frame_decode(buf, 2, NULL));
    buf[n - 1] ^= 0x01;
    TEST_ASSERT_FALSE(hubus_frame_decode(buf, n, NULL));
}

/* Hamming distance 4 up to 14 covered bytes: no 1-, 2-, or 3-bit error is
 * accepted (docs/MESSAGE_FORMAT.md section 7). Exhaustive for a frame with a
 * 12-byte payload (header + payload = 14 bytes + CRC). */
static void test_hamming_distance_4_for_12_byte_payload(void)
{
    uint8_t good[HUBUS_MAX_FRAME_BYTES];
    uint8_t payload[12];
    size_t n;
    size_t bits;
    size_t a, b, c;
    unsigned i;

    for (i = 0; i < sizeof payload; i++) {
        payload[i] = (uint8_t)(i * 29u + 3u);
    }
    n = hubus_frame_encode(good, HUBUS_S_DATA, 0, 17, payload, sizeof payload);
    bits = n * 8;

    for (a = 0; a < bits; a++) {
        uint8_t t[HUBUS_MAX_FRAME_BYTES];

        memcpy(t, good, n);
        t[a / 8] ^= (uint8_t)(0x80u >> (a % 8));
        TEST_ASSERT_FALSE(hubus_frame_decode(t, n, NULL));
        for (b = a + 1; b < bits; b++) {
            uint8_t t2[HUBUS_MAX_FRAME_BYTES];

            memcpy(t2, t, n);
            t2[b / 8] ^= (uint8_t)(0x80u >> (b % 8));
            TEST_ASSERT_FALSE(hubus_frame_decode(t2, n, NULL));
            for (c = b + 1; c < bits; c++) {
                uint8_t t3[HUBUS_MAX_FRAME_BYTES];

                memcpy(t3, t2, n);
                t3[c / 8] ^= (uint8_t)(0x80u >> (c % 8));
                TEST_ASSERT_FALSE(hubus_frame_decode(t3, n, NULL));
            }
        }
    }
}

/* The LEN field is part of the header, so a flipped LEN bit changes the frame
 * length that the receiver expects and the CRC position. */
static void test_len_bit_flip_detected(void)
{
    uint8_t buf[HUBUS_MAX_FRAME_BYTES];
    uint8_t payload[8] = {0};
    size_t n = hubus_frame_encode(buf, HUBUS_S_DATA, 0, 9, payload, sizeof payload);
    unsigned bit;

    for (bit = 0; bit < 5; bit++) {
        buf[1] ^= (uint8_t)(1u << bit);
        TEST_ASSERT_FALSE(hubus_frame_decode(buf, n, NULL));
        buf[1] ^= (uint8_t)(1u << bit);
    }
}

static void test_match_exhaustive_against_bit_strings(void)
{
    unsigned own, prefix, tlen;

    for (own = 1; own < 64; own++) {
        for (prefix = 0; prefix < 64; prefix++) {
            for (tlen = 0; tlen <= 6; tlen++) {
                unsigned expect = 1;
                unsigned k;

                for (k = 0; k < tlen; k++) { /* compare bit by bit, MSB first */
                    unsigned bit = 5 - k;
                    if (((own >> bit) & 1u) != ((prefix >> bit) & 1u)) {
                        expect = 0;
                    }
                }
                TEST_ASSERT_EQUAL_UINT(expect, hubus_mcid_match((uint8_t)own, (uint8_t)prefix, (uint8_t)tlen));
            }
        }
    }
}

static void test_match_edge_cases(void)
{
    TEST_ASSERT_TRUE(hubus_mcid_match(42, 0, 0));      /* broadcast */
    TEST_ASSERT_TRUE(hubus_mcid_match(42, 42, 6));     /* exact */
    TEST_ASSERT_FALSE(hubus_mcid_match(42, 43, 6));
    TEST_ASSERT_TRUE(hubus_mcid_match(0x35, 0x34, 5)); /* 11010x */
    TEST_ASSERT_FALSE(hubus_mcid_match(42, 42, 7));    /* TARGET_LEN above 6 never matches */
    TEST_ASSERT_FALSE(hubus_mcid_match(42, 42, 255));
}

static void test_mcid_zero_is_reserved(void)
{
    unsigned tlen;

    /* A frame addressed to MCID 0 matches no slave, and a slave cannot have MCID 0 */
    for (tlen = 0; tlen <= 6; tlen++) {
        TEST_ASSERT_FALSE(hubus_mcid_match(0, 0, (uint8_t)tlen));
    }
    TEST_ASSERT_FALSE(hubus_mcid_match(1, 0, 6));
    TEST_ASSERT_FALSE(hubus_mcid_match(64, 0, 0)); /* out of range */
}

static void test_prefix_mask(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x00, hubus_prefix_mask(0));
    TEST_ASSERT_EQUAL_HEX8(0x20, hubus_prefix_mask(1));
    TEST_ASSERT_EQUAL_HEX8(0x30, hubus_prefix_mask(2));
    TEST_ASSERT_EQUAL_HEX8(0x3E, hubus_prefix_mask(5));
    TEST_ASSERT_EQUAL_HEX8(0x3F, hubus_prefix_mask(6));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_header_bit_layout);
    RUN_TEST(test_header_roundtrip_exhaustive);
    RUN_TEST(test_slave_field_accessors);
    RUN_TEST(test_encode_decode);
    RUN_TEST(test_encode_rejects_oversized_payload);
    RUN_TEST(test_decode_rejects_bad_length_and_crc);
    RUN_TEST(test_hamming_distance_4_for_12_byte_payload);
    RUN_TEST(test_len_bit_flip_detected);
    RUN_TEST(test_match_exhaustive_against_bit_strings);
    RUN_TEST(test_match_edge_cases);
    RUN_TEST(test_mcid_zero_is_reserved);
    RUN_TEST(test_prefix_mask);
    return UNITY_END();
}
