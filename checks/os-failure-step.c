/* Host model of the supervisor's side of a failure return of an application that fails with its NPU queue open (apps/os/cpu-return-step.h,
 * driven the way os_cpu_return_supervise and bounded() drive it) against a model of the application's NPU owner and of the NPU client.
 *
 * The claim of the failure only marks the application finished and publishes `failing` and the time. The application's NPU owner polls
 * ss_live() every 100 ms and closes its queue when it is -1; the NPU client then publishes its report (40 ms) and the devices are ready.
 * A client that is stopped from outside while the owner has not closed (a supervisor that returns OS_FAILURE_STEP_STOP_NPU) takes the
 * client's retained path: a device failure, which sets board.fatal after 20 ms, and the NPU records a first failure at source line 0x28b.
 *
 * The run ends at a clean return, or at the EL3 stop with the detail the supervisor gives it. */
#include "cpu-return-step.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIMIT 3000000u     /* OS_FAILURE_RETURN_US */
#define DEADLINE 60000000u
struct result { int returned; unsigned detail; unsigned return_us; };

/* owner_poll_us: when the owner sees ss_live() == -1 and closes (0: never); fatal_us: a device failure at that time after the claim (0: none) */
static struct result run(int normal_return, unsigned npu_open, unsigned owner_poll_us, unsigned fatal_us, uint64_t claim_us)
{
    struct result r = {0, 0, 0};
    int failing = 0, requested = 0, retired = !npu_open, forced = 0, thermal_done = 0;
    uint64_t since = 0, forced_at = 0;
    int fatal = 0, npu_record = 0;
    for (uint64_t now = 0; now < 8000000; now += 10000) {
        if (now == claim_us) { failing = !normal_return; requested = 1; since = now; }
        if (!normal_return && failing && npu_open && !retired && owner_poll_us && now - since >= owner_poll_us + 40000) retired = 1;
        if (fatal_us && failing && now - since >= fatal_us) fatal = npu_record = 1;
        if (forced && !retired && now - forced_at >= 20000) fatal = npu_record = 1;            /* the retained path */
        /* the thermal owner, once the return is requested and the clock is lowered (10 ms after the claim): exits at once when the devices are not
         * retired and no wait is allowed; the exit closes admission (board.fatal) */
        if (requested && now - since >= 10000 && !thermal_done) {
            /* the thermal owner's pass calls the clock owner's step: a pass after the lowering is done must do nothing */
            if (!retired && failing && os_cpu_return_clock_pass(1)) fatal = 1;     /* a pass while waiting */
            switch (os_cpu_return_thermal_step(retired, failing)) {
            case OS_THERMAL_STEP_EXIT: fatal = 1; break;
            case OS_THERMAL_STEP_WAIT: break;
            case OS_THERMAL_STEP_STOP: thermal_done = 1; break;
            }
        }
        const unsigned detail = os_cpu_return_bounded_detail(fatal, now, DEADLINE, DEADLINE, 0, npu_record ? 0x28b : 0);
        if (detail) { r.detail = detail; return r; }                              /* bounded(): the EL3 stop */
        switch (os_cpu_return_failure_step(failing, requested, requested ? retired : 1, now, since, LIMIT)) {
        case OS_FAILURE_STEP_TIMEOUT: r.detail = 0x742; return r;
        case OS_FAILURE_STEP_DEVICES: r.detail = 0x741; return r;
        case OS_FAILURE_STEP_STOP_NPU: if (!forced) { forced = 1; forced_at = now; } continue;
        case OS_FAILURE_STEP_WAIT: continue;
        case OS_FAILURE_STEP_NONE: break;
        }
        if (requested && retired && thermal_done) { r.returned = 1; r.return_us = (unsigned)(now - since); return r; }
    }
    r.detail = 0x742;
    return r;
}

static int expect(const char *what, struct result r, int returned, unsigned detail)
{
    if (r.returned != returned || (!returned && r.detail != detail)) {
        fprintf(stderr, "FAIL: %s: returned %d detail %#x (wanted returned %d detail %#x)\n", what, r.returned, r.detail, returned, detail);
        return 1;
    }
    return 0;
}

int main(void)
{
    /* the owner closes within 100 ms of the failure, wherever in the supervisor's 10 ms cycle it comes: a clean return within 300 ms */
    for (unsigned ms = 0; ms < 40; ++ms) {
        struct result r = run(0, 1, 100000, 0, 50000 + ms * 10000);
        if (expect("owner closes", r, 1, 0) || r.return_us > 300000) { fprintf(stderr, "FAIL: owner closes: %u us\n", r.return_us); return 1; }
    }
    /* an owner that never closes: the return waits the whole 3 s and ends at the watchdog (0x742), not at once with 0x741 */
    struct result hung = run(0, 1, 0, 0, 50000);
    if (expect("owner never closes", hung, 0, 0x742)) return 1;
    /* a device failure during the return (board.fatal): the stop is by design (the owner retains its resources); the detail names the NPU's line */
    if (expect("device failure with an NPU record", run(0, 1, 100000, 30000, 50000), 0, 0x800 | 0x28b)) return 1;
    /* the thermal owner waits for the retirement in a failure return (it exited at once before, which is the stop 0x744 of the NPU failure test) */
    if (expect("thermal owner waits", run(0, 1, 100000, 0, 50000), 1, 0)) return 1;
    /* no failure, no NPU open: nothing to wait for */
    if (expect("NPU already retired", run(0, 0, 0, 0, 50000), 1, 0)) return 1;
    /* a normal return that finds the NPU open is a defect: stop at once (0x741) */
    if (expect("normal return, NPU open", run(1, 1, 0, 0, 50000), 0, 0x741)) return 1;
    /* bounded(): the details */
    if (os_cpu_return_bounded_detail(0, 100, DEADLINE, DEADLINE, 0, 0) != 0) { fprintf(stderr, "FAIL: live\n"); return 1; }
    if (os_cpu_return_bounded_detail(1, 100, DEADLINE, DEADLINE, 0, 0) != 0x744) { fprintf(stderr, "FAIL: fatal\n"); return 1; }
    if (os_cpu_return_bounded_detail(0, DEADLINE, DEADLINE, DEADLINE, 0, 0) != 0x736) { fprintf(stderr, "FAIL: deadline\n"); return 1; }
    if (os_cpu_return_bounded_detail(0, 100, DEADLINE, DEADLINE, 1, 0) != 0x736) { fprintf(stderr, "FAIL: late\n"); return 1; }
    puts("os-failure-step host model passed");
    return 0;
}
