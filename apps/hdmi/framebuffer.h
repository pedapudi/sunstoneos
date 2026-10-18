#ifndef SUNSTONE_HDMI_FRAMEBUFFER_H
#define SUNSTONE_HDMI_FRAMEBUFFER_H
#include "a733_display_engine.h"
#include "a733_iommu.h"
#include "a733_media_clock.h"
#include "device_memory_pool.h"
/* Authenticated session suffix; immutable initial isolation is a physical
 * prerequisite. Registers only verify consistency with that handoff. */
struct hdmi_framebuffer_session {
    uint8_t isolation[32],memory[32];
    uint64_t authority;
    uint32_t fabric[3],fabric_hz[3];
    uint32_t iommu_gates[2];
};
struct hdmi_framebuffer {
    struct hdmi_framebuffer *self;
    struct hdmi_framebuffer_session session;
    struct device_memory_pool *pool;
    struct device_memory_lease lease;
    struct device_memory_extent extent;
    struct a733_clock_token exclusion;
    struct a733_media_clock media;
    struct a733_media_clock_config media_config;
    struct a733_iommu iommu;
    struct a733_iommu_tables tables;
    struct a733_de de;
    struct a733_de_config de_config;
    uint64_t last_now,until;
    unsigned stage,index,held,failed,domain_on,boot_ready;
};
extern struct hdmi_framebuffer framebuffer;
int hdmi_framebuffer_session_valid(const struct hdmi_framebuffer_session *);
int hdmi_framebuffer_boot(void);
int hdmi_framebuffer_prepare(void);
int hdmi_framebuffer_stop(void);
int hdmi_framebuffer_service(void);
int hdmi_framebuffer_source(void *,unsigned starting);
void hdmi_framebuffer_retain(void);
#endif
