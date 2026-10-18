#ifndef SUNSTONE_A733_DMA_STARTUP_H
#define SUNSTONE_A733_DMA_STARTUP_H
#include "a733_counter_clock.h"
#include "a733_dma_platform.h"
#include "a733_platform_locks.h"

#define A733_DMA_HANDOFF_CPU_QUIET       (1u<<0)
#define A733_DMA_HANDOFF_MCU_QUIET       (1u<<1)
#define A733_DMA_HANDOFF_PRODUCERS_QUIET (1u<<2)
#define A733_DMA_HANDOFF_DRAINED         (1u<<3)
#define A733_DMA_HANDOFF_IRQ_MASKED      (1u<<4)
#define A733_DMA_HANDOFF_RECOVERY        (1u<<5)
#define A733_DMA_HANDOFF_MANUAL_POWER_REMOVAL (1u<<6)
#define A733_DMA_HANDOFF_SUPERVISED UINT32_C(0x5f)
#define A733_DMA_HANDOFF_REQUIRED        UINT32_C(0x3f)
/* Private boot-authority record. These assertions must refer to independently
 * established board evidence. Populating flags or an ID cannot establish MCU
 * quiescence, prior bus drain or watchdog recovery. Keep all earlier DMA0 users
 * stopped for the lifetime of the resulting reservation. Applications cannot
 * supply or modify this record. Configuration is exact, including deadline. */
struct a733_dma_handoff {
    struct a733_dma_platform_config allowed;
    uintptr_t controller;
    uint64_t evidence;
    uint32_t established;
    uint16_t owned_channels, service_channels;
};
struct a733_dma_startup {
    struct a733_dma_handoff handoff;
    struct a733_dma_platform platform;
    struct a733_dma_platform_operations operations;
    struct a733_clock_token token;
    struct a733_counter_clock *clock;
    struct a733_clock_leases *registry;
    struct a733_platform_locks *locks;
    TaskHandle_t task;
    unsigned initialized, failed, register_held, supervised;
};
/* Private EL1 storage, initialized before the scheduler. Copy the authenticated
 * boot record. Registry and shared locks must already use exactly
 * a733_counter_clock_now with this same clock object. No MMIO or acquisition.
 * Protect this object, clock, registry, locks and all callback state against DMA
 * mappings; preserve their lifetimes. All pointers must identify disjoint objects.
 */
int a733_dma_startup_init(struct a733_dma_startup *,const struct a733_dma_handoff *,
    struct a733_counter_clock *,struct a733_clock_leases *,struct a733_platform_locks *);
/* Separate supervised entry: quiet/drained/IRQ facts remain required; 0x5f
 * replaces watchdog recovery with an externally owned manual power-removal
 * session. No timeout authorizes reset, release, drain or buffer reuse. */
int a733_dma_startup_supervised_init(struct a733_dma_startup *,const struct a733_dma_handoff *,
    struct a733_counter_clock *,struct a733_clock_leases *,struct a733_platform_locks *);
/* Register the exact bootstrap task before scheduling starts, once. */
int a733_dma_startup_bind_task(struct a733_dma_startup *,TaskHandle_t);
/* One privileged bootstrap task polls after scheduling starts. That task owns
 * all later polls; applications cannot impersonate it by passing a handoff ID.
 * Preserve all admitted tokens on failure. No automatic release or retry. */
enum a733_dma_platform_result a733_dma_startup_poll(struct a733_dma_startup *);
int a733_dma_startup_ready(const struct a733_dma_startup *);
/* dma_service_activation.prepared callback. Exact controller/channel match,
 * designated bootstrap task, copied handoff, live token and healthy common
 * clock/registry/lock state are mandatory. Registry contention remains pending.
 * Returns 1 ready, 0 pending, -1 retained; no MMIO or further acquisition. */
int a733_dma_startup_prepared(void *,uintptr_t controller,uint16_t channels);
#endif
