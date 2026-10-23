#include "a733_pcie.h"
#include "a733_clock_reference.h"
#include "a733_range.h"
#define CCU UINT32_C(0x02002000)
#define DBI UINT32_C(0x06000000)
#define APP UINT32_C(0x06400000)
#define ATU UINT32_C(0x06300000)
#define CFG UINT32_C(0x20000000)
#define BIT(n) (UINT32_C(1)<<(n))
#define IRQ_MASK UINT32_C(0x0300ff00)
enum { CLOCK,RAIL,RAIL_WAIT,ADMIT,PCK,SUPPLY,PINS,BASELINE,ENABLE,PHY,RC,
    RESET_WAIT,RESET_RELEASE,TRAIN,LINK,IDENTIFY,READY,STOP_RESET,STOP_DOWN,STOPPED };
static const enum a733_rail supplies[]={A733_RAIL_DCDC2,A733_RAIL_BLDO1};
static const uint32_t uv[]={800000,1800000};
static const uint32_t fabric_off[]={0x500,0x580,0x588};
static const uint32_t fabric_mask[]={0x0300001f,0x8700001f,0x8700001f};
static const uint32_t parent_mhz[3][8]={
    {24,0,0,600,0,0,0,0},{24,0,800,600,480,0,0,0},{24,600,0,480,400,0,0,0}
};
int a733_pcie_output_outside(const struct a733_pcie *p,const void *v,size_t n)
{
    return a733_ranges_disjoint(p,sizeof(*p),v,n)&&a733_ranges_disjoint(p->clocks,sizeof(*p->clocks),v,n)&&
        a733_ranges_disjoint(p->pins,sizeof(*p->pins),v,n)&&a733_ranges_disjoint(p->rails,sizeof(*p->rails),v,n)&&
        a733_ranges_disjoint(p->pmic,sizeof(*p->pmic),v,n)&&a733_ranges_disjoint(p->pck,sizeof(*p->pck),v,n)&&
        p->ops.outside(p->cookie,v,n)==1&&p->pmic->ops.outside(p->pmic->cookie,v,n)==1;
}
static int config_valid(const struct a733_pcie_config *c)
{
    if(!c->owner||!c->identity||!c->topology||!c->deadline||
        c->lifetime_deadline<c->deadline||c->lifetime_deadline>UINT64_MAX-100000||
        c->pck_client>=A733_GPU_RESOURCE_CLIENTS||c->link_speed<1||c->link_speed>3)return 0;
    for(unsigned i=0;i<4;++i) {
        if(!a733_pin_config_valid_banked(A733_PIN_D,20+i,c->inherited[i]))return 0;
        if(i==2 ? c->inherited[i].function!=1 :
            (c->inherited[i].function!=0&&c->inherited[i].function!=15))return 0;
    }
    for(unsigned i=0;i<3;++i) {
        unsigned mhz=parent_mhz[i][(c->fabric[i]>>24)&7],div=(c->fabric[i]&31)+1;
        if((c->fabric[i]&~fabric_mask[i])||(i&&!(c->fabric[i]&BIT(31)))||!mhz||
            (mhz*1000000u)%div||c->fabric_hz[i]!=mhz*1000000u/div)return 0;
    }
    return 1;
}
int a733_pcie_init(struct a733_pcie *p,const struct a733_pcie_config *c,
    const struct a733_pcie_ops *o,void *cookie,struct a733_clock_leases *clocks,
    struct a733_pins *pins,struct a733_rail_leases *rails,struct a733_pmic *pmic,
    struct a733_gpu_resource_manager *pck)
{
    if(!p||p->self||!c||!o||!clocks||!pins||!rails||!pmic||!pck||
        !clocks->initialized||!pins->initialized||rails->self!=rails||pmic->self!=pmic||
        !pck->initialized||rails->binding.gpu!=pck||rails->binding.clocks!=clocks||
        pmic->clocks!=clocks||pmic->rails!=rails||pmic->pins!=pins||
        !o->now||!o->current||!o->admit||!o->enter||!o->leave||!o->retain||
        !o->read||!o->write||!o->outside||!config_valid(c)||
        !a733_ranges_disjoint(p,sizeof(*p),c,sizeof(*c))||!a733_ranges_disjoint(p,sizeof(*p),o,sizeof(*o))||
        !a733_ranges_disjoint(p,sizeof(*p),clocks,sizeof(*clocks))||!a733_ranges_disjoint(p,sizeof(*p),pins,sizeof(*pins))||
        !a733_ranges_disjoint(p,sizeof(*p),rails,sizeof(*rails))||!a733_ranges_disjoint(p,sizeof(*p),pmic,sizeof(*pmic))||
        !a733_ranges_disjoint(p,sizeof(*p),pck,sizeof(*pck))||o->outside(cookie,p,sizeof(*p))!=1||
        pmic->ops.outside(pmic->cookie,p,sizeof(*p))!=1)return 0;
    *p=(struct a733_pcie){.self=p,.config=*c,.ops=*o,.cookie=cookie,.clocks=clocks,
        .pins=pins,.rails=rails,.pmic=pmic,.pck=pck,.deadline=c->deadline};
    atomic_init(&p->busy,0);return 1;
}
static int timely(struct a733_pcie *p)
{
    uint64_t now;
    if(p->ops.now(p->cookie,&now)!=1||!a733_monotonic_advance(now,&p->last_now))return 0;
    return now<p->deadline&&now<p->config.lifetime_deadline;
}
int a733_pcie_read(struct a733_pcie *p,uintptr_t a,unsigned w,uint32_t *v)
{return timely(p)&&p->ops.read(p->cookie,a,w,v)==1&&timely(p);}
int a733_pcie_write(struct a733_pcie *p,uintptr_t a,unsigned w,uint32_t v)
{return timely(p)&&p->ops.write(p->cookie,a,w,v)==1&&timely(p);}
static int rd(struct a733_pcie *p,uintptr_t a,uint32_t *v)
{return a733_pcie_read(p,a,4,v);}
static int wr(struct a733_pcie *p,uintptr_t a,uint32_t v)
{return a733_pcie_write(p,a,4,v);}
static int check(struct a733_pcie *p,uintptr_t a,uint32_t mask,uint32_t value)
{return rd(p,a,&p->last_value)&&(p->last_value&mask)==value;}
static int update(struct a733_pcie *p,uintptr_t a,uint32_t mask,uint32_t value)
{
    uint32_t old,got;if(!rd(p,a,&old))return 0;
    uint32_t next=(old&~mask)|value;
    return (next==old||wr(p,a,next))&&rd(p,a,&got)&&got==next;
}
static int health(struct a733_pcie *p)
{
    if(p->clock.id) {
        enum a733_clock_result r=a733_clock_token_status(p->clocks,&p->clock);
        if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;
    }
    if(p->phase>RAIL_WAIT) {
        enum a733_rail_result r=a733_rail_status(p->rails,&p->rail);
        if(r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_READY)return -1;
    }
    if(p->pck_lease.token) {
        enum a733_gpu_resource_result r=a733_gpu_resources_status(p->pck,p->config.pck_client,&p->pck_lease);
        if(r==A733_GPU_RESOURCE_BUSY)return 0;if(r!=A733_GPU_RESOURCE_OK)return -1;
    }
    if(p->phase>SUPPLY) {
        enum a733_pmic_status r=a733_pmic_live(p->pmic,&p->pmic_lifetime);
        if(r==A733_PMIC_BUSY)return 0;if(r!=A733_PMIC_READY)return -1;
    }
    if(p->pin.id) {
        int held=0;
        enum a733_pins_result r=a733_pins_held_banked(p->pins,&p->pin,&held,p->deadline);
        if(r==A733_PINS_BUSY)return 0;
        if(r!=A733_PINS_OK||!held)return -1;
    }
    return 1;
}
static enum a733_pcie_result fault(struct a733_pcie *p)
{
    p->failed=1;
    if(p->held){p->ops.retain(p->cookie);p->held=0;}
    if(p->clock.id&&!p->clock_retained) {
        enum a733_clock_result r=a733_clock_retain(p->clocks,&p->clock);
        if(r!=A733_CLOCK_BUSY)p->clock_retained=1;
    }
    if(p->rail.id&&!p->rail_retained) {
        enum a733_rail_result r=a733_rail_retain(p->rails,&p->rail);
        if(r!=A733_RAIL_BUSY)p->rail_retained=1;
    }
    return A733_PCIE_RETAINED;
}
static enum a733_pcie_result finish(struct a733_pcie *p,enum a733_pcie_result r)
{
    if(p->held){p->ops.leave(p->cookie);p->held=0;}
    atomic_store_explicit(&p->busy,0,memory_order_release);return r;
}
static int enter(struct a733_pcie *p)
{
    if(!p||p->self!=p||p->ops.current(p->cookie)!=1)return 0;
    unsigned zero=0;return atomic_compare_exchange_strong_explicit(&p->busy,&zero,1,memory_order_acquire,memory_order_relaxed);
}
static int lock(struct a733_pcie *p)
{
    if(!timely(p))return -1;
    int r=p->ops.enter(p->cookie,p->deadline);
    if(r==0)return timely(p)?0:-1;
    if(r!=1)return -1;p->held=1;return timely(p)?1:-1;
}
static int parent_profile(struct a733_pcie *p)
{
    uint32_t osc,ref,pll,a,b,gate,status;
    /* Fabric and AUX parents use the normalized SYS_CLK24M reference output.
     * The COMB1 PHY also takes raw DCXO through DCXO_SERDES1, and its fixed
     * table is qualified only at 24 MHz. */
    if(!rd(p,0x07090160,&osc)||!rd(p,CCU,&ref)||!a733_clock_ref24_valid(osc,ref)||
        a733_clock_dcxo_hz(osc)!=24000000)return 0;
    unsigned n;
    for(unsigned i=0;i<3;++i)
        if(!check(p,CCU+fabric_off[i],fabric_mask[i],p->config.fabric[i]))return 0;
    if(!rd(p,CCU+0xa0,&pll)||(pll&0xfe000000u)!=0xfe000000u||(pll&(BIT(24)|BIT(1)))||
        !rd(p,CCU+0xa8,&a)||(a&BIT(31))||!rd(p,CCU+0xac,&b)||(b&BIT(27))||
        !rd(p,CCU+0x1908,&gate)||!rd(p,CCU+0x1988,&status))return 0;
    n=((pll>>8)&255)+1;if(n<53||n>105)return 0;
    /* Both fixed PCIe400 and SERDES600 branches, plus selected fabric parents. */
    for(unsigned i=0;i<5;++i) {
        unsigned mhz=i<3?parent_mhz[i][(p->config.fabric[i]>>24)&7]:i==3?400:600;
        unsigned div,branches;
        switch(mhz) {
        case 24:continue;
        case 600:div=(((pll>>20)&7)+1)*2;branches=BIT(11)|BIT(9);break;
        case 400:div=(((pll>>20)&7)+1)*3;branches=BIT(11)|BIT(2)|BIT(1);break;
        case 800:div=((pll>>16)&7)+1;branches=BIT(10);break;
        case 480:div=((pll>>2)&7)+1;branches=BIT(8)|BIT(7);break;
        default:return 0;
        }
        uint32_t enabled=branches<<16;
        if((gate&(branches|enabled))!=(branches|enabled)||(status&enabled)!=enabled||24u*n!=mhz*div)return 0;
    }
    return check(p,CCU+0x13c0,0x8700001f,0x81000005)&&
        check(p,CCU+0x13c4,BIT(16),BIT(16))&&check(p,CCU+0x5c0,BIT(8),BIT(8))&&
        check(p,CCU+0x5e0,BIT(28),BIT(28))&&check(p,CCU+0x1b28,0x30001,0x20000)&&
        check(p,0x070101ac,1,1)&&check(p,0x07067000,0x10f,8)&&check(p,0x07067008,15,8);
}
static int masked(struct a733_pcie *p)
{
    return check(p,0x03400000,1u<<31,0)&&check(p,0x03400114,IRQ_MASK,0)&&
        check(p,0x03400214,IRQ_MASK,0)&&check(p,0x03400314,IRQ_MASK,0);
}
static int link(struct a733_pcie *p)
{return check(p,APP+0xe0c,3,3);}
static int capability(struct a733_pcie *p,uintptr_t base,uint8_t *pcie,uint8_t *msi)
{
    uint32_t v;uint64_t seen=0;*pcie=0;*msi=0;
    if(!a733_pcie_read(p,base+6,2,&v))return 0;
    if(!(v&BIT(4)))return 1;
    if(!a733_pcie_read(p,base+0x34,1,&v))return 0;
    for(unsigned i=0;v&&i<48;++i) {
        if(v<0x40||v>0xfc||(v&3)||(seen&(UINT64_C(1)<<(v/4))))return 0;
        seen|=UINT64_C(1)<<(v/4);unsigned at=v;
        if(!a733_pcie_read(p,base+at,2,&v))return 0;
        unsigned id=v&255,next=(v>>8)&255;
        if(id==0x10) {if(*pcie||at>0xe4)return 0;*pcie=(uint8_t)at;}
        if(id==5) {
            if(*msi||at>0xe8)return 0;*msi=(uint8_t)at;
            if(!a733_pcie_read(p,base+at+2,2,&v)||(v&1))return 0;
        }
        if(id==0x11) {if(at>0xf4||!a733_pcie_read(p,base+at+2,2,&v)||(v&0x8000))return 0;}
        v=next;
    }
    return !v;
}
static int window_valid(const struct a733_pcie *p,unsigned region,uint32_t type,
    uint32_t cpu,uint32_t pci,uint32_t bytes)
{
    if(!region)return type==4&&cpu==CFG&&bytes==4096&&(pci&~UINT32_C(0x70000))==0x01000000u;
    return region==1&&(p->test||p->function)&&!type&&cpu==pci&&!(cpu&4095)&&bytes&&!(bytes&4095)&&
        cpu>=A733_PCIE_MEM&&bytes<=A733_PCIE_MEM_BYTES&&cpu-A733_PCIE_MEM<=A733_PCIE_MEM_BYTES-bytes;
}
int a733_pcie_atu_program(struct a733_pcie *p,unsigned region,uint32_t type,
    uint32_t cpu,uint32_t pci,uint32_t bytes)
{
    if(!window_valid(p,region,type,cpu,pci,bytes))return 0;
    uintptr_t at=A733_PCIE_ATU_OUTBOUND(region);
    p->windows[region]=(struct a733_pcie_window){type,cpu,pci,bytes};
    return wr(p,at+4,0)&&check(p,at+4,BIT(31),0)&&wr(p,at+8,cpu)&&wr(p,at+12,0)&&
        wr(p,at+16,cpu+bytes-1)&&wr(p,at+20,pci)&&wr(p,at+24,0)&&wr(p,at,type)&&wr(p,at+4,BIT(31));
}
int a733_pcie_atu_write_allowed(const struct a733_pcie *p,uintptr_t a,uint32_t v)
{
    if(!p||a<A733_PCIE_ATU_OUTBOUND(0)||a>=A733_PCIE_ATU_OUTBOUND(2))return 0;
    unsigned off=(unsigned)(a-A733_PCIE_ATU_OUTBOUND(0));
    const struct a733_pcie_window *w=&p->windows[off/0x200];
    if(!w->bytes)return 0;
    switch(off%0x200) {
    case 0:return v==w->type;
    case 4:return !v||v==BIT(31);
    case 8:return v==w->cpu;
    case 12:case 24:return !v;
    case 16:return v==w->cpu+w->bytes-1;
    case 20:return v==w->pci;
    default:return 0;
    }
}
static int atu_start(struct a733_pcie *p,unsigned function)
{return a733_pcie_atu_program(p,0,4,CFG,0x01000000u|(function<<16),4096);}
static int root_setup(struct a733_pcie *p)
{
    uint8_t msi;uint32_t flags,control;
    if(!check(p,APP+0xc00,1,0)||!update(p,APP+0xc00,0x41,0x40)||
        !capability(p,DBI,&p->root_cap,&msi)||!p->root_cap||p->root_cap>0xcc||
        !a733_pcie_read(p,DBI+p->root_cap+2,2,&flags)||(flags&0xf0)!=0x40||!(flags&15))return 0;
    if(!update(p,DBI+0x710,0x3f0000,0x10000)||!update(p,DBI+0x80c,0x1ff00,0x100)||
        !wr(p,DBI+0x18,0x00010100)||!wr(p,DBI+0x10,0)||!wr(p,DBI+0x14,0)||
        !a733_pcie_write(p,DBI+4,2,0x102)||!update(p,DBI+0x8bc,1,1))return 0;
    if(!a733_pcie_write(p,DBI+0xa,2,0x0604)||
        !update(p,DBI+p->root_cap+0xc,15,p->config.link_speed)||
        !a733_pcie_read(p,DBI+p->root_cap+0x30,2,&control)||
        !a733_pcie_write(p,DBI+p->root_cap+0x30,2,(control&~15u)|p->config.link_speed)||
        !update(p,DBI+0x8bc,1,0))return 0;
    for(unsigned i=0;i<8;++i)
        if(!wr(p,DBI+0x828+12*i,0)||!wr(p,DBI+0x82c+12*i,UINT32_MAX))return 0;
    return 1;
}
/* The link first trains at 2.5 GT/s. Above Gen1, follow the board kernel's
 * sunxi_pcie_host_speed_change(): with Link Control 2 already at the target,
 * a 0-to-1 transition of DBI+0x80c bit 17 directs a speed change, and the
 * controller clears that bit when it finishes. 1 done, 0 pending, -1 fault. */
