#include "counter_clock.h"
#define COUNTER_MASK ((UINT64_C(1) << 56) - 1)

int counter_clock_start(struct counter_clock *c, uint32_t frequency, uint64_t counter)
{
    if (!c || c->frequency || !frequency) return 0;
    *c = (struct counter_clock){.previous = counter & COUNTER_MASK, .frequency = frequency};
    return 1;
}
int counter_clock_sample(struct counter_clock *c, uint64_t counter)
{
    if (!c || !c->frequency) return 0;
    counter &= COUNTER_MASK;
    uint64_t elapsed = (counter - c->previous) & COUNTER_MASK;
    if (elapsed >= (UINT64_C(1) << 55)) return 0;
    uint64_t fraction = (elapsed % c->frequency) * UINT64_C(1000000) + c->remainder;
    c->microseconds += (elapsed / c->frequency) * UINT64_C(1000000) + fraction / c->frequency;
    c->remainder = fraction % c->frequency;
    c->previous = counter;
    return 1;
}
