#ifndef SUNSTONE_A733_DISPLAY_NATIVE_H
#define SUNSTONE_A733_DISPLAY_NATIVE_H
#include "a733_display_handoff.h"
#include "a733_iommu.h"
#include "device_memory_pool.h"
struct a733_platform_locks;
struct a733_display_native_config {
    struct a733_media_clock *media;
    struct a733_display_pll *pll;
    struct a733_iommu *iommu;
    struct device_memory_pool *pool;
    struct device_memory_lease frame, tables;
    struct display_request request;
    struct a733_platform_locks *locks;
    void *task, *context;
    uint64_t recovery_generation;
    uint64_t replay_frame_bytes; /* Zero preserves ordinary immutable-frame mode. */
    int (*now_us)(void *,uint64_t *);
    /* Image-owned admission authenticates initial isolation/drain, all remote
     * writers, supplies, panel pins/rails, qualified analog-profile provenance,
     * stable mappings without aliases, and permanent ownership until power
     * removal. It also authenticates this exact request and both allocations.
     * Register observations and numeric identities cannot establish these facts.
     * No production implementation exists until a panel/profile is selected. */
    int (*authorize)(void *,const struct a733_display_native_config *,uint64_t);
    /* Actual selected-panel command-controller/power/reset sequence; bounded
     * and asynchronous, same ownership and deadline as scanout. No default. */
    int (*panel)(void *,const struct display_request *,const struct display_ownership *,
                 const struct a733_dsi_phy *,uint64_t);
};
struct a733_display_native {
    struct a733_display_native *self;
    struct a733_display_native_config config;
    struct a733_display_handoff binding;
    struct a733_iommu_tables tables;
    struct device_memory_extent frame_extent, table_extent;
    struct display_frame_token frame_token;
    uint64_t last_now;
    unsigned initialized,failed,held;
};
/* Native privileged owner API. Sources and IOMMU must already be prepared by
 * their actual providers. Both allocations must be uncached, DEVICE-owned,
 * immutable and distinct. The whole frame allocation is exposed by request;
 * table allocation is exactly32KiB. Retain both forever, including faults.
 * All configuration/objects stay private, immutable, disjoint and alive; use
 * one designated EL1 task and the same register mutex as media/PLL setup.
 * Init performs no MMIO. No panel defaults, drain proof, release or retry. */
int a733_display_native_init(struct a733_display_native *,const struct a733_display_native_config *);
int a733_display_native_replay(struct a733_display_native *,unsigned slot,uint64_t deadline);
int a733_display_native_prepare(struct a733_display_native *,uint64_t);
int a733_display_native_request(struct a733_display_native *,enum display_scanout_action,uint64_t);
enum display_scanout_state a733_display_native_poll(struct a733_display_native *);
/* Platform implementation; not an alternate provider interface. */
int a733_display_native_owner(void *task);
int a733_display_native_mapping(uintptr_t,uint64_t,unsigned);
int a733_display_native_enter(struct a733_platform_locks *,uint64_t);
void a733_display_native_leave(struct a733_platform_locks *);
void a733_display_native_retain(struct a733_platform_locks *);
int a733_display_native_read(uintptr_t,uint32_t *);
int a733_display_native_write(uintptr_t,uint32_t);
int a733_display_native_barrier(void);
int a733_display_native_rcq_submit(void);
#endif
