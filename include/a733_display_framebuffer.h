#ifndef SUNSTONE_A733_DISPLAY_FRAMEBUFFER_H
#define SUNSTONE_A733_DISPLAY_FRAMEBUFFER_H
#include "a733_counter_clock.h"
#include "a733_power_domains.h"
#include "a733_display_engine.h"
#include "a733_iommu.h"
#include "a733_media_clock.h"
#include "a733_platform_locks.h"
#include "device_memory_pool.h"
/* Retained 1280x720 RGB565 DE1 source for one TV output. One uncached parent
 * holds the 32 KiB IOMMU table, a 4 KiB guard page and the frame bank, which
 * IOMMU master 8 reads at IOVA 0x10001000. The source owns the DE media-clock
 * token and the IOMMU/VI/VE exclusion, and holds a domain-1 row of the
 * power-domain owner. The output controller lends its clock, rail, PMIC and
 * stable PCK lease; they are never released here. Every fault keeps the
 * parent, tokens and mutex retained. */
struct a733_display_framebuffer_config {
    struct a733_counter_clock *counter;
    struct a733_clock_leases *clocks;
    struct a733_platform_locks *locks;
    struct a733_gpu_resource_manager *pck;
    const struct a733_gpu_resource_lease *pck_lease; /* Borrowed live output lease. */
    unsigned pck_client;
    uint64_t owner, authority, deadline;
    uint32_t fabric[3], fabric_hz[3], iommu_gates[2];
    uint32_t parent_bytes, frame_span; /* frame_span excludes the table. */
    enum a733_de_output output;        /* TV0 or TV1 RGB565. */
    void *context;
    /* Designated task, live session and unchanged admitted session record. */
    int (*current)(void *);
    /* The lent output clock, rail, PMIC lifetime and PCK lease remain live. */
    int (*output_owned)(void *);
    /* Optional: a live clock token check that may wait out registry BUSY
     * within the deadline. Without it, BUSY counts as not owned. */
    int (*token_live)(void *,const struct a733_clock_token *);
    /* Output controller state permits this DE action. */
    int (*admit)(void *,enum a733_de_action);
    /* Optional replay pair: the output admits RCQ submission through this
     * request deadline, and rcq_submit performs the platform RCQ write. */
    int (*replay)(void *,uint64_t deadline);
    int (*rcq_submit)(void);
    /* The image's power-domain owner and the lent output rail token, which
     * holds DCDC2; the domain-1 row is registered at boot. */
    struct a733_power_domains *domains;
    const struct a733_rail_token *rail;
};
struct a733_display_framebuffer {
    struct a733_display_framebuffer *self;
    struct a733_display_framebuffer_config config;
    struct device_memory_pool *pool;
    struct device_memory_lease lease;
    struct device_memory_extent extent;
    struct a733_clock_token exclusion;
    struct a733_media_clock media;
    struct a733_media_clock_config media_config;
    struct a733_power_domain_row row;
    struct a733_iommu iommu;
    struct a733_iommu_tables tables;
    struct a733_de de;
    struct a733_de_config de_config;
    uint64_t last_now,until;
    unsigned stage,index,held,failed,boot_ready,domain_requested;
};
/* Boot only, before the scheduler: bind zeroed storage, acquire the parent,
 * fill it with 0xa5 and build the IOMMU table. With colorbars, the frame is
 * written and the parent is published to DEVICE; otherwise the parent stays
 * CPU-owned until publish. Failure retains any acquired parent. */
int a733_display_framebuffer_boot(struct a733_display_framebuffer *,
    const struct a733_display_framebuffer_config *,unsigned colorbars);
/* Barrier, then the single CPU->DEVICE transition of the complete parent. */
int a733_display_framebuffer_publish(struct a733_display_framebuffer *);
/* Designated task while the output is PREPARED: 0 pending, 1 DE armed for
 * start, -1 retained. Asserts the DE resets, requests PCK domain 1 through its
 * row, prepares the media clock, MBUS gates, IOMMU and DE. */
int a733_display_framebuffer_prepare(struct a733_display_framebuffer *);
/* Designated task after output stop: 0 pending, 1 DE disabled, -1 retained. */
int a733_display_framebuffer_stop(struct a733_display_framebuffer *);
/* One IOMMU fault scan step: 0 pending, 1 complete, -1 failure. */
int a733_display_framebuffer_service(struct a733_display_framebuffer *);
/* Output pixel-source callback with the framebuffer as context. */
int a733_display_framebuffer_source(void *,unsigned starting);
/* The parent is DEVICE-owned, unchanged and inside the uncached pool. */
int a733_display_framebuffer_memory(const struct a733_display_framebuffer *);
/* Media, IOMMU and DE state match the fixed TV source configuration. */
int a733_display_framebuffer_ready(const struct a733_display_framebuffer *);
/* One locked owned-register access; an uncertain access retains. */
int a733_display_framebuffer_read(struct a733_display_framebuffer *,uintptr_t,uint32_t *);
int a733_display_framebuffer_write(struct a733_display_framebuffer *,uintptr_t,uint32_t);
void a733_display_framebuffer_retain(struct a733_display_framebuffer *);
#endif
