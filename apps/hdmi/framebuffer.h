#ifndef SUNSTONE_HDMI_FRAMEBUFFER_H
#define SUNSTONE_HDMI_FRAMEBUFFER_H
#include "a733_display_framebuffer.h"
/* Authenticated session suffix; immutable initial isolation is a physical
 * prerequisite. Registers only verify consistency with that handoff. */
struct hdmi_framebuffer_session {
    uint8_t isolation[32],memory[32];
    uint64_t authority;
    uint32_t fabric[3],fabric_hz[3];
    uint32_t iommu_gates[2];
};
/* TV0 framebuffer source; pixel_context of the HDMI native provider. */
extern struct a733_display_framebuffer framebuffer;
static inline int hdmi_framebuffer_session_valid(const struct hdmi_framebuffer_session *s)
{
    unsigned isolation=0,memory=0;
    if(!s)return 0;
    for(unsigned i=0;i<32;++i){isolation|=s->isolation[i];memory|=s->memory[i];}
    return s->authority&&isolation&&memory&&s->iommu_gates[0]==0x10007&&s->iommu_gates[1]==0x10007;
}
/* Boot only: bind the source to the admitted session and the HDMI provider. */
int hdmi_framebuffer_boot(void);
int hdmi_framebuffer_prepare(void);
int hdmi_framebuffer_stop(void);
#endif
