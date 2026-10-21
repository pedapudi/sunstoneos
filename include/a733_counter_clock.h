#ifndef SUNSTONE_A733_COUNTER_CLOCK_H
#define SUNSTONE_A733_COUNTER_CLOCK_H
#include <stdatomic.h>
#include <stdint.h>
/* One private platform-lifetime object for ALL clock registry, register-lock
 * and platform now callbacks. Initialize once at EL1 before concurrent use.
 * The short IRQ-masked critical section contains no callbacks or scheduler
 * operations. Samples are serialized before reading the physical counter.
 * A ticket lock admits waiters in arrival order. The holder's history store
 * stays at least 64 bytes from the words that waiters poll, so it never
 * shares their cache line. */
struct a733_counter_clock {
    atomic_uint next, serving, failed;
    uint32_t frequency, initialized;
    unsigned char separation[52];
    uint64_t last_ticks;
};
/* expected_frequency is the established platform counter rate, not a requested
 * clock change. Native A733 uses 24 MHz. Refuse a differing CNTFRQ value. */
int a733_counter_clock_init(struct a733_counter_clock *, uint32_t expected_frequency);
/* Absolute microseconds; no origin reset. Output unchanged on failure. Any
 * observed regression, changed frequency or overflow permanently faults this
 * object, as does waiting more than 100 microseconds of counter time for the
 * serialized section. Contention alone never faults it. All clients then
 * retain their own state. */
int a733_counter_clock_now(void *, uint64_t *);
#endif
