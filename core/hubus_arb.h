/*
 * Master-side arbitration helpers (docs/ARBITRATION.md section 6):
 * derive the winner and the pending masks from the 12 sampled arbitration
 * slots, and keep the last-in-first-out pending mask stack.
 */
#ifndef HUBUS_ARB_H
#define HUBUS_ARB_H

#include <stdint.h>

#include "hubus_proto.h"

/* One pending mask: MCID prefix (MSB-aligned, unused low bits 0) + length. */
typedef struct {
    uint8_t prefix;
    uint8_t len;
} hubus_mask_t;

/* Result of one arbitration. */
typedef struct {
    uint8_t contender;                /* 1 if a winner was found */
    uint8_t winner;                   /* winner MCID */
    uint8_t n_masks;                  /* number of derived masks */
    hubus_mask_t masks[HUBUS_MCID_BITS]; /* in push order: bit 5 first, bit 0 last */
} hubus_arb_result_t;

/*
 * `slots` holds the 12 sampled slots, slot 0 in bit 0 (first slot after the
 * turnaround slot). A set bit means DATA was low. Even slots are arbitration
 * bits 5..0, odd slots the handoff slot after each of them.
 *
 * `base_prefix`/`base_len` is the mask of the POLL_PENDING that produced the
 * slots. Returns 1 if the slots are plausible. Implausible slot patterns
 * (winner outside the polled prefix, handoff slot low where the winner bit is
 * 0, handoff inside the polled prefix, handoff without any arbitration bit)
 * are treated as noise: they are ignored and 0 is returned, `contender` tells
 * whether a usable winner remains.
 */
uint8_t hubus_arb_decode(uint16_t slots, uint8_t base_prefix, uint8_t base_len, hubus_arb_result_t *r);

#define HUBUS_MASK_STACK_MAX 8u

typedef struct {
    hubus_mask_t item[HUBUS_MASK_STACK_MAX];
    uint8_t n;
} hubus_mask_stack_t;

void hubus_mask_stack_clear(hubus_mask_stack_t *s);
/* Returns 0 on overflow (only possible with corrupted slot data). */
uint8_t hubus_mask_stack_push(hubus_mask_stack_t *s, hubus_mask_t m);
uint8_t hubus_mask_stack_pop(hubus_mask_stack_t *s, hubus_mask_t *m);

#endif /* HUBUS_ARB_H */
