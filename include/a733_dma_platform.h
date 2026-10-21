#ifndef SUNSTONE_A733_DMA_PLATFORM_H
#define SUNSTONE_A733_DMA_PLATFORM_H
#include "a733_clock_leases.h"

enum a733_dma_platform_result { A733_DMA_PLATFORM_READY, A733_DMA_PLATFORM_BUSY,
    A733_DMA_PLATFORM_PENDING, A733_DMA_PLATFORM_RETAINED };
/* Privileged, immutable startup metadata. AHB/NSI/MBUS words contain only their
 * source, divisor and (NSI/MBUS) enable fields, in that order. All rates are Hz.
 * handoff identifies external proof that all DMA0 users, including remote MCU
 * and FIFO producers, stopped and drained; interrupt delivery is masked. A
 * nonzero number alone is NOT that proof: acquire must authenticate it.
 * cold=1 only starts an inherited reset-asserted, gated controller. No path
 * asserts reset. cold=0 adopts already enabled hardware without writes.
 * Route words are low 16 bits. Changes must be within route_change_mask. */
struct a733_dma_platform_config {
    uint32_t fabric[3], rate_hz[3];
    uint32_t expected_cpu, expected_mcu, desired_cpu, desired_mcu, route_change_mask;
    uint64_t owner, handoff, deadline_us;
    unsigned cold;
};
struct a733_dma_platform_operations {
    /* Admit ONE combined registry token: supplied shared mask plus exclusive
     * DMA0, for config.owner, after authenticating config.handoff. Parent scopes
     * include OSC/REF transitively. Never call this with the register lock held.
     * BUSY is effect-free; READY/PENDING/RETAINED publish a token if admitted.
     * Repeated PENDING calls preserve identity. Tokens remain held for platform
     * lifetime, including failure; this component has no release/reset API. */
    enum a733_dma_platform_result (*acquire)(void *,
        const struct a733_dma_platform_config *, a733_clock_mask_t shared,
        struct a733_clock_token *);
    /* All callbacks bounded, serialized, non-reentrant. enter: 1 owns the ONE
     * common register mutex, 0 busy with no effects, -1 uncertain failure.
     * now_us enforces a common monotonic lower bound in microseconds, including
 * samples inside authority admission; this deadline is unrelated to DMA ticks.
     * Reads/writes return exactly 1 only after the ordered access completes. */
    int (*enter)(void *, uint64_t deadline_us);
    void (*leave)(void *);
    int (*now_us)(void *, uint64_t *);
    int (*read)(void *, uintptr_t, uint32_t *);
    int (*write)(void *, uintptr_t, uint32_t);
};
struct a733_dma_platform {
    struct a733_dma_platform_config config;
    struct a733_dma_platform_operations ops;
    struct a733_clock_token token;
    void *cookie;
    uint64_t last_now, reset_since;
    a733_clock_mask_t shared;
    uint32_t selected_mhz[3];
    unsigned initialized, stage, failed;
};
/* Pure profile validation; output untouched on refusal. FABRIC stabilizes the
 * AHB/NSI/MBUS source/divisor/global-gate fields. DMA0 exclusively owns its bus
 * and MBUS leaf gates. Other critical gate bits must already be set and survive
 * every update. Selected parent leases remain separate from FABRIC. */
int a733_dma_platform_scopes(const struct a733_dma_platform_config *, a733_clock_mask_t *shared);
/* Zero once, then initialize before concurrent use. All state and callbacks
 * are private privileged platform-lifetime storage; caller serializes polls. */
int a733_dma_platform_init(struct a733_dma_platform *,
    const struct a733_dma_platform_config *, const struct a733_dma_platform_operations *, void *);
enum a733_dma_platform_result a733_dma_platform_poll(struct a733_dma_platform *);
int a733_dma_platform_ready(const struct a733_dma_platform *);
#endif
