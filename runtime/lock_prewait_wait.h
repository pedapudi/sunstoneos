/* SPDX-License-Identifier: Apache-2.0 */
/* The unmasked wait of runtime/lock_prewait.c, shared with the host fairness check.
 * The includer defines SS_PW_NOW() (a monotonic tick count), SS_PW_ARM(word) (an
 * acquire load of the ticket word that arms the wake-up), SS_PW_SLEEP() (wait for a
 * change to the word, or return at once) and SS_PW_DISARM(). Ticket word: owner in
 * the low halfword, next ticket in the high halfword. */
#ifndef SS_LOCK_PREWAIT_WAIT_H
#define SS_LOCK_PREWAIT_WAIT_H
#include <stdint.h>

static inline uint32_t ss_prewait_depth(uint32_t word)
{
    return ((word >> 16) - word) & 0xffffu;
}

/* Wait until at most `depth` cores hold or wait for the lock, or `limit` ticks pass.
 * Returns 1 when the limit ended the wait. Takes no ticket. */
static inline unsigned ss_prewait_run(const volatile uint32_t *word, uint32_t depth, uint64_t start, uint64_t limit,
                                      uint64_t *end)
{
    unsigned timed_out = 0;
    uint64_t now;
    for (;;) {
        SS_PW_SLEEP();
        const uint32_t value = SS_PW_ARM(word);
        now = SS_PW_NOW();
        if (ss_prewait_depth(value) <= depth) break;
        if (now - start >= limit) { timed_out = 1; break; }
    }
    SS_PW_DISARM();
    *end = now;
    return timed_out;
}
#endif
