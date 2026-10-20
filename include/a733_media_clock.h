#ifndef SUNSTONE_A733_MEDIA_CLOCK_H
#define SUNSTONE_A733_MEDIA_CLOCK_H
#include <stdatomic.h>
#include <stdint.h>
#include "a733_clock_leases.h"
#include "a733_gpu_resources.h"

#define A733_MEDIA_VI 1u
#define A733_MEDIA_DE 2u
#define A733_MEDIA_VO0 4u
#define A733_MEDIA_IOMMU 8u
#define A733_MEDIA_ALL 15u
/* This profile prepares CSI/ISP at 324 MHz, DE at 600 MHz, DSI1 host at
 * 150 MHz and both IOMMU bus clocks. Selected camera MCLK1/2 are configured
 * for 24 MHz but remain disabled until actual sensor power/pin sequencing.
 * It adopts already powered domains and stable parents; it does not establish
 * electrical rail compatibility, sensor readiness or transaction drain. */
struct a733_media_clock_config {
    uint64_t owner, handoff, deadline_us;
    uint32_t groups, camera_mclk_mask; /* Bits 1/2; zero unless VI selected. */
    uint32_t fabric[3], rate_hz[3]; /* Masked AHB/NSI/MBUS selectors and rates. */
};
enum a733_media_clock_result { A733_MEDIA_PENDING, A733_MEDIA_BUSY,
    A733_MEDIA_PREPARED, A733_MEDIA_RETAINED, A733_MEDIA_INVALID };
struct a733_media_clock_ops {
    int (*now_us)(void *,uint64_t *);
    /* 1 authenticated; 0 pending without mutation; any other result fails.
     * Authenticate owner/handoff, the exact live common token and borrowed PCK
     * lease, inherited supplies/operating points, all selected-domain writers,
     * local quiescence, reset safety, Device mappings and independent recovery
     * or an image-owned supervised retained session under
     * docs/a733-smp-supervised.md. Initial transaction drain remains required;
     * manual power removal permits no table, buffer or resource reuse.
     * IOMMU mode also reserves VI, DE_SYS and VE_DEC against known native masters.
     * IOMMU additionally requires both banks disabled, inherited tables resolved,
     * exclusion/drain of all ten masters and continued power accessibility.
     * This callback supplies missing platform authority, never clock evidence.
     * Production binding remains required; returning 1 unconditionally is invalid. */
    int (*authorize)(void *,const struct a733_media_clock_config *,
        const struct a733_clock_token *,const struct a733_gpu_resource_lease *);
    int (*enter)(void *,uint64_t deadline); /* 1 held, 0 busy, otherwise uncertain. */
    void (*leave)(void *);
    /* Called after uncertain entry (which may be unowned) or a fault while
     * held. Determine actual task/mutex ownership before forwarding retention;
     * a negative entry alone does not prove acquisition. Retain known ownership
     * even when health has failed, and prohibit subsequent register access. */
    void (*retain)(void *);
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
};
struct a733_media_clock {
    struct a733_media_clock *self;
    void *successor; /* Single serialized consumer; never copied or released. */
    unsigned transferred;
    struct a733_media_clock_config config;
    struct a733_media_clock_ops ops;
    struct a733_clock_leases *registry;
    struct a733_gpu_resource_manager *pck_manager;
    struct a733_gpu_resource_lease pck;
    struct a733_clock_token token;
    void *cookie;
    atomic_uint busy;
    uint64_t last_now;
    a733_clock_mask_t shared, exclusive;
    uint32_t selected_mhz[3];
    unsigned pck_client, stage, step;
    unsigned initialized, fault, held, wrote;
};
/* Zero private storage once. Config, ops, registry, manager and object must be
 * disjoint and remain alive. All callbacks are bounded, task-only, non-reentrant,
 * return exactly 1 on success and use monotonic microseconds. One global clock
 * registry and one existing GPU-resource manager own shared PCK/OSC stability.
 * The supplied stable PCK lease must remain owned; it is borrowed, not released.
 * Every CCU writer uses the same register mutex. Locals cannot be delegated
 * until PREPARED and exact token authentication by the higher-level coordinator.
 * VO0 jointly reserves DISPLL1 exclusively for delegated cold PLL preparation.
 * Delegation keeps all parent, rail, domain, PCK and global leases live. */
int a733_media_clock_init(struct a733_media_clock *,const struct a733_media_clock_config *,
    const struct a733_media_clock_ops *,void *,struct a733_clock_leases *,
    struct a733_gpu_resource_manager *,unsigned,const struct a733_gpu_resource_lease *);
enum a733_media_clock_result a733_media_clock_poll(struct a733_media_clock *);
/* Serialized, callback-free completion observation. No physical revalidation. */
int a733_media_clock_prepared(const struct a733_media_clock *);
/* No release API: prepared or faulted clocks and authority remain retained.
 * PREPARED is a historical initialization result, not a current readiness scan.
 * BUSY can mean registry fault retention is pending; retry metadata polling.
 * After RETAINED, no callback or hardware operation occurs on later polls. */
#endif