static int speed(struct a733_pcie *p)
{
    uint32_t v;
    if(p->config.link_speed==1)return 1;
    if(!p->speed_change) {
        if(!a733_pcie_read(p,DBI+p->root_cap+0x30,2,&v)||(v&15)!=p->config.link_speed||
            !rd(p,DBI+0x80c,&v)||!wr(p,DBI+0x80c,v&~BIT(17))||!wr(p,DBI+0x80c,v|BIT(17)))return -1;
        p->speed_change=1;return 0;
    }
    if(!rd(p,DBI+0x80c,&v))return -1;
    return !(v&BIT(17));
}
/* PCIe Base 6.6.1 allows an endpoint up to 1 s after PERST release to answer
 * configuration requests. Inside that window, vendor ID FFFFh (no successful
 * completion) or 0001h (CRS with software visibility) means not ready yet. */
static int ready(struct a733_pcie *p)
{
    uint32_t v;
    if(!rd(p,CFG,&v))return -1;
    v&=65535;
    if(v==65535||v==1)return p->last_now-p->perst_since<1000000?0:-1;
    return v?1:-1;
}
static int endpoint(struct a733_pcie *p)
{
    uint32_t v;
    if(!rd(p,CFG,&v)||(v&65535)==65535||(v&65535)==1||!(v&65535))return 0;
    p->endpoint.vendor=(uint16_t)v;p->endpoint.device=(uint16_t)(v>>16);
    if(!a733_pcie_read(p,CFG+4,2,&v)||(v&4))return 0;p->endpoint.command=(uint16_t)v;
    if(!rd(p,CFG+8,&p->endpoint.class_revision)||!a733_pcie_read(p,CFG+14,1,&v)||(v&0x7f))return 0;
    p->endpoint.header_type=(uint8_t)v;
    if(!capability(p,CFG,&p->endpoint.pcie_cap,&p->endpoint.msi_cap)||!p->endpoint.pcie_cap||
        !a733_pcie_read(p,CFG+p->endpoint.pcie_cap+2,2,&v)||!(v&15)||((v>>4)&15)>1||
        !a733_pcie_read(p,DBI+p->root_cap+0x12,2,&v)||(v&0x3f0)!=0x10||!(v&15)||
        (v&15)>p->config.link_speed||(p->endpoint.link_status&&(v&0x3ff)!=(p->endpoint.link_status&0x3ffu)))return 0;
    p->endpoint.link_status=(uint16_t)v;return 1;
}
enum a733_pcie_result a733_pcie_poll(struct a733_pcie *p)
{
    if(!enter(p))return A733_PCIE_BUSY;
    if(p->failed)return finish(p,fault(p));
    if(p->test||p->function||p->phase>READY)return finish(p,p->phase==STOPPED?A733_PCIE_STOPPED:A733_PCIE_INVALID);
    p->deadline=p->phase==READY?p->config.lifetime_deadline:p->config.deadline;
    if(!timely(p))return finish(p,fault(p));
    int h=health(p);if(h<0)return finish(p,fault(p));if(!h)return finish(p,A733_PCIE_PENDING);
    if(p->phase==CLOCK) {
        enum a733_clock_result r=a733_clock_acquire(p->clocks,p->config.owner,
            A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC|
            A733_CLOCK_SERDES_COMMON|A733_CLOCK_USB_PCIE_DOMAIN,
            A733_CLOCK_PCIE0|A733_CLOCK_COMB1,p->deadline,&p->clock);
        if(r==A733_CLOCK_BUSY)return finish(p,A733_PCIE_PENDING);
        if(r!=A733_CLOCK_OK)return finish(p,fault(p));++p->phase;
    } else if(p->phase==RAIL) {
        enum a733_rail_result r=a733_rail_acquire(p->rails,p->config.owner,
            A733_RAIL_BIT(A733_RAIL_DCDC2)|A733_RAIL_BIT(A733_RAIL_BLDO1),0,p->deadline,&p->rail);
        if(r==A733_RAIL_BUSY)return finish(p,A733_PCIE_PENDING);
        if(r!=A733_RAIL_PENDING)return finish(p,fault(p));++p->phase;
    } else if(p->phase==RAIL_WAIT) {
        enum a733_rail_result r=a733_rail_poll(p->rails,&p->rail);
        if(r==A733_RAIL_BUSY||r==A733_RAIL_PENDING)return finish(p,A733_PCIE_PENDING);
        if(r!=A733_RAIL_READY)return finish(p,fault(p));++p->phase;
    } else if(p->phase==PCK) {
        struct a733_gpu_power_resources ready;
        enum a733_gpu_resource_result r=a733_gpu_resources_acquire(p->pck,p->config.pck_client,
            A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,p->deadline,&p->pck_lease.token,&ready);
        if(r==A733_GPU_RESOURCE_BUSY)return finish(p,A733_PCIE_PENDING);
        if(r!=A733_GPU_RESOURCE_OK)return finish(p,fault(p));
        p->pck_lease.mask=A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK;p->pck_lease.mode=A733_GPU_RESOURCE_STABLE;++p->phase;
    } else if(p->phase==ADMIT) {
        if(p->ops.admit(p->cookie,&p->config)!=1||!timely(p))return finish(p,fault(p));
        enum a733_pmic_status r=a733_pmic_identity(p->pmic,&p->pmic_lifetime);
        if(r==A733_PMIC_BUSY)return finish(p,A733_PCIE_PENDING);
        if(r!=A733_PMIC_READY)return finish(p,fault(p));++p->phase;
    } else if(p->phase==SUPPLY) {
        enum a733_pmic_status r=a733_pmic_live(p->pmic,&p->pmic_lifetime);
        if(r==A733_PMIC_BUSY||r==A733_PMIC_PENDING)return finish(p,A733_PCIE_PENDING);
        if(r!=A733_PMIC_READY)return finish(p,fault(p));
        if(!p->request.id) {
            r=a733_pmic_submit(p->pmic,p->config.owner,&p->rail,supplies[p->supply_index],p->deadline,&p->request);
            if(r!=A733_PMIC_BUSY&&r!=A733_PMIC_PENDING)return finish(p,fault(p));
        } else {
            struct a733_pmic_snapshot *s=&p->supplies[p->supply_index];
            r=a733_pmic_result(p->pmic,&p->request,s);
            if(r==A733_PMIC_BUSY||r==A733_PMIC_PENDING)return finish(p,A733_PCIE_PENDING);
            if(r!=A733_PMIC_COMPLETE||s->rail!=supplies[p->supply_index]||s->page_before||s->page_after||
                s->chip!=3||s->revision||!s->enabled||!s->decoded||s->programmed_uv!=uv[p->supply_index])return finish(p,fault(p));
            r=a733_pmic_reap(p->pmic,&p->request);
            if(r==A733_PMIC_BUSY)return finish(p,A733_PCIE_PENDING);
            if(r!=A733_PMIC_COMPLETE)return finish(p,fault(p));
            p->request=(struct a733_pmic_request){0};if(++p->supply_index==2)++p->phase;
        }
    } else if(p->phase==PINS) {
        struct a733_pin_request_banked requests[4];
        for(unsigned i=0;i<4;++i)requests[i]=(struct a733_pin_request_banked){
            .bank=A733_PIN_D,.pin=20+i,.expected=p->config.inherited[i],.desired=p->config.inherited[i]};
        const struct a733_pin_output_condition condition={.masks={[A733_PIN_D]=BIT(22)}};
        enum a733_pins_result r=a733_pins_acquire_banked_checked(p->pins,p->config.owner,requests,4,
            &condition,&p->pin,p->deadline);
        if(r==A733_PINS_BUSY)return finish(p,A733_PCIE_PENDING);
        if(r!=A733_PINS_OK||!timely(p))return finish(p,fault(p));
        p->perst_since=p->last_now;++p->phase;
    } else if(p->phase==RESET_RELEASE) {
        enum a733_pins_result r=a733_pins_write_outputs(p->pins,&p->pin,A733_PIN_D,BIT(22),BIT(22),p->deadline);
        if(r==A733_PINS_BUSY)return finish(p,A733_PCIE_PENDING);
        if(r!=A733_PINS_OK||!timely(p))return finish(p,fault(p));
        p->perst_since=p->last_now;++p->phase; /* Taken after the release write. */
    } else {
        int l=lock(p);if(l<0)return finish(p,fault(p));if(!l)return finish(p,A733_PCIE_PENDING);
        int ok=1;
        switch(p->phase) {
        case BASELINE:
            ok=parent_profile(p)&&check(p,CCU+0x1380,BIT(31),0)&&check(p,CCU+0x1384,BIT(31),0)&&
                check(p,CCU+0x138c,0x30000,0)&&check(p,CCU+0x574,0x10002,0)&&
                wr(p,0x03400194,IRQ_MASK)&&masked(p);if(ok)++p->phase;break;
        case ENABLE:
            ok=parent_profile(p)&&update(p,CCU+0x138c,0x30000,0x30000)&&
                update(p,CCU+0x1380,0x8100001f,0x80000000)&&update(p,CCU+0x1384,0x8700001f,0x82000000)&&
                update(p,CCU+0x574,0x10002,0x10002)&&update(p,0x0709016c,BIT(5),BIT(5))&&
                update(p,0x06c00004,0x70000,0x70000)&&update(p,0x06c000f0,BIT(29),BIT(29))&&
                wr(p,0x06c06c44,1)&&check(p,0x06c06c44,UINT32_MAX,1)&&check(p,APP+0xc00,1,0)&&check(p,APP+0xe0c,3,0);
            for(unsigned i=0;ok&&i<16;++i)
                ok=check(p,ATU+i*0x200+4,BIT(31),0)&&check(p,ATU+i*0x200+0x104,BIT(31),0);
            if(ok)++p->phase;break;
        case PHY: {
            int r=a733_pcie_phy_step(p);ok=r>=0;
            if(r==1){p->perst_since=p->last_now;++p->phase;}break;
        }
        case RC:ok=root_setup(p);if(ok)++p->phase;break;
        case RESET_WAIT:if(p->last_now-p->perst_since>=100000)++p->phase;break;
        case TRAIN:ok=update(p,APP+0xc00,0x41,0x41);if(ok)++p->phase;break;
        case LINK:
            if(!rd(p,APP+0xe0c,&p->last_value))ok=0;
            else if((p->last_value&3)==3) {
                int r=speed(p);ok=r>=0;
                if(r>0){ok=atu_start(p,0);if(ok)++p->phase;}
            }
            break;
        case IDENTIFY:
            /* No configuration request until 100 ms after PERST release. */
            if(p->last_now-p->perst_since<100000)break;
            if(!rd(p,ATU+4,&p->last_value))ok=0;
            else if(p->last_value&BIT(31)) {
                int r=link(p)?ready(p):-1;ok=r>=0;
                if(r>0) {
                    ok=endpoint(p)&&parent_profile(p)&&masked(p)&&link(p)&&check(p,CFG+4,4,0);
                    if(ok)++p->phase;
                }
            }
            break;
        case READY:ok=parent_profile(p)&&masked(p)&&link(p)&&check(p,CFG+4,4,0);break;
        default:ok=0;break;
        }
        if(!ok)return finish(p,fault(p));
    }
    if(p->held){p->ops.leave(p->cookie);p->held=0;}
    if(!timely(p))return finish(p,fault(p));
    h=health(p);if(h<0)return finish(p,fault(p));
    return finish(p,p->phase==READY&&h?A733_PCIE_READY:A733_PCIE_PENDING);
}
enum a733_pcie_result a733_pcie_identity(struct a733_pcie *p,struct a733_pcie_endpoint *out)
{
    if(!p||p->self!=p||!a733_pcie_output_outside(p,out,sizeof(*out)))return A733_PCIE_INVALID;
    if(!enter(p))return A733_PCIE_BUSY;
    if(p->failed)return finish(p,A733_PCIE_RETAINED);
    if(p->test||p->function||p->phase>READY)return finish(p,p->phase==STOPPED?A733_PCIE_STOPPED:A733_PCIE_INVALID);
    if(p->phase!=READY)return finish(p,A733_PCIE_PENDING);
    p->deadline=p->config.lifetime_deadline;
    if(!timely(p))return finish(p,fault(p));
    int h=health(p);if(h<0)return finish(p,fault(p));if(!h)return finish(p,A733_PCIE_BUSY);
    *out=p->endpoint;return finish(p,A733_PCIE_READY);
}
enum a733_pcie_result a733_pcie_config_read(struct a733_pcie *p,unsigned function,unsigned offset,
    uint64_t deadline,uint32_t *out)
{
    if(!p||p->self!=p||function>7||offset>4092||(offset&3)||!deadline||
        !a733_pcie_output_outside(p,out,sizeof(*out)))return A733_PCIE_INVALID;
    if(!enter(p))return A733_PCIE_BUSY;
    if(p->failed)return finish(p,A733_PCIE_RETAINED);
    if(p->test||p->function||p->phase>READY)return finish(p,p->phase==STOPPED?A733_PCIE_STOPPED:A733_PCIE_INVALID);
    if(p->phase!=READY)return finish(p,A733_PCIE_PENDING);
    if(function&&!(p->endpoint.header_type&0x80))return finish(p,A733_PCIE_INVALID);
    p->deadline=deadline;
    if(!timely(p))return finish(p,fault(p));
    int h=health(p);if(h<0)return finish(p,fault(p));if(!h)return finish(p,A733_PCIE_BUSY);
    int l=lock(p);if(l<0)return finish(p,fault(p));if(!l)return finish(p,A733_PCIE_BUSY);
    uint32_t v;
    if(!link(p)||!atu_start(p,function)||!check(p,ATU+4,BIT(31),BIT(31))||
        !check(p,CFG+4,4,0)||!rd(p,CFG+offset,&v)||!link(p))return finish(p,fault(p));
    /* Restore function0 before returning, keeping service observations exact. */
    if(function&&(!atu_start(p,0)||!check(p,ATU+4,BIT(31),BIT(31))))return finish(p,fault(p));
    p->ops.leave(p->cookie);p->held=0;
    if(!timely(p))return finish(p,fault(p));
    h=health(p);if(h<0)return finish(p,fault(p));if(!h)return finish(p,A733_PCIE_BUSY);
    *out=v;return finish(p,A733_PCIE_READY);
}

