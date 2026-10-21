#include "hubus_frame.h"

#include "hubus_crc.h"

void hubus_header_pack(uint8_t out[2], uint8_t type, uint8_t field, uint8_t mcid, uint8_t len)
{
    out[0] = (uint8_t)(((type & 3u) << 6) | ((field & 7u) << 3) | ((mcid >> 3) & 7u));
    out[1] = (uint8_t)(((mcid & 7u) << 5) | (len & 0x1Fu));
}

void hubus_header_unpack(hubus_header_t *h, const uint8_t in[2])
{
    h->type = (uint8_t)(in[0] >> 6);
    h->field = (uint8_t)((in[0] >> 3) & 7u);
    h->mcid = (uint8_t)(((in[0] & 7u) << 3) | (in[1] >> 5));
    h->len = (uint8_t)(in[1] & 0x1Fu);
}

uint8_t hubus_mcid_match(uint8_t own_mcid, uint8_t prefix, uint8_t target_len)
{
    if (own_mcid < HUBUS_MCID_MIN || own_mcid > HUBUS_MCID_MAX || target_len > HUBUS_TARGET_LEN_EXACT) {
        return 0;
    }
    return (uint8_t)(((own_mcid ^ prefix) & hubus_prefix_mask(target_len)) == 0);
}

size_t hubus_frame_encode(uint8_t *out, uint8_t type, uint8_t field, uint8_t mcid, const uint8_t *payload, uint8_t len)
{
    uint8_t i;
    uint8_t crc;

    if (len > HUBUS_MAX_PAYLOAD) {
        return 0;
    }
    hubus_header_pack(out, type, field, mcid, len);
    for (i = 0; i < len; i++) {
        out[HUBUS_HEADER_BYTES + i] = payload[i];
    }
    crc = hubus_crc8(out, HUBUS_HEADER_BYTES + (size_t)len);
    out[HUBUS_HEADER_BYTES + len] = crc;
    return HUBUS_HEADER_BYTES + (size_t)len + 1u;
}

uint8_t hubus_frame_decode(const uint8_t *buf, size_t n, hubus_header_t *h)
{
    hubus_header_t tmp;

    if (n < HUBUS_HEADER_BYTES + 1u) {
        return 0;
    }
    hubus_header_unpack(&tmp, buf);
    if (n != HUBUS_HEADER_BYTES + (size_t)tmp.len + 1u) {
        return 0;
    }
    if (hubus_crc8(buf, n - 1u) != buf[n - 1u]) {
        return 0;
    }
    if (h) {
        *h = tmp;
    }
    return 1;
}
