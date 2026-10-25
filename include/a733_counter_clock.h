#ifndef SUNSTONE_A733_COUNTER_CLOCK_H
#define SUNSTONE_A733_COUNTER_CLOCK_H
#include <stdatomic.h>
#include <stdint.h>
/* One private platform-lifetime object for ALL clock registry, register-lock
 * and platform now callbacks. Initialize once at EL1 before concurrent use.
 * Callers read the physical counter without a lock or interrupt mask, after an
 * instruction barrier. The shared history of the largest sample only rises,
 * through compare-and-swap, so every caller observes nondecreasing time. The
 * history stays at least 64 bytes from the fault flag that every caller reads,
 * so the two never share a cache line. */
struct a733_counter_clock {
    atomic_uint failed;
    uint32_t frequency, initialized;
    unsigned char separation[52];
    _Atomic uint64_t last_ticks;
};
/* expected_frequency is the established platform counter rate, not a requested
 * clock change. Native A733 uses 24 MHz. Refuse a differing CNTFRQ value. */
int a733_counter_clock_init(struct a733_counter_clock *, uint32_t expected_frequency);
/* Absolute microseconds; no origin reset. Output unchanged on failure. A
 * sample more than 100 microseconds of counter time below the history, a
 * changed frequency or an overflow permanently faults this object. A smaller
 * backward step is skew between cores and reports the history. Contention
 * never faults it. All clients then retain their own state. */
int a733_counter_clock_now(void *, uint64_t *);
#endif
