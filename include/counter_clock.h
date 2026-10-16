#ifndef SUNSTONE_COUNTER_CLOCK_H
#define SUNSTONE_COUNTER_CLOCK_H
#include <stdint.h>

/* Serialized conversion of the low 56 counter bits to microseconds modulo
 * 2^64. Private state, zero-initialize once. Frequency is fixed and nonzero.
 * Samples must be less than 2^55 ticks apart. A backward or ambiguous sample
 * refuses without mutation; an entire unobserved wrap cannot be detected.
 * No polling or hardware access occurs here. Fractional ticks are preserved. */
struct counter_clock {
    uint64_t previous, microseconds, remainder;
    uint32_t frequency;
};
int counter_clock_start(struct counter_clock *, uint32_t frequency, uint64_t counter);
int counter_clock_sample(struct counter_clock *, uint64_t counter);
#endif
