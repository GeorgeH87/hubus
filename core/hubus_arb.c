#include "hubus_arb.h"

#include "hubus_frame.h"

uint8_t hubus_arb_decode(uint16_t slots, uint8_t base_prefix, uint8_t base_len, hubus_arb_result_t *r)
{
    uint8_t winner = 0;
    uint8_t any = 0;
    uint8_t ok = 1;
    uint8_t k;

    r->contender = 0;
    r->winner = 0;
    r->n_masks = 0;

    for (k = HUBUS_MCID_BITS; k-- > 0;) {
        uint8_t slot = (uint8_t)(2u * (HUBUS_MCID_BITS - 1u - k));
        if (slots & (1u << slot)) {
            winner |= (uint8_t)(1u << k);
            any = 1;
        }
    }

    if (!any) {
        /* No contender. Low handoff slots without arbitration bits are noise. */
        return (uint8_t)(slots == 0);
    }
    if (base_len > HUBUS_TARGET_LEN_EXACT ||
        ((winner ^ base_prefix) & hubus_prefix_mask(base_len)) != 0) {
        return 0; /* winner outside the polled group: noise */
    }

    for (k = HUBUS_MCID_BITS; k-- > 0;) {
        uint8_t slot = (uint8_t)(2u * (HUBUS_MCID_BITS - 1u - k) + 1u);
        uint8_t tlen;

        if (!(slots & (1u << slot))) {
            continue;
        }
        tlen = (uint8_t)(HUBUS_MCID_BITS - k);
        if (tlen <= base_len || !(winner & (1u << k))) {
            ok = 0; /* handoff inside the prefix, or loser bit not dominated */
            continue;
        }
        r->masks[r->n_masks].prefix = (uint8_t)(winner & hubus_prefix_mask(tlen) & ~(1u << k));
        r->masks[r->n_masks].len = tlen;
        r->n_masks++;
    }

    r->contender = 1;
    r->winner = winner;
    return ok;
}

void hubus_mask_stack_clear(hubus_mask_stack_t *s)
{
    s->n = 0;
}

uint8_t hubus_mask_stack_push(hubus_mask_stack_t *s, hubus_mask_t m)
{
    if (s->n >= HUBUS_MASK_STACK_MAX) {
        return 0;
    }
    s->item[s->n++] = m;
    return 1;
}

uint8_t hubus_mask_stack_pop(hubus_mask_stack_t *s, hubus_mask_t *m)
{
    if (s->n == 0) {
        return 0;
    }
    *m = s->item[--s->n];
    return 1;
}
