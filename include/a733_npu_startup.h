#ifndef SUNSTONE_A733_NPU_STARTUP_H
#define SUNSTONE_A733_NPU_STARTUP_H
/* The NPU startup sequencer (docs/designs/npu-board.md). One bootstrap task
 * calls a733_npu_startup_step() once per tick until it returns READY. Each
 * call runs steps until one waits on time or on another task:
 *
 * 1. one clock-registry token (exclusive PLL_NPU and NPU, shared fabric,
 *    reference, oscillator and PERI0 when selected), one DCDC2 rail token and
 *    one stable OSC/PCK lease from the image's GPU/PCK resource manager;
 * 2. supply: the power-authority word shows DCDC2 verified with no fault;
 * 3. under one register-lock acquisition, the domain-4 check, the register
 *    pass and the PLL_NPU enable table; LOCK is then sampled without the lock;
 * 4. under a second acquisition, the local open table;
 * 5. the domain-4 row request, answered by the power-domain owner;
 * 6. a 2 ms settle from a counter sample taken when the row reads ON, then the
 *    final readback under a third acquisition.
 *
 * Every wait has a counter deadline. A refusal before the first NPU register
 * write releases the tokens; any later failure retains them, the row and the
 * register lock if a table stopped midway. */
#include "a733_counter_clock.h"
#include "a733_gpu_resources.h"
#include "a733_npu_platform.h"
#include "a733_npu_pll.h"
#include "a733_platform_locks.h"
#include "a733_power_domains.h"
#include "a733_rail_leases.h"
#include "FreeRTOS.h"
#include "task.h"

/* The image's providers, borrowed for the startup's lifetime. pck is the
 * image's only GPU/PCK resource manager; rails is the ledger bound to it and
 * carries the power-authority word; domains is the power-domain owner. */
struct a733_npu_startup_providers {
    struct a733_counter_clock *counter;
    struct a733_clock_leases *clocks;
    struct a733_platform_locks *locks;
    struct a733_gpu_resource_manager *pck;
    struct a733_rail_leases *rails;
    struct a733_power_domains *domains;
};
/* owner: the clock, rail and row owner. deadline_us: the session end in the
 * counter's microseconds. pck_client: the image's PCK client of the NPU. */
struct a733_npu_startup_config {
    struct a733_npu_platform_config fabric;
    uint64_t owner, deadline_us;
    unsigned pck_client;
};
/* Register, time and lock callbacks. NULL selects the native ones: the NPU
 * allowlist adapter, the counter clock, CNTPCT_EL0 and the register lock.
 * now gives monotonic microseconds for deadlines; spin_us gives raw counter
 * microseconds for LOCK spacing. enter returns 1 held, 0 busy, -1 uncertain.
 * retain marks the held register lock retained after a sequence stopped
 * midway; the lock is never released afterwards. */
struct a733_npu_startup_ops {
    struct a733_npu_register_ops registers;
    int (*now)(void *, uint64_t *);
    uint64_t (*spin_us)(void *);
    int (*enter)(void *, uint64_t deadline_us);
    void (*leave)(void *);
    void (*retain)(void *);
};
enum { A733_NPU_STARTUP_TOKENS, A733_NPU_STARTUP_SUPPLY, A733_NPU_STARTUP_ENABLE,
    A733_NPU_STARTUP_OPEN, A733_NPU_STARTUP_ROW, A733_NPU_STARTUP_SETTLE,
    A733_NPU_STARTUP_FINAL, A733_NPU_STARTUP_READY, A733_NPU_STARTUP_STOPPED };
struct a733_npu_startup {
    struct a733_npu_startup_config config;
    struct a733_npu_startup_providers providers;
    struct a733_npu_startup_ops ops;
    void *cookie;
    /* The row's request field is written by the bootstrap task and its answer
     * field by the power service task (a733_power_domain_row.h). */
    struct a733_power_domain_row row;
    struct a733_clock_token clock;
    struct a733_rail_token rail;
    struct a733_gpu_resource_lease pck;
    TaskHandle_t task;
    a733_clock_mask_t shared;
    uint64_t last_now, wait_until;
    unsigned initialized, stage, failed, register_held, lock_samples;
    /* Work counters, read by the host checks. */
    unsigned calls, reads, writes, clock_reads, lock_enters;
};
/* EL1, before the scheduler starts and before the power service first runs:
 * copies the configuration and providers and registers the domain-4 row. */
int a733_npu_startup_init(struct a733_npu_startup *, const struct a733_npu_startup_config *,
    const struct a733_npu_startup_providers *, const struct a733_npu_startup_ops *, void *cookie);
int a733_npu_startup_bind_task(struct a733_npu_startup *, TaskHandle_t);
/* The bound EL1 bootstrap task only. READY once the NPU is powered, clocked
 * and out of reset; PENDING while a wait is open; RETAINED after a failure. */
enum a733_npu_platform_result a733_npu_startup_step(struct a733_npu_startup *);
/* After a successful controller CLOSE, the bound task only: the local stop
 * table in one locked pass. READY when stopped. */
enum a733_npu_platform_result a733_npu_startup_stop(struct a733_npu_startup *);
int a733_npu_startup_ready(const struct a733_npu_startup *);
/* The service activation check, on the bound task: 1 when READY and no
 * shared provider has failed, 0 before READY, -1 after a failure or from
 * another task. No register access. */
int a733_npu_startup_prepared(struct a733_npu_startup *);
#endif
