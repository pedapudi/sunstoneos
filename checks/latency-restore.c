/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

void __wrap_ss_latency_mask_close(unsigned, uintptr_t);
void ss_latency_exception_enter(unsigned);
void ss_latency_exception_return(unsigned, uint64_t, uint64_t);
static bool el1, active;
static uint64_t flags, now, begin, duration;
static unsigned daif_reads, records;
bool ss_restore_test_el1(void) { return el1; }
uint64_t ss_restore_test_daif(void) { assert(el1); ++daif_reads; return flags; }
uint64_t ss_latency_now(void) { return now; }
void ss_latency_mask_open(unsigned core, uint64_t start, uintptr_t site)
{ (void)site; assert(!core); if (!active) { begin = start; active = true; } }
void __real_ss_latency_mask_close(unsigned core, uintptr_t site)
{ (void)site; assert(!core); if (active) { duration = now - begin; ++records; active = false; } }
int main(void)
{
    now = 10; ss_latency_exception_enter(0);
    now = 20; ss_latency_exception_enter(0); /* an earlier outer origin survives */
    __wrap_ss_latency_mask_close(0, 1); /* EL0 unmask waits for SVC return */
    assert(!daif_reads && !records);
    el1 = true; flags = 0x80;
    now = 30; __wrap_ss_latency_mask_close(0, 2);
    assert(daif_reads == 1 && !records);
    ss_latency_exception_return(0, 0x80, 255); /* restored I remains masked */
    ss_latency_exception_return(0, 0, 128); /* restored API PMR remains masked */
    assert(!records && active);
    now = 50; ss_latency_exception_return(0, 0, 255);
    assert(records == 1 && duration == 40 && !active);
    now = 60; ss_latency_exception_enter(0);
    flags = 0; now = 70; __wrap_ss_latency_mask_close(0, 3);
    assert(records == 2 && duration == 10);
    now = 80; ss_latency_exception_enter(0);
    el1 = false; now = 90; __wrap_ss_latency_mask_close(0, 4);
    assert(daif_reads == 2 && records == 2);
    now = 100; ss_latency_exception_return(0, 0, 255);
    assert(records == 3 && duration == 20);
    puts("PASS restore spans: EL0 guard, nested origin, restored I/PMR and one contiguous interval");
}
