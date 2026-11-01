/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_CHECK_REGISTER_LATENCY_H
#define SUNSTONE_CHECK_REGISTER_LATENCY_H
#include <stdint.h>

/* One deferred hardware effect. Time uses the fixture's explicit counter
 * units of a monotonic uint64_t counter; subtraction permits one wrap.
 * Reads do not advance it. Keep the visible busy/stale bits in the
 * fixture word until apply() completes. Separate effects can share a word
 * when, for example, UPDATE clears before LOCK asserts. */
struct register_latency {
    uint64_t start, delay;
    uint32_t mask, value;
    unsigned pending, stuck;
};

static inline void reg_latency_schedule(struct register_latency *e,
    uint64_t now, uint64_t delay, uint32_t mask, uint32_t value)
{
    *e=(struct register_latency){.start=now,.delay=delay,.mask=mask,
        .value=value,.pending=1};
}

static inline uint32_t reg_latency_apply(struct register_latency *e,
    uint64_t now, uint32_t visible)
{
    if(e->pending && !e->stuck && now-e->start>=e->delay) {
        e->pending=0;
        return (visible&~e->mask)|(e->value&e->mask);
    }
    return visible;
}
#endif
