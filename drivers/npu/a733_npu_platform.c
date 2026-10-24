#include "a733_npu_platform.h"

#define BIT(n) (UINT32_C(1)<<(n))
const uint32_t a733_npu_fabric_masks[3]={0x0300001f,0x8700001f,0x8700001f};
const uint16_t a733_npu_parent_mhz[3][8]={
    {24,0,0,600,0,0,0,0}, {24,0,800,600,480,0,0,0}, {24,600,0,480,400,0,0,0}
};
int a733_npu_platform_scopes(const struct a733_npu_platform_config *c,a733_clock_mask_t *out)
{
    if (!c || !out) return 0;
    a733_clock_mask_t shared=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC;
    for (unsigned i=0;i<3;++i) {
        if ((c->fabric[i]&~a733_npu_fabric_masks[i]) || (i && !(c->fabric[i]&BIT(31)))) return 0;
        unsigned mhz=a733_npu_parent_mhz[i][(c->fabric[i]>>24)&7], divisor=(c->fabric[i]&31)+1;
        if (!mhz || (mhz*1000000u)%divisor || c->rate_hz[i]!=(mhz*1000000u)/divisor) return 0;
        if (mhz!=24) shared|=A733_CLOCK_PERI0;
    }
    *out=shared;return 1;
}
