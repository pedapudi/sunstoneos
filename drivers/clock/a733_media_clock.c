#include "a733_media_clock.h"
#include <stddef.h>
#define BIT(n) (UINT32_C(1)<<(n))
#define CCU UINT32_C(0x02002000)
#define OSC UINT32_C(0x07090160)
#define PCK UINT32_C(0x07060000)
#define VI A733_MEDIA_VI
#define DE A733_MEDIA_DE
#define VO A733_MEDIA_VO0
#define MM A733_MEDIA_IOMMU
#define MODULE UINT32_C(0x8700001f)
enum { ACQUIRE, AUTHORIZE, BASELINE, PROGRAM, COMPLETE, FAULTED };
struct change { uint16_t offset; uint8_t group, mclk; uint32_t mask,value; };
/* Source order: domain/bus reset deassertion, disabled module configuration,
 * bus gates, then module gates. No reset assertion or parent/fabric write. */
static const struct change changes[]={
    {0x1884,VI,0,BIT(16),BIT(16)}, {0x1844,VI,0,BIT(16),BIT(16)},
    {0x0a74,DE,0,BIT(16),BIT(16)}, {0x0a04,DE,0,BIT(16),BIT(16)},
    {0x16e4,VO,0,BIT(16),BIT(16)}, {0x16c4,VO,0,BIT(16),BIT(16)},
    {0x150c,VO,0,BIT(16),BIT(16)}, {0x158c,VO,0,BIT(16),BIT(16)},
    {0x058c,MM,0,BIT(16),BIT(16)}, {0x05b4,MM,0,BIT(16),BIT(16)},
    {0x1840,VI,0,0x0700001f,0x05000000}, {0x1860,VI,0,0x0700001f,0x04000000},
    {0x1804,VI,2,0x87001f1f,0}, {0x1808,VI,4,0x87001f1f,0},
    {0x0a00,DE,0,0x0700001f,0}, {0x1588,VO,0,0x0700001f,0x02000000},
    {0x1844,VI,0,1,1}, {0x05e4,VI,0,0x300,0x300},
    {0x0a04,DE,0,1,1}, {0x16c4,VO,0,1,1},
    {0x150c,VO,0,1,1}, {0x158c,VO,0,1,1},
    {0x058c,MM,0,7,7}, {0x05b4,MM,0,7,7},
    {0x1840,VI,0,BIT(31),BIT(31)}, {0x1860,VI,0,BIT(31),BIT(31)},
    {0x0a00,DE,0,BIT(31),BIT(31)}, {0x1588,VO,0,BIT(31),BIT(31)}
};
static const uint32_t fabric_offsets[]={0x500,0x580,0x588};
static const uint32_t fabric_masks[]={0x0300001f,0x8700001f,0x8700001f};
static const unsigned parent_mhz[3][8]={
    {24,0,0,600,0,0,0,0}, {24,0,800,600,480,0,0,0}, {24,600,0,480,400,0,0,0}
};
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
int a733_media_clock_init(struct a733_media_clock *p,const struct a733_media_clock_config *c,
    const struct a733_media_clock_ops *o,void *cookie,struct a733_clock_leases *registry,
    struct a733_gpu_resource_manager *manager,unsigned client,const struct a733_gpu_resource_lease *pck)
{
    if (!p || p->initialized || !c || !o || !registry || !manager || !pck ||
        !o->now_us || !o->authorize || !o->enter || !o->leave || !o->retain || !o->read || !o->write ||
        !c->owner || !c->handoff || !c->deadline_us || !c->groups || (c->groups&~A733_MEDIA_ALL) ||
        (c->camera_mclk_mask&~6u) || (c->camera_mclk_mask && !(c->groups&VI)) ||
        client>=A733_GPU_RESOURCE_CLIENTS || !pck->token || pck->mode!=A733_GPU_RESOURCE_STABLE ||
        pck->mask!=(A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK)) return 0;
    const void *objects[]={p,c,o,registry,manager,pck};
    const size_t sizes[]={sizeof(*p),sizeof(*c),sizeof(*o),sizeof(*registry),sizeof(*manager),sizeof(*pck)};
    for (unsigned i=0;i<6;++i) for (unsigned j=i+1;j<6;++j)
        if (!separate(objects[i],sizes[i],objects[j],sizes[j])) return 0;
    a733_clock_mask_t shared=A733_CLOCK_FABRIC|A733_CLOCK_OSC|A733_POWER_DCDC2,exclusive=0;
    unsigned selected[3];
    for (unsigned i=0;i<3;++i) {
        if ((c->fabric[i]&~fabric_masks[i]) || (i && !(c->fabric[i]&BIT(31)))) return 0;
        unsigned mhz=parent_mhz[i][(c->fabric[i]>>24)&7],div=(c->fabric[i]&31)+1;
        if (!mhz || (mhz*1000000u)%div || c->rate_hz[i]!=(mhz*1000000u)/div) return 0;
        selected[i]=mhz;
        if (mhz!=24) shared|=A733_CLOCK_PERI0;
    }
    if (c->groups&VI) {shared|=A733_CLOCK_PLL_VIDEO0;exclusive|=A733_CLOCK_VI;}
    if (c->groups&DE) {shared|=A733_CLOCK_PLL_DE;exclusive|=A733_CLOCK_DE_SYS;}
    if (c->groups&VO) {shared|=A733_CLOCK_PERI0;exclusive|=A733_CLOCK_VO0|A733_CLOCK_DISPLL1;}
    if (c->groups&MM) exclusive|=A733_CLOCK_IOMMU_SYS|A733_CLOCK_VI|A733_CLOCK_DE_SYS|A733_CLOCK_VE_DEC;
    *p=(struct a733_media_clock){.config=*c,.ops=*o,.registry=registry,.pck_manager=manager,
        .pck=*pck,.cookie=cookie,.shared=shared,.exclusive=exclusive,.pck_client=client,.initialized=1};
    for (unsigned i=0;i<3;++i) p->selected_mhz[i]=selected[i];
    atomic_init(&p->busy,0);return 1;
}
static int timely(struct a733_media_clock *p)
{
    uint64_t now;
    if (p->ops.now_us(p->cookie,&now)!=1 || now<p->last_now || now>=p->config.deadline_us) return 0;
    p->last_now=now;return 1;
}
static int rd(struct a733_media_clock *p,uintptr_t a,uint32_t *v)
{return timely(p) && p->ops.read(p->cookie,a,v)==1 && timely(p);}
static int bits(struct a733_media_clock *p,uintptr_t a,uint32_t mask,uint32_t value)
{uint32_t v;return rd(p,a,&v) && (v&mask)==value;}
static int output(struct a733_media_clock *p,unsigned offset,uint32_t mask)
{
    return bits(p,CCU+offset,mask|(mask<<16),mask|(mask<<16)) &&
        bits(p,CCU+offset+0x80,mask<<16,mask<<16);
}
static int integer_pll(struct a733_media_clock *p,unsigned offset,uint32_t gates,uint32_t *v)
{
    return rd(p,CCU+offset,v) && (*v&gates)==gates && !(*v&BIT(1)) &&
        bits(p,CCU+offset+8,BIT(31),0) && bits(p,CCU+offset+12,BIT(31)|BIT(27)|BIT(26)|BIT(25),0) &&
        (((*v>>8)&255)+1)>=53 && (((*v>>8)&255)+1)<=105;
}
static int peri_rate(struct a733_media_clock *p,uint32_t v,unsigned mhz)
{
    unsigned div,mask,n=((v>>8)&255)+1;
    switch (mhz) {
    case 24:return 1;
    case 600:div=(((v>>20)&7)+1)*2;mask=BIT(11)|BIT(9);break;
    case 400:div=(((v>>20)&7)+1)*3;mask=BIT(11)|BIT(2)|BIT(1);break;
    case 800:div=((v>>16)&7)+1;mask=BIT(10);break;
    case 480:div=((v>>2)&7)+1;mask=BIT(8)|BIT(7);break;
    case 150:div=(((v>>20)&7)+1)*8;mask=BIT(11)|BIT(9)|BIT(5)|BIT(4)|BIT(3);break;
    default:return 0;
    }
    return 24u*n==mhz*div && output(p,0x1908,mask);
}
static int stable(struct a733_media_clock *p)
{
    uint32_t v;
    if (!rd(p,OSC,&v) || (((v>>14)&3)!=0 && ((v>>14)&3)!=3) ||
        !bits(p,UINT32_C(0x070101ac),1,1)) return 0;
    if (p->shared&(A733_CLOCK_PERI0|A733_CLOCK_PLL_VIDEO0|A733_CLOCK_PLL_DE)) {
        if (!rd(p,CCU,&v) || (v&UINT32_C(0xf8000000))!=UINT32_C(0xf8000000)) return 0;
        unsigned n=((v>>8)&255)+1,m=((v>>16)&127)+1;
        if (n<80 || n>116 || n!=m) return 0;
    }
    if (p->shared&A733_CLOCK_PERI0) {
        if (!integer_pll(p,0xa0,0xfe000000,&v) || (v&BIT(24))) return 0;
        for (unsigned i=0;i<3;++i) if (!peri_rate(p,v,p->selected_mhz[i])) return 0;
        if ((p->config.groups&VO) && !peri_rate(p,v,150)) return 0;
    }
    if (p->config.groups&VI) {
        if (!integer_pll(p,0x120,0xf8000000,&v) ||
            24u*(((v>>8)&255)+1)!=324u*(((v>>20)&7)+1) || !output(p,0x1910,1)) return 0;
    }
    if (p->config.groups&DE) {
        if (!integer_pll(p,0x2e0,0xf4000000,&v) ||
            24u*(((v>>8)&255)+1)!=600u*(((v>>16)&7)+1) || !output(p,0x1928,2)) return 0;
    }
    for (unsigned i=0;i<3;++i)
        if (!bits(p,CCU+fabric_offsets[i],fabric_masks[i],p->config.fabric[i])) return 0;
    uint32_t ahb=0,mbus=0,domains=0;
    if (p->config.groups&VI) {ahb|=BIT(2);mbus|=BIT(24);domains|=BIT(0);}
    if (p->config.groups&DE) {ahb|=BIT(5);mbus|=BIT(11);domains|=BIT(1);}
    if (p->config.groups&VO) {ahb|=BIT(3);domains|=BIT(9);}
    if (p->config.groups&MM) {mbus|=3;domains|=BIT(0)|BIT(1)|BIT(2)|BIT(3)|BIT(8);}
    if (!bits(p,CCU+0x5c0,ahb,ahb) || !bits(p,CCU+0x5e0,mbus,mbus) ||
        !bits(p,CCU+0x5e4,0x40302,0x40302)) return 0;
    for (unsigned domain=0;domain<=9;++domain) if (domains&BIT(domain)) {
        uintptr_t base=PCK+domain*0x1000u;
        if (!bits(p,base,15,8) || !bits(p,base+8,15,8)) return 0;
    }
    return 1;
}
static int baseline(struct a733_media_clock *p)
{
    static const struct {unsigned group,offset;} disabled[]={
        {VI,0x1800},{VI,0x1804},{VI,0x1808},{VI,0x1840},{VI,0x1860},
        {DE,0x0a00},{VO,0x1500},{VO,0x1508},{VO,0x1510},{VO,0x1580},
        {VO,0x1588},{VO,0x15c0},{VO,0x15c4}
    };
    for (unsigned i=0;i<sizeof(disabled)/sizeof(*disabled);++i)
        if ((p->config.groups&disabled[i].group) && !bits(p,CCU+disabled[i].offset,BIT(31),0)) return 0;
    return 1;
}
static int selected(struct a733_media_clock *p,const struct change *c)
{return (p->config.groups&c->group) && (!c->mclk || (p->config.camera_mclk_mask&c->mclk));}
static int update(struct a733_media_clock *p,const struct change *c)
{
    uint32_t old,actual;
    if (!rd(p,CCU+c->offset,&old)) return 0;
    /* Recheck disabled state immediately before modifying a module divider. */
    if ((c->mask&0x07000000) && (old&BIT(31))) return 0;
    uint32_t next=(old&~c->mask)|c->value;
    if (next==old) return 1;
    p->wrote=1;
    return timely(p) && p->ops.write(p->cookie,CCU+c->offset,next)==1 && timely(p) &&
        rd(p,CCU+c->offset,&actual) && actual==next;
}
static enum a733_media_clock_result finish(struct a733_media_clock *p,enum a733_media_clock_result r)
{atomic_store_explicit(&p->busy,0,memory_order_release);return r;}
static enum a733_media_clock_result failed(struct a733_media_clock *p)
{
    p->fault=1;
    if (p->held) {p->ops.retain(p->cookie);p->held=0;}
    if (p->token.id) {
        enum a733_clock_result r=a733_clock_retain(p->registry,&p->token);
        if (r==A733_CLOCK_BUSY) return finish(p,A733_MEDIA_BUSY);
        /* FAULT includes a registry already retained by another owner. An
         * INVALID token cannot justify dropping the separately held authority. */
    }
    p->stage=FAULTED;return finish(p,A733_MEDIA_RETAINED);
}
enum a733_media_clock_result a733_media_clock_poll(struct a733_media_clock *p)
{
    if (!p || !p->initialized) return A733_MEDIA_INVALID;
    unsigned expected=0;
    if (!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))
        return A733_MEDIA_BUSY;
    if (p->stage==FAULTED) return finish(p,A733_MEDIA_RETAINED);
    if (p->fault) return failed(p); /* Metadata-only retry after registry BUSY. */
    if (p->stage==COMPLETE) return finish(p,A733_MEDIA_PREPARED);
    if (!timely(p)) return failed(p);
    if (p->stage==ACQUIRE) {
        enum a733_clock_result r=a733_clock_acquire(p->registry,p->config.owner,p->shared,p->exclusive,
            p->config.deadline_us,&p->token);
        if (r==A733_CLOCK_BUSY) return finish(p,A733_MEDIA_BUSY);
        if (r!=A733_CLOCK_OK || !timely(p)) return failed(p);
        p->stage=AUTHORIZE;return finish(p,A733_MEDIA_PENDING);
    }
    enum a733_clock_result cr=a733_clock_token_status(p->registry,&p->token);
    enum a733_gpu_resource_result gr=a733_gpu_resources_status(p->pck_manager,p->pck_client,&p->pck);
    if (cr==A733_CLOCK_BUSY || gr==A733_GPU_RESOURCE_BUSY) return finish(p,A733_MEDIA_BUSY);
    if (cr!=A733_CLOCK_OK || gr!=A733_GPU_RESOURCE_OK) return failed(p);
    if (p->stage==AUTHORIZE) {
        int r=p->ops.authorize(p->cookie,&p->config,&p->token,&p->pck);
        if (!timely(p) || (r!=0 && r!=1)) return failed(p);
        if (!r) return finish(p,A733_MEDIA_PENDING);
        p->stage=BASELINE;return finish(p,A733_MEDIA_PENDING);
    }
    int entered=p->ops.enter(p->cookie,p->config.deadline_us);
    if (entered!=0) p->held=1;
    if (entered!=1) {
        if (entered!=0 || !timely(p)) return failed(p);
        return finish(p,A733_MEDIA_BUSY);
    }
    if (!timely(p) || !stable(p)) return failed(p);
    unsigned completed=0;
    if (p->stage==BASELINE) {
        if (!baseline(p)) return failed(p);
        p->stage=PROGRAM;
    } else {
        while (p->step<sizeof(changes)/sizeof(*changes) && !selected(p,&changes[p->step])) ++p->step;
        if (p->step<sizeof(changes)/sizeof(*changes)) {
            if (!update(p,&changes[p->step])) return failed(p);
            ++p->step;
        } else {
            for (unsigned i=0;i<sizeof(changes)/sizeof(*changes);++i)
                if (selected(p,&changes[i]) && !bits(p,CCU+changes[i].offset,changes[i].mask,changes[i].value)) return failed(p);
            completed=1;
        }
    }
    if (!timely(p)) return failed(p);
    p->ops.leave(p->cookie);p->held=0;
    if (!timely(p)) return failed(p);
    /* Publish only after all final observations, mutex release and time checks. */
    if (completed) p->stage=COMPLETE;
    return finish(p,completed ? A733_MEDIA_PREPARED : A733_MEDIA_PENDING);
}
