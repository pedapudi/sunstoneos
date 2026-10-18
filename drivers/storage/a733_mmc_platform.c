#include "a733_mmc_platform.h"
#include "a733_clock_reference.h"
#include "a733_storage_update.h"
#define CCU UINT32_C(0x02002000)
#define PIO UINT32_C(0x02000000)
#define GIC UINT32_C(0x03400000)
#define BIT(n) (UINT32_C(1)<<(n))
enum { CLOCK,RAIL,RAIL_WAIT,ADMIT,SUPPLIES,BASELINE,GIC_WAIT,PINS,PRESENCE,
       ENABLE,RESET_WAIT,RESET_RELEASE,OPEN,IDENTIFY,READY };
static const unsigned emmc_pins[]={0,1,5,6,8,9,10,11,13,14,15,16};
static const enum a733_rail supply[]={A733_RAIL_DCDC1,A733_RAIL_DC1SW2,A733_RAIL_CLDO5};
static const uint32_t fabric_off[]={0x500,0x580,0x588};
static const uint32_t fabric_mask[]={0x0300001f,0x8700001f,0x8700001f};
static const uint32_t parent_mhz[3][8]={
    {24,0,0,600,0,0,0,0},{24,0,800,600,480,0,0,0},{24,600,0,480,400,0,0,0}
};
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);
}
static int outside(const struct a733_mmc_platform *p,const void *v,size_t n)
{
    return separate(p,sizeof(*p),v,n)&&separate(p->clocks,sizeof(*p->clocks),v,n)&&
        separate(p->pins,sizeof(*p->pins),v,n)&&separate(p->rails,sizeof(*p->rails),v,n)&&
        separate(p->pmic,sizeof(*p->pmic),v,n)&&
        separate(p->rails->binding.gpu,sizeof(*p->rails->binding.gpu),v,n)&&p->ops.outside(p->cookie,v,n)==1&&p->pmic->ops.outside(p->pmic->cookie,v,n)==1;
}
int a733_mmc_platform_scopes(const struct a733_mmc_platform_config *c,a733_clock_mask_t *out)
{
    if(!c||!out||!c->owner||!c->handoff||!c->topology||!c->deadline_us||
       c->lifetime_deadline_us<c->deadline_us||
       (c->kind!=SUNXI_MMC_SD&&c->kind!=SUNXI_MMC_EMMC)||
       (unsigned)c->emmc_host>A733_MMC_EMMC_SDC3||
       (c->kind==SUNXI_MMC_SD&&c->emmc_host!=A733_MMC_EMMC_SDC2)||
       !c->voltage_mask||(c->voltage_mask&~UINT32_C(0x00300000)))return 0;
    a733_clock_mask_t shared=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC;
    for(unsigned i=0;i<3;++i) {
        unsigned mhz=parent_mhz[i][(c->fabric[i]>>24)&7],div=(c->fabric[i]&31)+1;
        if((c->fabric[i]&~fabric_mask[i])||(i&&!(c->fabric[i]&BIT(31)))||!mhz||
           (mhz*1000000u)%div||c->fabric_hz[i]!=mhz*1000000u/div)return 0;
        if(mhz!=24)shared|=A733_CLOCK_PERI0;
    }
    *out=shared;return 1;
}
static enum a733_mmc_platform_result fail(struct a733_mmc_platform *p)
{
    p->fault=1;
    if(p->held){p->ops.retain(p->cookie);p->held=0;}
    if(p->clock.id&&!p->clock_retained) {
        enum a733_clock_result r=a733_clock_retain(p->clocks,&p->clock);
        if(r!=A733_CLOCK_BUSY)p->clock_retained=1;
    }
    if(p->rail.id&&!p->rail_retained) {
        enum a733_rail_result r=a733_rail_retain(p->rails,&p->rail);
        if(r!=A733_RAIL_BUSY)p->rail_retained=1;
    }
    return A733_MMC_RETAINED;
}
static int timely(struct a733_mmc_platform *p)
{
    uint64_t now;
    if((p->update&&!a733_storage_update_alive(p->update))||p->ops.now_us(p->cookie,&now)!=1||now<p->last_now)return 0;
    p->last_now=now;return now<p->deadline&&now<p->config.lifetime_deadline_us;
}
static enum a733_mmc_platform_result finish(struct a733_mmc_platform *p,enum a733_mmc_platform_result r)
{
    if(p->held){p->ops.leave(p->cookie);p->held=0;}
    atomic_store_explicit(&p->busy,0,memory_order_release);return r;
}
static int lock(struct a733_mmc_platform *p)
{
    if(!timely(p))return -1;
    int r=p->ops.enter(p->cookie,p->deadline);
    if(!r)return timely(p)?0:-1;
    if(r!=1)return -1;
    p->held=1;return timely(p)?1:-1;
}
static int rd(struct a733_mmc_platform *p,uintptr_t a,uint32_t *v)
{return timely(p)&&p->ops.read(p->cookie,a,v)==1&&timely(p);}
static int wr(struct a733_mmc_platform *p,uintptr_t a,uint32_t v)
{return timely(p)&&p->ops.write(p->cookie,a,v)==1&&timely(p);}
static int update(struct a733_mmc_platform *p,uintptr_t a,uint32_t mask,uint32_t expected,uint32_t bits)
{
    uint32_t old,got;
    if(!rd(p,a,&old)||(old&mask)!=expected)return 0;
    uint32_t v=(old&~mask)|bits;
    return v==old||(wr(p,a,v)&&rd(p,a,&got)&&got==v);
}
static int profile(struct a733_mmc_platform *p)
{
    uint32_t osc,ref,pll,a,b,gate,status;
    if(!rd(p,UINT32_C(0x07090160),&osc)||(osc&(BIT(28)|BIT(1)))!=(BIT(28)|BIT(1))||
        !rd(p,CCU,&ref)||!a733_clock_ref24_valid(osc,ref))return 0;
    for(unsigned i=0;i<3;++i)
        if(!rd(p,CCU+fabric_off[i],&a)||(a&fabric_mask[i])!=p->config.fabric[i])return 0;
    if(!rd(p,CCU+0x5c0,&a)||(a&(BIT(29)|BIT(24)))!=BIT(24)||
        !rd(p,CCU+0x5e0,&a)||!(a&BIT(30))||
        !rd(p,CCU+0x59c,&a)||(a&0x30001u)!=0x30001u)return 0;
    if(!(p->shared&A733_CLOCK_PERI0))return 1;
    if(!rd(p,CCU+0xa0,&pll)||(pll&0xfe000000u)!=0xfe000000u||(pll&BIT(1))||
        !rd(p,CCU+0xa8,&a)||(a&BIT(31))||!rd(p,CCU+0xac,&b)||(b&BIT(27))||
        !rd(p,CCU+0x1908,&gate)||!rd(p,CCU+0x1988,&status))return 0;
    unsigned n=((pll>>8)&255)+1;if(n<53||n>105)return 0;
    for(unsigned i=0;i<3;++i) {
        unsigned mhz=parent_mhz[i][(p->config.fabric[i]>>24)&7],div,branches;
        switch(mhz) {
        case 24:continue;
        case 600:div=(((pll>>20)&7)+1)*2;branches=BIT(11)|BIT(9);break;
        case 400:div=(((pll>>20)&7)+1)*3;branches=BIT(11)|BIT(2)|BIT(1);break;
        case 800:div=((pll>>16)&7)+1;branches=BIT(10);break;
        case 480:div=((pll>>2)&7)+1;branches=BIT(8)|BIT(7);break;
        default:return 0;
        }
        uint32_t enabled=branches<<16,required=branches|enabled;
        if((gate&required)!=required||(status&enabled)!=enabled||24u*n!=mhz*div)return 0;
    }
    return 1;
}
static int voltage(struct a733_mmc_platform *p,unsigned initial)
{
    uint32_t mode,value,select;
    unsigned shift=p->config.kind==SUNXI_MMC_SD?10:4;
    if(!rd(p,PIO+0x40,&mode)||!rd(p,PIO+0x48,&value))return 0;
    mode=(mode>>shift)&3;value=(value>>shift)&3;
    if(p->config.kind==SUNXI_MMC_SD) {
        if(mode>2||value!=2||!rd(p,PIO+0x70,&select)||!(select&1))return 0;
    } else if(mode!=3||value!=0)return 0; /* PC status uses a different encoding. */
    if(initial)p->voltage_mode=mode;
    return mode==p->voltage_mode;
}
static uint32_t irq_bit(const struct a733_mmc_platform *p)
{return p->config.kind==SUNXI_MMC_SD?BIT(1):p->config.emmc_host==A733_MMC_EMMC_SDC3?BIT(4):BIT(3);}
static int masked(struct a733_mmc_platform *p)
{
    uint32_t v,mask=irq_bit(p);
    if(!rd(p,GIC,&v))return -1;
    if(v&BIT(31))return 0;
    if(!rd(p,GIC+0x118,&v))return -1;
    if(v&mask)return 0;
    if(!rd(p,GIC+0x318,&v))return -1;
    if(v&mask)return 0;
    if(!rd(p,GIC+0x218,&v))return -1;
    return (v&mask)?-1:1; /* Preserve an unexplained inherited pending interrupt. */
}
static int live(struct a733_mmc_platform *p)
{
    enum a733_clock_result c=a733_clock_token_status(p->clocks,&p->clock);
    if(c==A733_CLOCK_BUSY)return 0;
    if(c!=A733_CLOCK_OK)return -1;
    if(p->rail.id) {
        enum a733_rail_result r=a733_rail_status(p->rails,&p->rail);
        if(r==A733_RAIL_BUSY||r==A733_RAIL_PENDING)return 0;
        if(r!=A733_RAIL_READY)return -1;
    }
    enum a733_pmic_status s=a733_pmic_live(p->pmic,&p->power);
    if(s==A733_PMIC_PENDING||s==A733_PMIC_BUSY)return 0;
    if(s!=A733_PMIC_READY)return -1;
    if(p->pin.id) {
        int held=0;
        enum a733_pins_result r=a733_pins_held_banked(p->pins,&p->pin,&held,p->deadline);
        if(r==A733_PINS_BUSY)return 0;
        if(r!=A733_PINS_OK||!held)return -1;
    }
    return timely(p)?1:-1;
}
static int presence(struct a733_mmc_platform *p)
{
    uint32_t v=0;unsigned sd=p->config.kind==SUNXI_MMC_SD;
    enum a733_pins_result r=a733_pins_read_inputs_banked(p->pins,&p->pin,
        sd?A733_PIN_F:A733_PIN_C,sd?BIT(6):BIT(1),&v,p->deadline);
    if(r==A733_PINS_BUSY)return 0;
    if(r!=A733_PINS_OK||!timely(p))return -1;
    return sd?(v?-1:1):(v==BIT(1)?1:-1);
}
static int supply_poll(struct a733_mmc_platform *p)
{
    if(p->rail_index==3)return 1;
    enum a733_rail rail=supply[p->rail_index];
    enum a733_pmic_status s;
    if(!p->request.id) {
        s=a733_pmic_submit(p->pmic,p->config.owner,&p->rail,rail,p->deadline,&p->request);
        if(s==A733_PMIC_BUSY)return 0;
        return s==A733_PMIC_PENDING&&p->request.id?0:-1;
    }
    struct a733_pmic_snapshot *v=&p->snapshots[p->rail_index];
    s=a733_pmic_result(p->pmic,&p->request,v);
    if(s==A733_PMIC_BUSY||s==A733_PMIC_PENDING)return 0;
    if(s!=A733_PMIC_COMPLETE||v->rail!=rail||v->page_before||v->page_after||
       v->chip!=3||v->revision||v->enabled!=1||v->decoded!=1||
       v->programmed_uv!=(rail==A733_RAIL_CLDO5?1800000u:3300000u))return -1;
    s=a733_pmic_reap(p->pmic,&p->request);
    if(s==A733_PMIC_BUSY)return 0;
    if(s!=A733_PMIC_COMPLETE)return -1;
    p->request=(struct a733_pmic_request){0};++p->rail_index;return p->rail_index==3?1:0;
}
/* Core callbacks execute only within one admitted, mutex-held poll. Pin and
 * shared-manager methods are never called recursively through these callbacks. */
