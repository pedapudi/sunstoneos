#ifndef SUNSTONE_A733_CE_H
#define SUNSTONE_A733_CE_H
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include "a733_clock_leases.h"
#define A733_CE_RANDOM_BYTES 32u
#define A733_CE_DESCRIPTOR_BYTES 208u
#define A733_CE_HASH_INPUT_BYTES 2048u
enum a733_ce_profile { A733_CE_TRNG_ONLY, A733_CE_SHA256_ONLY, A733_CE_TRNG_SHA256 };
#define A733_CE_NS_INTID 200u
#define A733_CE_S_INTID 201u
/* Separate complete cache lines: the hardware descriptor occupies 208 bytes.
 * Storage, aliases and physical extents remain exclusively reserved forever
 * after init; task completion permits reuse under the same reservation,
 * but no global drain or release operation is provided. */
struct a733_ce_memory {
    _Alignas(64) uint8_t descriptor[256];
    _Alignas(64) uint8_t output[64];
    _Alignas(64) uint8_t iv[64];
    _Alignas(64) uint8_t input[A733_CE_HASH_INPUT_BYTES];
};
struct a733_ce_config {
    uint64_t owner, handoff, health_profile, deadline_us;
    uint64_t descriptor_physical, output_physical;
    uint32_t fabric[3], fabric_hz[3]; /* AHB0500, NSI0580, MBUS0588. */
    uint32_t health[4]; /* Secure RCTC, APTC, APTWP, HTCR at 90..9c. */
    enum a733_ce_profile profile; /* Immutable admitted capabilities; zero preserves TRNG. */
    unsigned configure_health; /* 0 adopt; 1 program with both tests initially disabled. */
};
enum a733_ce_sync { A733_CE_PUBLISH, A733_CE_ACQUIRE };
struct a733_ce_ops {
    int (*now_us)(void *,uint64_t *);
    /* Authenticate owner, handoff, actual token and the complete memory mapping.
     * Require independent prior CE DMA drain; exclusion of ALL secure/nonsecure
     * clients, remote firmware, IRQ handlers and clock/reset/health writers;
     * For TRNG capability: immutable characterized health profile bound to this silicon, RC16M,
     * supplies and the double-entropy-source mode enabled here; native secure-read permissions and, for configure_health, secure-write
     * authority while both tests are disabled; stable AHB/NSI/MBUS paths; both
     * GIC routes owned/masked; qualified recovery. This provides authority, not
     * measured entropy or substitute register observations.
     * Memory must be private normal RAM, mapped to the specified CE physical
     * addresses without IOMMU inference. SHA capability requires the complete
     * contiguous memory object beginning at descriptor_physical (output +256,
     * IV +320, input +384), including all cache lines. TRNG uses 256/64-byte DMA extents;
     * cache-line ownership, no aliases or remote accesses. Keep all leases.
     * Success grants immutable authority through the retained lifetime and all
     * rearmed requests and every capability in profile. SHA-only admission
     * requires no health profile or entropy/startup assertion. deadline_us bounds execution only; it cannot limit or
     * silently extend this lifetime authority. */
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
    A733_CE_CONSUMED,A733_CE_RETAINED,A733_CE_INVALID,A733_CE_DIGEST };
struct a733_ce {
    uintptr_t self;
    struct a733_ce_config config;
    struct a733_ce_ops ops;
    struct a733_clock_token token;
    struct a733_clock_leases *registry;
    struct a733_ce_memory *memory;
    void *cookie;
    atomic_uint busy;
    uint64_t last_now;
    uint32_t error_ns,error_secure,status_ns,status_secure;
    uint64_t hash_total_bits;
    unsigned initialized,stage,fault,held,health_programmed;
    unsigned operation,task_configured,hash_bytes,hash_continue;
};
/* Zero controller storage once. All inputs, registry and DMA memory are pairwise
 * disjoint and live throughout retained lifetime. Borrow an immutable actual
 * token: CE exclusive; PERI0/REF/OSC/FABRIC stable, plus RC16M for TRNG. All overlapping writers
 * use the same registry and common register mutex. Callbacks are bounded and
 * non-reentrant; exactly 1 means success, authorize/enter may return 0 pending.
 * Uses adopted enabled CE400 clocks; TRNG requires an assessed secure health profile.
 * Optional cold provisioning programs real cutoffs/window and enables last.
 * No startup-test completion or physical entropy qualification is inferred.
 * Never supplies threshold defaults, seed fallback or an entropy-credit claim.
 * TRNG output is UNQUALIFIED for production security use: characterization of the raw source
 * and a source-backed successful startup protocol for the current test-enable
 * epoch remain required. Assessed thresholds or request counts do not prove it. */
int a733_ce_init(struct a733_ce *,const struct a733_ce_config *,const struct a733_ce_ops *,
    void *,struct a733_clock_leases *,const struct a733_clock_token *,struct a733_ce_memory *);
/* Each request produces one fixed TRNG block. Output is unchanged unless RANDOM, emitted once only.
 * The private output cannot be accessed while DMA owns it. Failure retains all
 * resources; retention BUSY retries metadata only. No reset, retry or reclamation.
 * Task completion permits copying output; it does not establish global drain. */
enum a733_ce_result a733_ce_poll(struct a733_ce *,uint8_t output[A733_CE_RANDOM_BYTES]);
/* Start another request only after the previous RANDOM was consumed, or update
 * the first request deadline before admission. Authenticate the same live token,
 * observe clear status/errors, stable health/clocks and disabled IRQs, then reuse
 * the same retained buffers. No reset, global drain or external success callback.
 * BUSY makes no request pending; retry this function. The new deadline is absolute. */
enum a733_ce_result a733_ce_request(struct a733_ce *,uint64_t deadline_us);
/* Typed SHA-256 compression task: 64..2048 input bytes, multiple of 64, copied
 * into private memory before publication. continuation reuses an opaque 32-byte
 * intermediate digest as IV. total_bits counts all submitted blocks including
 * caller software padding. Every task keeps LPKG clear; no hardware padding.
 * Returns DIGEST for the task's 32-byte digest using the same poll API. The
 * shared session is the sole caller; do not interleave direct controller use. */
enum a733_ce_result a733_ce_hash_request(struct a733_ce *,const uint8_t *,size_t,
    const uint8_t iv[32],unsigned continuation,uint64_t total_bits,uint64_t deadline_us);
/* Callback-free completion boundary used by the retained session owner. */
int a733_ce_idle(const struct a733_ce *);
#endif
