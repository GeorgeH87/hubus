/*
 * Master-side evaluation of the 2-bit generation counter (GEN),
 * docs/spec/04-frames.md section 4.9. One tracker per MCID.
 */
#ifndef HUBUS_GEN_H
#define HUBUS_GEN_H

#include <stdint.h>

#include "hubus_proto.h"

typedef enum {
    HUBUS_GEN_BASELINE = 0,  /* first frame from this MCID since master start */
    HUBUS_GEN_NORMAL,        /* GEN = last + 1 */
    HUBUS_GEN_DUPLICATE,     /* equal GEN, identical payload: discard */
    HUBUS_GEN_WRAPPED,       /* equal GEN, different payload: new content after a wrap */
    HUBUS_GEN_GAP            /* other step: buffer contents were overwritten unread */
} hubus_gen_result_t;

typedef struct {
    uint8_t valid;
    uint8_t gen;
    uint8_t more;
    uint8_t len;
    uint8_t payload[HUBUS_MAX_PAYLOAD];
} hubus_gen_tracker_t;

void hubus_gen_reset(hubus_gen_tracker_t *t);

/*
 * Evaluates a newly received SLAVE_DATA frame and updates the tracker
 * (except for duplicates, which leave it unchanged).
 */
hubus_gen_result_t hubus_gen_eval(hubus_gen_tracker_t *t, uint8_t gen, uint8_t more, const uint8_t *payload, uint8_t len);

/* Number of buffer contents lost for a `HUBUS_GEN_GAP` between `last` and `gen`. */
static inline uint8_t hubus_gen_lost(uint8_t last, uint8_t gen)
{
    return (uint8_t)(((gen - last) & 3u) - 1u);
}

#endif /* HUBUS_GEN_H */