static int core_guard(struct a733_mmc_platform *p)
{return p->core_call&&p->held&&!p->fault&&(!p->update||a733_storage_update_alive(p->update))&&p->ops.current(p->cookie)==1&&timely(p);}
static int core_read(void *v,uint32_t off,uint32_t *out)
{struct a733_mmc_platform *p=v;int ok=core_guard(p)&&rd(p,p->base+off,out);if(!ok)fail(p);return ok;}
static int core_write(void *v,uint32_t off,uint32_t value)
{struct a733_mmc_platform *p=v;int ok=core_guard(p)&&wr(p,p->base+off,value);if(!ok)fail(p);return ok;}
static int core_barrier(void *v)
{struct a733_mmc_platform *p=v;int ok=core_guard(p)&&p->ops.barrier(p->cookie)==1&&timely(p);if(!ok)fail(p);return ok;}
static int core_now(void *v,uint64_t *out)
{struct a733_mmc_platform *p=v;if(!core_guard(p)){fail(p);return 0;}*out=p->last_now;return 1;}
static int core_retained(void *v,uintptr_t token)
{struct a733_mmc_platform *p=v;return token==(uintptr_t)p&&core_guard(p);}
static int core_admit(void *v,uintptr_t token,enum sunxi_mmc_kind kind,uint32_t hz)
{struct a733_mmc_platform *p=v;return kind==p->config.kind&&hz==800000&&core_retained(v,token);}
int a733_mmc_platform_init(struct a733_mmc_platform *p,const struct a733_mmc_platform_config *c,
    const struct a733_mmc_platform_ops *o,void *cookie,struct a733_clock_leases *clocks,
    struct a733_pins *pins,struct a733_rail_leases *rails,struct a733_pmic *pmic)
{
    a733_clock_mask_t shared;
    if(!p||p->initialized||!a733_mmc_platform_scopes(c,&shared)||!o||!o->now_us||!o->current||
       !o->admit||!o->enter||!o->leave||!o->retain||!o->read||!o->write||!o->barrier||!o->outside||
       !clocks||!clocks->initialized||!pins||!pins->initialized||!rails||rails->self!=rails||
       rails->binding.clocks!=clocks||!rails->binding.gpu||!pmic||pmic->clocks!=clocks||
       pmic->rails!=rails||pmic->pins!=pins||!pmic->ops.outside||
       pmic->ops.outside(pmic->cookie,p,sizeof(*p))!=1||!separate(p,sizeof(*p),c,sizeof(*c))||
       !separate(p,sizeof(*p),o,sizeof(*o))||!separate(p,sizeof(*p),clocks,sizeof(*clocks))||
       !separate(p,sizeof(*p),pins,sizeof(*pins))||!separate(p,sizeof(*p),rails,sizeof(*rails))||
       !separate(p,sizeof(*p),pmic,sizeof(*pmic))||
       !separate(p,sizeof(*p),rails->binding.gpu,sizeof(*rails->binding.gpu))||o->outside(cookie,p,sizeof(*p))!=1)return 0;
    unsigned sd=c->kind==SUNXI_MMC_SD,count=sd?7:12,bank=sd?A733_PIN_F:A733_PIN_C;
    if(sd&&!a733_mmc_sd0_inherited_mux(c->expected))return 0;
    for(unsigned i=0;i<count;++i) {
        unsigned pin=sd?i:emmc_pins[i],fn=sd?(pin>=1&&pin<=3?2:0):((pin==5||pin==6||pin==10)?3+(unsigned)c->emmc_host:0);
        if((!sd&&c->expected[i].function!=0&&c->expected[i].function!=15)||c->desired[i].function!=fn||
           !a733_pin_config_valid_banked(bank,pin,c->expected[i])||
           !a733_pin_config_valid_banked(bank,pin,c->desired[i]))return 0;
        if((sd&&pin!=2)||(!sd&&pin!=0&&pin!=5))if(c->desired[i].pull!=1)return 0;
    }
    struct a733_pmic_lifetime identity;
    if(a733_pmic_identity(pmic,&identity)!=A733_PMIC_READY)return 0;
    unsigned index=sd?0:c->emmc_host==A733_MMC_EMMC_SDC3?3:2;
    *p=(struct a733_mmc_platform){.config=*c,.ops=*o,.cookie=cookie,.clocks=clocks,.pins=pins,
        .rails=rails,.pmic=pmic,.power=identity,.shared=shared,.deadline=c->deadline_us,
        .base=0x04020000+index*0x1000,.module=CCU+0xd00+index*0x10,.bus=CCU+0xd0c+index*0x10};
    struct sunxi_mmc_config cfg={{core_read,core_write,core_barrier,core_now,core_admit,core_retained},
        p,(uintptr_t)p,c->kind,800000,c->voltage_mask};
    if(!sunxi_mmc_init(&p->core,&cfg))return 0;
    atomic_init(&p->busy,0);p->initialized=1;return 1;
}
static enum a733_mmc_platform_result metadata(struct a733_mmc_platform *p)
{
    if(p->stage==CLOCK) {
        enum a733_clock_result r=a733_clock_acquire(p->clocks,p->config.owner,p->shared,
            p->config.kind==SUNXI_MMC_SD?A733_CLOCK_SDC0:(A733_CLOCK_SDC2|A733_CLOCK_SDC3),p->deadline,&p->clock);
        if(r==A733_CLOCK_BUSY)return A733_MMC_BUSY;
        if(r!=A733_CLOCK_OK||!timely(p))return fail(p);
        p->stage=RAIL;return A733_MMC_PENDING;
    }
    if(p->stage==RAIL) {
        a733_rail_mask_t mask=A733_RAIL_BIT(A733_RAIL_DCDC1)|A733_RAIL_BIT(A733_RAIL_DC1SW2)|A733_RAIL_BIT(A733_RAIL_CLDO5);
        enum a733_rail_result r=a733_rail_acquire(p->rails,p->config.owner,mask,0,p->deadline,&p->rail);
        if(r==A733_RAIL_BUSY)return A733_MMC_BUSY;
        if(r!=A733_RAIL_PENDING||!p->rail.id||!timely(p))return fail(p);
        p->stage=RAIL_WAIT;return A733_MMC_PENDING;
    }
    if(p->stage==RAIL_WAIT) {
        enum a733_rail_result r=a733_rail_poll(p->rails,&p->rail);
        if(!timely(p))return fail(p);
        if(r==A733_RAIL_BUSY)return A733_MMC_BUSY;
        if(r==A733_RAIL_PENDING)return A733_MMC_PENDING;
        if(r!=A733_RAIL_READY)return fail(p);
        p->stage=ADMIT;return A733_MMC_PENDING;
    }
    if(p->stage==ADMIT) {
        int r=p->ops.admit(p->cookie,&p->config,&p->clock);
        if((r!=0&&r!=1)||!timely(p))return fail(p);
        if(r==1)p->stage=SUPPLIES;
        return A733_MMC_PENDING;
    }
    if(p->stage==SUPPLIES) {
        int r=supply_poll(p);
        if(r<0||!timely(p))return fail(p);
        if(r==1)p->stage=BASELINE;
        return A733_MMC_PENDING;
    }
    if(p->stage==PINS) {
        unsigned sd=p->config.kind==SUNXI_MMC_SD,count=sd?7:12;
        struct a733_pin_request_banked requests[12];
        for(unsigned i=0;i<count;++i)requests[i]=(struct a733_pin_request_banked){
            sd?A733_PIN_F:A733_PIN_C,sd?i:emmc_pins[i],p->config.expected[i],p->config.desired[i],0,0,0};
        enum a733_pins_result r=a733_pins_acquire_banked(p->pins,p->config.owner,requests,count,&p->pin,p->deadline);
        if(!timely(p))return fail(p);
        if(r==A733_PINS_BUSY||r==A733_PINS_CONFLICT)return A733_MMC_BUSY;
        if(r!=A733_PINS_OK)return fail(p);
        p->stage=PRESENCE;return A733_MMC_PENDING;
    }
    if(p->stage==PRESENCE) {
        int r=presence(p);
        if(r<0)return fail(p);
        if(!r)return A733_MMC_BUSY;
        p->stage=ENABLE;return A733_MMC_PENDING;
    }
    return fail(p);
}
static int running(struct a733_mmc_platform *p)
{
    uint32_t module,bus,ndma;
    if(p->pins->failed||!profile(p)||!voltage(p,0)||masked(p)!=1||
       !rd(p,p->module,&module)||(module&0x87001f1fu)!=0x8000001du||
       !rd(p,p->bus,&bus)||(bus&0x10001u)!=0x10001u)return 0;
    return p->config.kind!=SUNXI_MMC_EMMC||p->config.emmc_host!=A733_MMC_EMMC_SDC3||
        (rd(p,p->base+0x7c,&ndma)&&!(ndma&3));
}
static enum a733_mmc_platform_result registers(struct a733_mmc_platform *p)
{
    int r=lock(p);if(r<0)return fail(p);if(!r)return A733_MMC_BUSY;
    if((p->pin.id&&p->pins->failed)||!profile(p)||!voltage(p,p->stage==BASELINE))return fail(p);
    int ok=1;
    if(p->stage==BASELINE) {
        uint32_t enabled,pending,active;
        ok=rd(p,p->module,&p->initial_module)&&!(p->initial_module&BIT(31))&&
            rd(p,p->bus,&p->initial_bus)&&!(p->initial_bus&0x10001u)&&
            rd(p,GIC+0x118,&enabled)&&rd(p,GIC+0x218,&pending)&&rd(p,GIC+0x318,&active)&&
            !((enabled|pending|active)&irq_bit(p))&&wr(p,GIC+0x198,irq_bit(p));
        if(ok)p->stage=GIC_WAIT;
    } else if(p->stage==GIC_WAIT) {
        r=masked(p);ok=r>=0;if(r==1)p->stage=PINS;
    } else if(p->stage==ENABLE) {
        ok=masked(p)==1&&update(p,p->module,0x87001f1f,p->initial_module&0x87001f1f,29)&&
            update(p,p->bus,0x10001,0,1)&&update(p,p->module,0x87001f1f,29,0x8000001d);
        if(ok){p->reset_since=p->last_now;p->stage=RESET_WAIT;}
    } else if(p->stage==RESET_WAIT) {
        uint32_t v;ok=rd(p,p->bus,&v)&&(v&0x10001u)==1;
        if(ok&&p->last_now-p->reset_since>=10)p->stage=RESET_RELEASE;
    } else if(p->stage==RESET_RELEASE) {
        ok=masked(p)==1&&update(p,p->bus,0x10001,1,0x10001);
        if(ok)p->stage=OPEN;
    } else ok=0;
    return ok&&timely(p)?A733_MMC_PENDING:fail(p);
}
static enum a733_mmc_platform_result controller(struct a733_mmc_platform *p)
{
    int r=presence(p);if(r<0)return fail(p);if(!r)return A733_MMC_BUSY;
    r=lock(p);if(r<0)return fail(p);if(!r)return A733_MMC_BUSY;
    if(!running(p))return fail(p);
    p->core_call=1;
    if(p->stage==OPEN) {
        if(!sunxi_mmc_open(&p->core,p->deadline)){p->core_call=0;return fail(p);}
        p->stage=IDENTIFY;
    } else sunxi_mmc_poll(&p->core);
    p->core_call=0;
    if(p->fault||p->core.state==SUNXI_MMC_FAULT||!timely(p))return fail(p);
    if(p->core.state==SUNXI_MMC_READY){p->stage=READY;return A733_MMC_READY;}
    if(p->core.state==SUNXI_MMC_DONE)return A733_MMC_COMPLETE;
    return A733_MMC_PENDING;
}
static int enter_api(struct a733_mmc_platform *p)
{
    unsigned zero=0;
    return p&&p->initialized&&atomic_compare_exchange_strong_explicit(&p->busy,&zero,1,memory_order_acquire,memory_order_relaxed);
}
static enum a733_mmc_platform_result poll_owned(struct a733_mmc_platform *p,struct a733_storage_update *owner)
{
    if(!p||!p->initialized||p->update!=owner)return A733_MMC_INVALID;
    if(!enter_api(p))return A733_MMC_BUSY;
    if(p->fault)return finish(p,fail(p));
    if(p->ops.current(p->cookie)!=1)return finish(p,A733_MMC_INVALID);
    if(p->stage==READY&&p->core.state==SUNXI_MMC_READY)return finish(p,A733_MMC_READY);
    if(p->stage==READY&&p->core.state==SUNXI_MMC_DONE)return finish(p,A733_MMC_COMPLETE);
    if(!timely(p))return finish(p,fail(p));
    if(p->stage>CLOCK) {
        /* The opening rail reservation must be advanced before live() can
         * require an ACTIVE token. Other stages require the complete graph. */
        int r;
        if(p->stage<=RAIL_WAIT) {
            enum a733_clock_result c=a733_clock_token_status(p->clocks,&p->clock);
            r=c==A733_CLOCK_OK?1:c==A733_CLOCK_BUSY?0:-1;
        } else r=live(p);
        if(r<0)return finish(p,fail(p));
        if(!r)return finish(p,A733_MMC_BUSY);
    }
    enum a733_mmc_platform_result result;
    if(p->stage<=SUPPLIES||p->stage==PINS||p->stage==PRESENCE)result=metadata(p);
    else if(p->stage<OPEN)result=registers(p);
    else result=controller(p);
    return finish(p,result);
}
static enum a733_mmc_platform_result command(struct a733_mmc_platform *p,unsigned operation,
    uint64_t first,uint32_t blocks,void *out,size_t bytes,uint64_t deadline,struct a733_storage_update *owner)
{
    if(!p||!p->initialized||p->update!=owner||!out||!outside(p,out,bytes)||!deadline||deadline>p->config.lifetime_deadline_us)return A733_MMC_INVALID;
    if(!enter_api(p))return A733_MMC_BUSY;
    if(p->fault)return finish(p,fail(p));
    if(p->ops.current(p->cookie)!=1||p->stage!=READY||
       (operation==1?p->core.state!=SUNXI_MMC_DONE:p->core.state!=SUNXI_MMC_READY)||
       (p->core.borrowed&&!separate(out,bytes,p->core.borrowed,p->core.borrowed_bytes)))return finish(p,A733_MMC_INVALID);
    if(operation==0||operation==3) {
        const struct sunxi_mmc_card *c=&p->core.card;
        if(!blocks||blocks>SUNXI_MMC_MAX_BLOCKS||bytes!=(size_t)blocks*512||
           first>=c->sectors||blocks>c->sectors-first)
            return finish(p,A733_MMC_INVALID);
        uint64_t last=first+blocks-1;
        if((c->sector_addressed&&last>UINT32_MAX)||(!c->sector_addressed&&last>UINT32_MAX/512u))
            return finish(p,A733_MMC_INVALID);
    }
    p->deadline=deadline;
    int r=live(p);if(r<0)return finish(p,fail(p));if(!r)return finish(p,A733_MMC_BUSY);
    r=presence(p);if(r<0)return finish(p,fail(p));if(!r)return finish(p,A733_MMC_BUSY);
    r=lock(p);if(r<0)return finish(p,fail(p));if(!r)return finish(p,A733_MMC_BUSY);
    if(!running(p)||!timely(p))return finish(p,fail(p));
    p->core_call=1;
    if(operation==0||operation==3) {
        int ok;
        if(operation==3) {
            const struct sunxi_mmc_write_authority authority={owner,a733_storage_update_authorize};
            ok=owner&&a733_storage_update_owned(owner,p)&&
                sunxi_mmc_write(&p->core,&owner->request,&authority);
        }else ok=sunxi_mmc_read(&p->core,first,blocks,out,bytes,deadline);
        p->core_call=0;
        if(p->fault||p->core.state==SUNXI_MMC_FAULT)return finish(p,fail(p));
        return finish(p,ok?A733_MMC_PENDING:A733_MMC_INVALID);
    }
    p->core_call=0;
    /* Successful take is the final metadata operation: no fallible callback
     * observes time or hardware after releasing the borrowed buffer. */
    if(operation==1)return finish(p,sunxi_mmc_take(&p->core,out)?A733_MMC_READY:A733_MMC_INVALID);
    *(struct sunxi_mmc_card *)out=p->core.card;return finish(p,A733_MMC_READY);
}
enum a733_mmc_platform_result a733_mmc_platform_read(struct a733_mmc_platform *p,
    uint64_t first,uint32_t blocks,uint8_t *buffer,size_t bytes,uint64_t deadline)
{return command(p,0,first,blocks,buffer,bytes,deadline,NULL);}
enum a733_mmc_platform_result a733_mmc_platform_take(struct a733_mmc_platform *p,
    struct sunxi_mmc_result *out,uint64_t deadline)
{return command(p,1,0,0,out,sizeof(*out),deadline,NULL);}
enum a733_mmc_platform_result a733_mmc_platform_card(struct a733_mmc_platform *p,
    struct sunxi_mmc_card *out,uint64_t deadline)
{return command(p,2,0,0,out,sizeof(*out),deadline,NULL);}