static enum a733_pcie_result claim(struct a733_pcie *p,struct a733_pcie_epf_test *test,
    struct a733_pcie_function *function)
{
    if((!test&&!function)||!enter(p))return A733_PCIE_BUSY;
    if(p->failed)return finish(p,A733_PCIE_RETAINED);
    if(p->test||p->function||p->phase>READY)return finish(p,A733_PCIE_INVALID);
    if(p->phase!=READY)return finish(p,A733_PCIE_PENDING);
    p->deadline=p->config.lifetime_deadline;
    if(!timely(p))return finish(p,fault(p));
    int h=health(p);if(h<0)return finish(p,fault(p));if(!h)return finish(p,A733_PCIE_BUSY);
    int l=lock(p);if(l<0)return finish(p,fault(p));if(!l)return finish(p,A733_PCIE_BUSY);
    if(!parent_profile(p)||!masked(p)||!link(p)||!atu_start(p,0)||
       !check(p,ATU+4,BIT(31),BIT(31))||!endpoint(p)||!check(p,CFG+4,4,0))
        return finish(p,fault(p));
    p->test=test;p->function=function;return finish(p,A733_PCIE_READY);
}
enum a733_pcie_result a733_pcie_test_claim(struct a733_pcie *p,struct a733_pcie_epf_test *test)
{return claim(p,test,NULL);}
enum a733_pcie_result a733_pcie_function_claim(struct a733_pcie *p,struct a733_pcie_function *function)
{return claim(p,NULL,function);}
static enum a733_pcie_result client_lock(struct a733_pcie *p,uint64_t deadline)
{
    if(!enter(p))return A733_PCIE_BUSY;
    if(p->failed)return finish(p,A733_PCIE_RETAINED);
    p->deadline=deadline;
    if(!timely(p))return finish(p,fault(p));
    int h=health(p);if(h<0)return finish(p,fault(p));if(!h)return finish(p,A733_PCIE_BUSY);
    int l=lock(p);if(l<0)return finish(p,fault(p));if(!l)return finish(p,A733_PCIE_BUSY);
    if(!parent_profile(p)||!link(p))return finish(p,fault(p));
    return A733_PCIE_READY;
}
enum a733_pcie_result a733_pcie_test_lock(struct a733_pcie *p,struct a733_pcie_epf_test *test,uint64_t deadline)
{
    if(!p||p->self!=p||!test||p->test!=test||!deadline||deadline>p->config.lifetime_deadline)return A733_PCIE_INVALID;
    return client_lock(p,deadline);
}
enum a733_pcie_result a733_pcie_function_lock(struct a733_pcie *p,struct a733_pcie_function *function,uint64_t deadline)
{
    if(!p||p->self!=p||!function||p->function!=function||p->client_stopped||
       !deadline||deadline>p->config.lifetime_deadline)return A733_PCIE_INVALID;
    return client_lock(p,deadline);
}
enum a733_pcie_result a733_pcie_client_end(struct a733_pcie *p,int ok)
{
    if(!p||p->self!=p||!atomic_load_explicit(&p->busy,memory_order_acquire)||!p->held)return A733_PCIE_INVALID;
    if(!ok||!timely(p)||!link(p))return finish(p,fault(p));
    p->ops.leave(p->cookie);p->held=0;
    int h=health(p);if(h<0)return finish(p,fault(p));
    return finish(p,h?A733_PCIE_READY:A733_PCIE_BUSY);
}
/* The board kernel's suspend clears LTSSM enable before PHY and power
 * teardown. This stop keeps that first step, then holds the endpoint in PERST
 * and requires the link to fall. Function zero and region one must already be
 * cold; the endpoint test client never establishes that. */
