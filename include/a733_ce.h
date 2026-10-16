#ifndef SUNSTONE_A733_CE_H
#define SUNSTONE_A733_CE_H
#include <stdatomic.h>
#include <stdint.h>
#include "a733_clock_leases.h"
#define A733_CE_RANDOM_BYTES 32u
#define A733_CE_DESCRIPTOR_BYTES 208u
#define A733_CE_NS_INTID 200u
#define A733_CE_S_INTID 201u
/* Separate complete cache lines: the hardware descriptor occupies 208 bytes.
 * Storage, aliases and physical extents remain exclusively reserved forever
 * after init; this first operation provides no drain/release or resubmit API. */
struct a733_ce_memory {
    _Alignas(64) uint8_t descriptor[256];
    _Alignas(64) uint8_t output[64];
};
struct a733_ce_config {
    uint64_t owner, handoff, health_profile, deadline_us;
    uint64_t descriptor_physical, output_physical;
    uint32_t fabric[3], fabric_hz[3]; /* AHB0500, NSI0580, MBUS0588. */
    uint32_t health[4]; /* Secure RCTC, APTC, APTWP, HTCR at 90..9c. */
};
enum a733_ce_sync { A733_CE_PUBLISH, A733_CE_ACQUIRE };
struct a733_ce_ops {
    int (*now_us)(void *,uint64_t *);
    /* Authenticate owner, handoff, actual token and the complete memory mapping.
     * Require independent prior CE DMA drain; exclusion of ALL secure/nonsecure
     * clients, remote firmware, IRQ handlers and clock/reset/health writers;
     * immutable characterized health profile bound to this silicon, RC16M,
     * supplies and the double-entropy-source mode enabled here; native secure-read permissions; stable AHB/NSI/MBUS paths; both
     * GIC routes owned/masked; qualified recovery. This provides authority, not
     * measured entropy or substitute register observations.
     * Memory must be private normal RAM, mapped to the specified CE physical
     * addresses without IOMMU inference, with complete 256/64-byte extents,
     * cache-line ownership, no aliases or remote accesses. Keep all leases. */
    int (*authorize)(void *,const struct a733_ce_config *,const struct a733_clock_token *,
        const struct a733_ce_memory *);
    int (*enter)(void *,uint64_t); /* 1 held; 0 busy; other uncertain. */
    void (*leave)(void *);
    void (*retain)(void *); /* Retain the common register mutex after uncertainty. */
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
    /* Publish cleans descriptor and cleans/invalidates output before DMA.
     * Acquire invalidates output after successful completion. Exact 1 means
     * completed cache maintenance and barriers, never memory drain or release. */
    int (*sync)(void *,struct a733_ce_memory *,enum a733_ce_sync);
};
enum a733_ce_result { A733_CE_PENDING,A733_CE_BUSY,A733_CE_RANDOM,
    A733_CE_CONSUMED,A733_CE_RETAINED,A733_CE_INVALID };
struct a733_ce {
    struct a733_ce_config config;
    struct a733_ce_ops ops;
    struct a733_clock_token token;
    struct a733_clock_leases *registry;
    struct a733_ce_memory *memory;
    void *cookie;
    atomic_uint busy;
    uint64_t last_now;
    uint32_t error_ns,error_secure,status_ns,status_secure;
    unsigned initialized,stage,fault,held;
};
/* Zero controller storage once. All inputs, registry and DMA memory are pairwise
 * disjoint and live throughout retained lifetime. Borrow an immutable actual
 * token: CE exclusive; PERI0/REF/OSC/FABRIC/RC16M stable. All overlapping writers
 * use the same registry and common register mutex. Callbacks are bounded and
 * non-reentrant; exactly 1 means success, authorize/enter may return 0 pending.
 * Uses adopted enabled CE400 clocks and an existing secure health profile.
 * Never supplies threshold defaults, seed fallback or an entropy-credit claim. */
int a733_ce_init(struct a733_ce *,const struct a733_ce_config *,const struct a733_ce_ops *,
    void *,struct a733_clock_leases *,const struct a733_clock_token *,struct a733_ce_memory *);
/* One fixed TRNG request. Output is unchanged unless RANDOM, emitted once only.
 * The private output cannot be accessed while DMA owns it. Failure retains all
 * resources; retention BUSY retries metadata only. No reset, retry or reclamation.
 * Task completion permits copying output; it does not establish global drain. */
enum a733_ce_result a733_ce_poll(struct a733_ce *,uint8_t output[A733_CE_RANDOM_BYTES]);
#endif