enum a733_mmc_platform_result a733_mmc_platform_poll(struct a733_mmc_platform *p)
{return poll_owned(p,NULL);}
int a733_mmc_platform_update_bind(struct a733_mmc_platform *p,struct a733_storage_update *u)
{
    if(!p||!p->initialized||!u||!outside(p,u,sizeof(*u))||!a733_storage_update_owned(u,p)||!enter_api(p))return 0;
    int ok=!p->update&&!p->fault&&p->stage==READY&&p->core.state==SUNXI_MMC_READY&&p->ops.current(p->cookie)==1;
    if(ok)p->update=u;
    atomic_store_explicit(&p->busy,0,memory_order_release);return ok;
}
enum a733_mmc_platform_result a733_mmc_platform_update_poll(struct a733_mmc_platform *p,struct a733_storage_update *u)
{return u&&a733_storage_update_owned(u,p)?poll_owned(p,u):A733_MMC_INVALID;}
enum a733_mmc_platform_result a733_mmc_platform_update_io(struct a733_mmc_platform *p,struct a733_storage_update *u,
    unsigned op,uint64_t first,uint32_t blocks,void *buffer,size_t bytes,uint64_t deadline)
{
    if(!u||!a733_storage_update_owned(u,p)||(op!=0&&op!=1&&op!=3))return A733_MMC_INVALID;
    if(op==0) {
        uint64_t offset=u->state==A733_STORAGE_READ_START?u->read_blocks:u->written_blocks;
        if((u->state!=A733_STORAGE_READ_START&&u->state!=A733_STORAGE_VERIFY_START)||
           buffer!=u->readback||bytes!=u->chunk_bytes||blocks!=bytes/512||
           first!=u->grant.first+offset||deadline!=u->operation_deadline)return A733_MMC_INVALID;
    }
    if(op==1&&(buffer!=&u->result||bytes!=sizeof(u->result)||deadline!=u->operation_deadline||
       (u->state!=A733_STORAGE_READ_WAIT&&u->state!=A733_STORAGE_WRITE_WAIT&&u->state!=A733_STORAGE_VERIFY_WAIT)))return A733_MMC_INVALID;
    if(op==3&&(buffer!=u->request.source||bytes!=u->request.bytes||first!=u->request.first_sector||
       blocks!=u->request.blocks||deadline!=u->request.deadline_us))return A733_MMC_INVALID;
    return command(p,op,first,blocks,buffer,bytes,deadline,u);
}
void a733_mmc_platform_update_retain(struct a733_mmc_platform *p,struct a733_storage_update *u)
{if(p&&p->update==u&&enter_api(p))finish(p,fail(p));}

int a733_mmc_platform_storage(struct a733_mmc_platform *p,const void *v,size_t n)
{return p&&p->initialized&&outside(p,v,n);}

enum a733_mmc_platform_result a733_mmc_platform_retain(struct a733_mmc_platform *p)
{
    if(!p||!p->initialized)return A733_MMC_INVALID;
    if(!enter_api(p))return A733_MMC_BUSY;
    if(p->update||p->ops.current(p->cookie)!=1)return finish(p,A733_MMC_INVALID);
    (void)fail(p);
    enum a733_mmc_platform_result r=
        ((p->clock.id&&!p->clock_retained)||(p->rail.id&&!p->rail_retained))?
        A733_MMC_BUSY:A733_MMC_RETAINED;
    return finish(p,r);
}
