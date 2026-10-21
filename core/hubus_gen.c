#include "hubus_gen.h"

void hubus_gen_reset(hubus_gen_tracker_t *t)
{
    t->valid = 0;
}

hubus_gen_result_t hubus_gen_eval(hubus_gen_tracker_t *t, uint8_t gen, uint8_t more, const uint8_t *payload, uint8_t len)
{
    hubus_gen_result_t res;
    uint8_t i;

    gen &= 3u;
    if (!t->valid) {
        res = HUBUS_GEN_BASELINE;
    } else if (gen == (uint8_t)((t->gen + 1u) & 3u)) {
        res = HUBUS_GEN_NORMAL;
    } else if (gen == t->gen) {
        uint8_t same = (uint8_t)(len == t->len);
        for (i = 0; same && i < len; i++) {
            same = (uint8_t)(payload[i] == t->payload[i]);
        }
        if (same) {
            return HUBUS_GEN_DUPLICATE;
        }
        res = HUBUS_GEN_WRAPPED;
    } else {
        res = HUBUS_GEN_GAP;
    }

    t->valid = 1;
    t->gen = gen;
    t->more = more ? 1u : 0u;
    t->len = len;
    for (i = 0; i < len && i < HUBUS_MAX_PAYLOAD; i++) {
        t->payload[i] = payload[i];
    }
    return res;
}
