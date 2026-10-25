#ifndef SUNSTONE_A733_NPU_STARTUP_H
#define SUNSTONE_A733_NPU_STARTUP_H
/* The NPU startup sequencer: docs/npu-startup.md and docs/designs/npu-board.md. */
#include <stdatomic.h>
#include "a733_counter_clock.h"
#include "a733_gpu_resources.h"
#include "a733_npu_platform.h"
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
 * midway; the lock is never released afterwards. wake, optional, is called
 * by the power service when it answers the domain-4 row or publishes the
 * supply result; the native one notifies the bound task. */
struct a733_npu_startup_ops {
    struct a733_npu_register_ops registers;
    int (*now)(void *, uint64_t *);
    uint64_t (*spin_us)(void *);
    int (*enter)(void *, uint64_t deadline_us);
    void (*leave)(void *);
    void (*retain)(void *);
    void (*wake)(void *);
};
/* Stages in the order of the board's vendor kernel: domain 4 ON, then PLL_NPU,
 * then the shared SRAM routed to the NPU and the local clocks and resets. */
enum { A733_NPU_STARTUP_TOKENS, A733_NPU_STARTUP_SUPPLY, A733_NPU_STARTUP_PASS,
    A733_NPU_STARTUP_ROW, A733_NPU_STARTUP_ENABLE, A733_NPU_STARTUP_OPEN,
    A733_NPU_STARTUP_SETTLE, A733_NPU_STARTUP_FINAL, A733_NPU_STARTUP_READY,
    A733_NPU_STARTUP_STOPPED };
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
    unsigned initialized, stage, failed, register_held;
    uint32_t pll_inherited; /* Reserved BIT24 captured once before any write; never programmed. */
    /* 1 when the register pass found PLL_NPU locked at the profile with its
     * output gate open, as an earlier startup in this power lifetime left it. */
    unsigned pll_adopted;
    /* 1 once PLL_NPU has passed its LOCK samples. Bootstrap task only. */
    unsigned pll_locked;
    /* The selected profile's factor field: 492 MHz unless
     * a733_npu_startup_rate selected 852 MHz. Fixed before the scheduler. */
    uint32_t pll_factors;
    /* 1 when the register pass found PLL_NPU locked at the other profile with
     * its output gate open; the enable stage then changes its rate. */
    unsigned pll_retune;
    /* Counter microseconds of the first step and of READY. Written by the
     * bootstrap task; readable by a client once the service is active. */
    uint64_t entry_us, ready_us;
    /* 1 from the readback that shows the shared SRAM routed to the NPU until
     * local stop routes it back. Bootstrap task only. */
    unsigned sram_routed;
    /* 1 from READY until stop or a failure: the only state in which the NPU
     * register window may be accessed. Written by the bootstrap task with
     * release order; read by the service worker before each access. */
    atomic_uint window_open;
};
/* EL1, before the scheduler starts and before the power service first runs:
 * copies the configuration and providers and registers the domain-4 row. */
int a733_npu_startup_init(struct a733_npu_startup *, const struct a733_npu_startup_config *,
    const struct a733_npu_startup_providers *, const struct a733_npu_startup_ops *, void *cookie);
int a733_npu_startup_bind_task(struct a733_npu_startup *, TaskHandle_t);
/* After init, before bind and before the scheduler: PLL_NPU at 492 (the
 * default) or 852 MHz, both at the inherited 800 mV. The register pass admits
 * 852 MHz only for a SID speed bin whose operating-point column lists 800 mV
 * for it and refuses otherwise, before any write. */
int a733_npu_startup_rate(struct a733_npu_startup *, unsigned mhz);
/* The bound EL1 bootstrap task only. READY once the NPU is powered, clocked
 * and out of reset; PENDING while a wait is open; RETAINED after a failure. */
enum a733_npu_platform_result a733_npu_startup_step(struct a733_npu_startup *);
/* After a successful controller CLOSE, the bound task only: the local stop
 * table in one locked pass. READY when stopped. */
enum a733_npu_platform_result a733_npu_startup_stop(struct a733_npu_startup *);
/* The service activation check, on the bound task: 1 when READY and no
 * shared provider has failed, 0 before READY, -1 after a failure or from
 * another task. No register access. */
int a733_npu_startup_prepared(struct a733_npu_startup *);
/* The bound task, after a PENDING step: the ticks to block in
 * ulTaskNotifyTake(pdTRUE, ticks) before the next step. A wait for the
 * supply or the domain-4 row ends with the power service's notification and
 * otherwise at its deadline; the 2 ms settle wakes on the last tick before
 * its end, and the step spins the remainder. 0 means step again at once. */
uint32_t a733_npu_startup_wait_ticks(struct a733_npu_startup *);
/* Any task, before each NPU register access: 1 only while the startup is
 * READY, which requires the shared SRAM routed to the NPU. One acquire load. */
static inline int a733_npu_startup_window_open(const struct a733_npu_startup *s)
{
    return s && atomic_load_explicit(&s->window_open, memory_order_acquire) == 1;
}
#endif
