#include "a733_npu_platform.h"

#define CCU UINT32_C(0x02002000)
#define BIT(n) (UINT32_C(1)<<(n))
#define PLL_LOCK BIT(28)
static const uint32_t fabric_masks[]={0x0300001f,0x8700001f,0x8700001f};
static const uint32_t parent_mhz[3][8]={
    {24,0,0,600,0,0,0,0}, {24,0,800,600,480,0,0,0}, {24,600,0,480,400,0,0,0}
};
int a733_npu_platform_scopes(const struct a733_npu_platform_config *c,a733_clock_mask_t *out)
{
    if (!c || !out) return 0;
    a733_clock_mask_t shared=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC;
    for (unsigned i=0;i<3;++i) {
        if ((c->fabric[i]&~fabric_masks[i]) || (i && !(c->fabric[i]&BIT(31)))) return 0;
        unsigned mhz=parent_mhz[i][(c->fabric[i]>>24)&7], divisor=(c->fabric[i]&31)+1;
        if (!mhz || (mhz*1000000u)%divisor || c->rate_hz[i]!=(mhz*1000000u)/divisor) return 0;
        if (mhz!=24) shared|=A733_CLOCK_PERI0;
    }
    *out=shared;return 1;
}
const struct a733_npu_step a733_npu_local_open[A733_NPU_LOCAL_OPEN_STEPS]={
    {CCU+0x2a0,BIT(27),BIT(27),PLL_LOCK},
    {CCU+0xb04,BIT(18),BIT(18),0}, {CCU+0xb04,BIT(17),BIT(17),0}, {CCU+0xb04,BIT(16),BIT(16),0},
    {CCU+0xb00,0x0700001f,0,0}, {CCU+0xb04,1,1,0}, {CCU+0xb00,BIT(31),BIT(31),0}
};
const struct a733_npu_step a733_npu_local_stop[A733_NPU_LOCAL_STOP_STEPS]={
    {CCU+0xb00,BIT(31),0,0}, {CCU+0xb04,1,0,0},
    {CCU+0xb04,BIT(18),0,0}, {CCU+0xb04,BIT(17),0,0}, {CCU+0xb04,BIT(16),0,0}
};
unsigned a733_npu_apply(const struct a733_npu_register_ops *o,void *cookie,
    const struct a733_npu_step *t,unsigned first,unsigned count,unsigned *wrote)
{
    unsigned i=first;
    for (; i<count; ++i) {
        uint32_t old,actual,keep=~t[i].status;
        if (o->read(cookie,t[i].address,&old)!=1) break;
        uint32_t next=((old&~t[i].mask)|(t[i].value&t[i].mask))&keep;
        if ((old&keep)==next) continue;
        if (wrote) ++*wrote;
        if (o->write(cookie,t[i].address,next)!=1 || o->read(cookie,t[i].address,&actual)!=1 ||
            (actual&keep)!=next) break;
    }
    return i;
}