enum a733_pcie_result a733_pcie_stop(struct a733_pcie *p,uint64_t deadline)
{
    if(!p||p->self!=p||!deadline||deadline>p->config.lifetime_deadline)return A733_PCIE_INVALID;
    if(!enter(p))return A733_PCIE_BUSY;
    if(p->failed)return finish(p,A733_PCIE_RETAINED);
    if(p->phase==STOPPED)return finish(p,A733_PCIE_STOPPED);
    if(p->phase<READY)return finish(p,A733_PCIE_PENDING);
    if(p->test||(p->function&&!p->client_stopped))return finish(p,A733_PCIE_INVALID);
    p->deadline=deadline;
    if(!timely(p))return finish(p,fault(p));
    int h=health(p);if(h<0)return finish(p,fault(p));if(!h)return finish(p,A733_PCIE_BUSY);
    if(p->phase==STOP_RESET) {
        enum a733_pins_result r=a733_pins_write_outputs(p->pins,&p->pin,A733_PIN_D,BIT(22),0,p->deadline);
        if(r==A733_PINS_BUSY)return finish(p,A733_PCIE_PENDING);
        if(r!=A733_PINS_OK||!timely(p))return finish(p,fault(p));
        p->perst_since=p->last_now;p->phase=STOP_DOWN;return finish(p,A733_PCIE_PENDING);
    }
    int l=lock(p);if(l<0)return finish(p,fault(p));if(!l)return finish(p,A733_PCIE_PENDING);
    int ok;
    if(p->phase==READY) {
        ok=link(p)&&atu_start(p,0)&&check(p,ATU+4,BIT(31),BIT(31))&&check(p,CFG+4,7,0)&&
            (!p->windows[1].bytes||check(p,A733_PCIE_ATU_OUTBOUND(1)+4,BIT(31),0))&&
            wr(p,ATU+4,0)&&check(p,ATU+4,BIT(31),0)&&update(p,APP+0xc00,1,0);
        if(ok)p->phase=STOP_RESET;
    } else {
        ok=p->phase==STOP_DOWN&&rd(p,APP+0xe0c,&p->last_value);
        if(ok&&!(p->last_value&3))p->phase=STOPPED;
    }
    if(!ok)return finish(p,fault(p));
    p->ops.leave(p->cookie);p->held=0;
    if(!timely(p))return finish(p,fault(p));
    h=health(p);if(h<0)return finish(p,fault(p));
    return finish(p,p->phase==STOPPED&&h?A733_PCIE_STOPPED:A733_PCIE_PENDING);
}
