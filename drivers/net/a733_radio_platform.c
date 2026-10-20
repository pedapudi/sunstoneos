#include "a733_clock_reference.h"
#include "a733_radio_platform.h"
#define CCU UINT32_C(0x02002000)
#define SDC UINT32_C(0x04021000)
#define GIC UINT32_C(0x03400000)
#define BIT(n) (UINT32_C(1)<<(n))
enum { ACQUIRE, RESERVE_RAILS, WAIT_RAILS, ADMIT, RAILS, BASELINE, GIC_WAIT, PINS, SUPPLY_WAIT,
       ENABLE_MODULE, MODULE_SETTLE, BUS_GATE, RESET_WAIT, RESET_RELEASE, PREPARE,
       PREPARING, ENUMERATE, ENUMERATING, READY };
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
static int output_disjoint(const struct a733_radio_platform *p,const void *out,size_t bytes)
{
    return separate(p,sizeof(*p),out,bytes)&&separate(p->clocks,sizeof(*p->clocks),out,bytes)&&
        separate(p->pins,sizeof(*p->pins),out,bytes)&&separate(p->rails,sizeof(*p->rails),out,bytes)&&
        separate(p->pmic,sizeof(*p->pmic),out,bytes)&&
        separate(p->rails->binding.gpu,sizeof(*p->rails->binding.gpu),out,bytes);
}
static a733_rail_mask_t rail_scope(enum a733_radio_rail rail)
{
    static const a733_rail_mask_t scopes[]={A733_RAIL_BIT(A733_RAIL_DCDC1),A733_RAIL_BIT(A733_RAIL_DCDC8),
        A733_RAIL_BIT(A733_RAIL_BLDO5),A733_RAIL_BIT(A733_RAIL_CLDO1)};
    return scopes[rail];
}
int a733_radio_platform_rails(const struct a733_radio_platform_config *c,a733_rail_mask_t *out)
{
    if(!c||!out||!c->owner||!c->handoff||!c->deadline_us||c->lifetime_deadline_us<c->deadline_us||!c->topology.identity||
        c->topology.vbat!=A733_RADIO_DCDC1||
        (unsigned)c->topology.pg<A733_RADIO_DCDC8||(unsigned)c->topology.pg>A733_RADIO_CLDO1||
        (unsigned)c->topology.pm<A733_RADIO_DCDC8||(unsigned)c->topology.pm>A733_RADIO_CLDO1||
        (unsigned)c->topology.vddio<A733_RADIO_DCDC8||(unsigned)c->topology.vddio>A733_RADIO_CLDO1)return 0;
    *out=rail_scope(c->topology.vbat)|rail_scope(c->topology.pg)|rail_scope(c->topology.pm)|rail_scope(c->topology.vddio);
    return 1;
}
int a733_radio_platform_scopes(const struct a733_radio_platform_config *c,a733_clock_mask_t *out)
{
    a733_rail_mask_t rails;
    if(!out||!a733_radio_platform_rails(c,&rails))return 0;
    a733_clock_mask_t shared=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC;
    for(unsigned i=0;i<3;++i) {
        unsigned mhz=parent_mhz[i][(c->fabric[i]>>24)&7],div=(c->fabric[i]&31)+1;
        if((c->fabric[i]&~fabric_mask[i])||(i&&!(c->fabric[i]&BIT(31)))||!mhz||
            (mhz*1000000u)%div||c->fabric_hz[i]!=mhz*1000000u/div)return 0;
        if(mhz!=24)shared|=A733_CLOCK_PERI0;
    }
    *out=shared;return 1;
}
static enum a733_radio_platform_result fail(struct a733_radio_platform *p)
{
    p->fault=1;
    if(p->host.initialized)sunxi_sdio_stop(&p->host,SUNXI_SDIO_IO_ERROR);
    if(p->held){p->ops.retain(p->cookie);p->held=0;}
    if(p->clock.id&&!p->retained) {
        enum a733_clock_result r=a733_clock_retain(p->clocks,&p->clock);
        if(r!=A733_CLOCK_BUSY)p->retained=1;
    }
    if(p->rail.id&&!p->rail_retained) {
        enum a733_rail_result r=a733_rail_retain(p->rails,&p->rail);
        if(r!=A733_RAIL_BUSY)p->rail_retained=1;
    }
    return A733_RADIO_RETAINED;
}
static int observe(struct a733_radio_platform *p)
{
    uint64_t now;
    if(p->ops.now_us(p->cookie,&now)!=1||now<p->last_now||now>=p->config.lifetime_deadline_us)return 0;
    p->last_now=now;return 1;
}
static int timely(struct a733_radio_platform *p)
{return observe(p)&&p->last_now<p->deadline;}
static enum a733_radio_platform_result finish(struct a733_radio_platform *p,enum a733_radio_platform_result r)
{
    if(p->held){p->ops.leave(p->cookie);p->held=0;}
    atomic_store_explicit(&p->busy,0,memory_order_release);return r;
}
static int lock(struct a733_radio_platform *p)
{
    if(!timely(p))return -1;
    int r=p->ops.enter(p->cookie,p->deadline);
    if(r==0)return timely(p)?0:-1;
    if(r!=1)return -1;
    p->held=1;return timely(p)?1:-1;
}
enum a733_radio_platform_result a733_radio_platform_retain(struct a733_radio_platform *p)
{
    if(!p||!p->initialized||p->ops.current(p->cookie)!=1)return A733_RADIO_INVALID;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return A733_RADIO_BUSY;
    return finish(p,fail(p));
}
static int rd(struct a733_radio_platform *p,uintptr_t a,uint32_t *v)
{return timely(p)&&p->ops.read(p->cookie,a,v)==1&&timely(p);}
static int wr(struct a733_radio_platform *p,uintptr_t a,uint32_t v)
{return timely(p)&&p->ops.write(p->cookie,a,v)==1&&timely(p);}
static int update(struct a733_radio_platform *p,uintptr_t a,uint32_t mask,uint32_t expected,uint32_t bits)
{
    uint32_t old,got;
    if(!rd(p,a,&old)||(old&mask)!=expected)return 0;
    uint32_t value=(old&~mask)|bits;
    return value==old||(wr(p,a,value)&&rd(p,a,&got)&&got==value);
}
static int clock_live(struct a733_radio_platform *p)
{
    enum a733_clock_result r=a733_clock_token_status(p->clocks,&p->clock);
    return r==A733_CLOCK_OK?1:r==A733_CLOCK_BUSY?0:-1;
}
static int profile(struct a733_radio_platform *p)
{
    uint32_t osc,ref,pll,a,b,gate,status;
    /* Selected fabric and SMHC1 routes use SYS24M/PERI0, not raw DCXO. */
    if(!rd(p,UINT32_C(0x07090160),&osc)||!rd(p,CCU,&ref)||!a733_clock_ref24_valid(osc,ref))return 0;
    unsigned n;
    for(unsigned i=0;i<3;++i)
        if(!rd(p,CCU+fabric_off[i],&a)||(a&fabric_mask[i])!=p->config.fabric[i])return 0;
    if(!rd(p,CCU+0x5c0,&a)||(a&(BIT(29)|BIT(24)))!=BIT(24)||
        !rd(p,CCU+0x5e0,&a)||!(a&BIT(30))||
        !rd(p,CCU+0x59c,&a)||(a&0x30001u)!=0x30001u)return 0;
    if(!(p->shared&A733_CLOCK_PERI0))return 1;
    if(!rd(p,CCU+0xa0,&pll)||(pll&0xfe000000u)!=0xfe000000u||(pll&(BIT(24)|BIT(1)))||
        !rd(p,CCU+0xa8,&a)||(a&BIT(31))||!rd(p,CCU+0xac,&b)||(b&BIT(27))||
        !rd(p,CCU+0x1908,&gate)||!rd(p,CCU+0x1988,&status))return 0;
    n=((pll>>8)&255)+1;if(n<53||n>105)return 0;
    for(unsigned i=0;i<3;++i) {
        unsigned mhz=p->selected_mhz[i],div,branches;
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
static int masked(struct a733_radio_platform *p)
{
    uint32_t v;
    if(!rd(p,GIC,&v))return -1;
    if(v&BIT(31))return 0;
    if(!rd(p,GIC+0x118,&v))return -1;
    if(v&4)return 0;
    if(!rd(p,GIC+0x318,&v))return -1;
    return (v&4)?0:1;
}
static int supply_live(struct a733_radio_platform *p)
{
    enum a733_rail_result r=a733_rail_status(p->rails,&p->rail);
    if(r==A733_RAIL_BUSY||r==A733_RAIL_PENDING)return 0;
    if(r!=A733_RAIL_READY)return -1;
    enum a733_pmic_status status=a733_pmic_live(p->pmic,&p->pmic_lifetime);
    return status==A733_PMIC_READY?1:
        (status==A733_PMIC_PENDING||status==A733_PMIC_BUSY)?0:-1;
}
static int supply_poll(struct a733_radio_platform *p)
{
    int live=supply_live(p);if(live!=1)return live;
    if(p->rail_index==p->rail_count)return 1;
    enum a733_rail rail=p->rail_sequence[p->rail_index];
    enum a733_pmic_status status;
    if(!p->pmic_request.id) {
        status=a733_pmic_submit(p->pmic,p->config.owner,&p->rail,rail,p->deadline,&p->pmic_request);
        if(status==A733_PMIC_BUSY)return 0;
        return status==A733_PMIC_PENDING&&p->pmic_request.id?0:-1;
    }
    struct a733_pmic_snapshot *snapshot=&p->snapshots[p->rail_index];
    status=a733_pmic_result(p->pmic,&p->pmic_request,snapshot);
    if(status==A733_PMIC_PENDING||status==A733_PMIC_BUSY)return 0;
    if(status!=A733_PMIC_COMPLETE||snapshot->rail!=rail||snapshot->page_before||
        snapshot->page_after||snapshot->chip!=3||snapshot->revision||
        snapshot->enabled!=1||snapshot->decoded!=1||
        snapshot->programmed_uv!=(rail==A733_RAIL_DCDC1?3300000u:1800000u))return -1;
    status=a733_pmic_reap(p->pmic,&p->pmic_request);
    if(status==A733_PMIC_BUSY)return 0;
    if(status!=A733_PMIC_COMPLETE)return -1;
    /* Reap is final: the request ticket is gone even if a later time read fails. */
    p->pmic_request=(struct a733_pmic_request){0};++p->rail_index;
    return p->rail_index==p->rail_count?1:0;
}
static int host_time(void *cookie,uint64_t *out)
{
    struct a733_radio_platform *p=cookie;
    if(p->fault||p->ops.current(p->cookie)!=1)return 0;
    if(!timely(p)){fail(p);return 0;}
    if((p->host.state==SUNXI_SDIO_PREPARING||p->host.state==SUNXI_SDIO_QUEUED||
        p->host.state==SUNXI_SDIO_ACTIVE||p->host.state==SUNXI_SDIO_DONE)&&
        p->last_now>=p->host.deadline) {
        sunxi_sdio_stop(&p->host,SUNXI_SDIO_TIMEOUT);fail(p);return 0;
    }
    *out=p->last_now;return 1;
}
static int host_access(struct a733_radio_platform *p,uint32_t off,uint32_t *v,unsigned writing)
{
    if(p->fault||p->ops.current(p->cookie)!=1||off>0x300||(off&3))return 0;
    unsigned entered=0;
    if(!p->core_call) {
        unsigned expected=0;
        if(p->stage!=READY||!p->borrower.generation||
            !atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return 0;
        entered=1;
    }
    int ok=timely(p)&&(writing?p->ops.write(p->cookie,SDC+off,*v):p->ops.read(p->cookie,SDC+off,v))==1&&timely(p);
    if(!ok)fail(p);
    if(entered)atomic_store_explicit(&p->busy,0,memory_order_release);
    return ok;
}
static int host_read(void *p,uint32_t off,uint32_t *v){return host_access(p,off,v,0);}
static int host_write(void *p,uint32_t off,uint32_t v){return host_access(p,off,&v,1);}
static int host_barrier(void *cookie)
{
    struct a733_radio_platform *p=cookie;
    if(p->fault||p->ops.current(p->cookie)!=1)return 0;
    if(!timely(p)||p->ops.barrier(p->cookie)!=1||!timely(p)){fail(p);return 0;}
    return 1;
}
int a733_radio_platform_init(struct a733_radio_platform *p,const struct a733_radio_platform_config *c,
    const struct a733_radio_platform_ops *o,void *cookie,struct a733_clock_leases *clocks,
    struct a733_pins *pins,struct a733_rail_leases *rails,struct a733_pmic *pmic)
{
    a733_clock_mask_t shared;
    if(!p||p->initialized||!c||!o||!o->now_us||!o->current||!o->admit||!o->enter||!o->leave||
        !o->retain||!o->read||!o->write||!o->barrier||!clocks||!clocks->initialized||!pins||!pins->initialized||!rails||rails->self!=rails||rails->binding.clocks!=clocks||!pmic||
        !a733_radio_platform_scopes(c,&shared)||!c->card.supported_ocr||
        (c->card.supported_ocr&~UINT32_C(0x00300000))||!c->card.command_timeout||!c->card.ready_poll_interval||
        !separate(p,sizeof(*p),c,sizeof(*c))||!separate(p,sizeof(*p),o,sizeof(*o))||
        !separate(p,sizeof(*p),clocks,sizeof(*clocks))||!separate(p,sizeof(*p),pins,sizeof(*pins))||
        !separate(p,sizeof(*p),rails,sizeof(*rails))||
        !separate(p,sizeof(*p),rails->binding.gpu,sizeof(*rails->binding.gpu)))return 0;
    for(unsigned i=0;i<8;++i) {
        unsigned bank=i<6?A733_PIN_G:A733_PIN_M,pin=i<6?i:i-6;
        if((c->expected[i].function!=0&&c->expected[i].function!=15)||
            c->desired[i].function!=(i<6?2:i==6?0:1)||
            !a733_pin_config_valid_banked(bank,pin,c->expected[i])||
            !a733_pin_config_valid_banked(bank,pin,c->desired[i]))return 0;
    }
    struct a733_pmic_lifetime identity;
    if(!separate(p,sizeof(*p),pmic,sizeof(*pmic))||pmic->clocks!=clocks||pmic->rails!=rails||
        pmic->pins!=pins||a733_pmic_identity(pmic,&identity)!=A733_PMIC_READY)return 0;
    p->config=*c;p->ops=*o;p->cookie=cookie;p->clocks=clocks;p->pins=pins;p->rails=rails;p->pmic=pmic;
    p->pmic_lifetime=identity;
    (void)a733_radio_platform_rails(c,&p->rail_scopes);
    for(unsigned rail=0;rail<A733_RAIL_COUNT;++rail)
        if(p->rail_scopes&A733_RAIL_BIT(rail))p->rail_sequence[p->rail_count++]=(enum a733_rail)rail;
    p->shared=shared;p->deadline=c->deadline_us;
    for(unsigned i=0;i<3;++i)p->selected_mhz[i]=parent_mhz[i][(c->fabric[i]>>24)&7];
    struct sunxi_sdio_config host={{host_read,host_write,host_barrier,host_time},p,800000};
    if(!sunxi_sdio_init(&p->host,&host)||!sdio_card_init(&p->card,&p->host,&c->card))return 0;
    atomic_init(&p->busy,0);p->initialized=1;return 1;
}
int a733_radio_platform_bluetooth(struct a733_radio_platform *p,const struct a733_radio_bluetooth_pins *b)
{
    if(!p||!p->initialized||p->stage!=ACQUIRE||p->clock.id||p->fault||p->bluetooth.authority||
        !b||!b->authority||!separate(p,sizeof(*p),b,sizeof(*b)))return 0;
    for(unsigned i=0;i<2;++i)
        if((b->expected[i].function!=0&&b->expected[i].function!=15)||
            b->desired[i].function!=(i?0u:1u)||
            !a733_pin_config_valid_banked(A733_PIN_M,3+i,b->expected[i])||
            !a733_pin_config_valid_banked(A733_PIN_M,3+i,b->desired[i]))return 0;
    p->bluetooth=*b;return 1;
}
static enum a733_radio_platform_result metadata(struct a733_radio_platform *p)
{
    if(p->stage==ACQUIRE) {
        enum a733_clock_result r=a733_clock_acquire(p->clocks,p->config.owner,p->shared,
            A733_CLOCK_SDC1|A733_CLOCK_RADIO_MODULE,p->deadline,&p->clock);
        if(r==A733_CLOCK_BUSY)return A733_RADIO_BUSY;
        if(r!=A733_CLOCK_OK||!timely(p))return fail(p);
        p->stage=RESERVE_RAILS;return A733_RADIO_PENDING;
    }
    int live=clock_live(p);
    if(live<0)return fail(p);
    if(!live)return A733_RADIO_BUSY;
    if(p->stage==RESERVE_RAILS) {
        enum a733_rail_result r=a733_rail_acquire(p->rails,p->config.owner,p->rail_scopes,0,p->deadline,&p->rail);
        if(r==A733_RAIL_BUSY)return A733_RADIO_BUSY;
        if(r!=A733_RAIL_PENDING||!p->rail.id||!timely(p))return fail(p);
        p->stage=WAIT_RAILS;return A733_RADIO_PENDING;
    }
    if(p->stage==WAIT_RAILS) {
        enum a733_rail_result r=a733_rail_poll(p->rails,&p->rail);
        if(!timely(p))return fail(p);
        if(r==A733_RAIL_BUSY)return A733_RADIO_BUSY;
        if(r==A733_RAIL_PENDING)return A733_RADIO_PENDING;
        if(r!=A733_RAIL_READY)return fail(p);
        p->stage=ADMIT;return A733_RADIO_PENDING;
    }
    if(p->stage==ADMIT) {
        int r=p->ops.admit(p->cookie,&p->config,&p->clock);
        if((r!=0&&r!=1)||!timely(p))return fail(p);
        if(r==1)p->stage=RAILS;
        return A733_RADIO_PENDING;
    }
    if(p->stage==RAILS) {
        int r=supply_poll(p);
        if(r<0||!timely(p))return fail(p);
        if(r==1)p->stage=BASELINE;
        return A733_RADIO_PENDING;
    }
    if(p->stage==PINS) {
        struct a733_pin_request_banked request[10];
        for(unsigned i=0;i<8;++i)request[i]=(struct a733_pin_request_banked){
            i<6?A733_PIN_G:A733_PIN_M,i<6?i:i-6,p->config.expected[i],p->config.desired[i],0,0,0};
        unsigned count=8;
        if(p->bluetooth.authority){
            for(unsigned i=0;i<2;++i)request[8+i]=(struct a733_pin_request_banked){
                A733_PIN_M,3+i,p->bluetooth.expected[i],p->bluetooth.desired[i],i?0:1,0,0};
            count=10;
        }
        enum a733_pins_result r=a733_pins_acquire_banked(p->pins,p->config.owner,request,count,&p->pin_lease,p->deadline);
        if(!timely(p))return fail(p);
        if(r==A733_PINS_BUSY||r==A733_PINS_CONFLICT)return A733_RADIO_BUSY;
        if(r!=A733_PINS_OK)return fail(p);
        p->wait_since=p->last_now;p->stage=SUPPLY_WAIT;return A733_RADIO_PENDING;
    }
    if(p->stage==SUPPLY_WAIT) {
        if(p->last_now-p->wait_since>=10000)p->stage=ENABLE_MODULE;
        return A733_RADIO_PENDING;
    }
    if(p->stage==ENABLE_MODULE) {
        enum a733_pins_result r=a733_pins_write_outputs(p->pins,&p->pin_lease,A733_PIN_M,2,2,p->deadline);
        if(!timely(p))return fail(p);
        if(r==A733_PINS_BUSY)return A733_RADIO_BUSY;
        if(r!=A733_PINS_OK)return fail(p);
        p->wait_since=p->last_now;p->stage=MODULE_SETTLE;return A733_RADIO_PENDING;
    }
    if(p->stage==MODULE_SETTLE) {
        /* The pinned AIC BSP waits 50ms after WLAN power-on before card rescan. */
        if(p->last_now-p->wait_since>=50000)p->stage=BUS_GATE;
        return A733_RADIO_PENDING;
    }
    return A733_RADIO_INVALID;
}
static enum a733_radio_platform_result registers(struct a733_radio_platform *p)
{
    int r=lock(p);if(r<0)return fail(p);if(!r)return A733_RADIO_BUSY;
    if((p->pin_lease.id&&p->pins->failed)||!profile(p))return fail(p);
    int ok=1;
    if(p->stage==BASELINE) {
        ok=rd(p,CCU+0xd10,&p->initial_module)&&!(p->initial_module&BIT(31))&&
            rd(p,CCU+0xd1c,&p->initial_bus)&&!(p->initial_bus&0x10001u)&&wr(p,GIC+0x198,4);
        if(ok)p->stage=GIC_WAIT;
    } else if(p->stage==GIC_WAIT) {
        r=masked(p);ok=r>=0;if(r==1)p->stage=PINS;
    } else if(p->stage==BUS_GATE) {
        ok=update(p,CCU+0xd10,0x87001f1f,p->initial_module&0x87001f1f,29)&&
            update(p,CCU+0xd1c,0x10001,0,1)&&update(p,CCU+0xd10,0x87001f1f,29,BIT(31)|29);
        if(ok){p->wait_since=p->last_now;p->stage=RESET_WAIT;}
    } else if(p->stage==RESET_WAIT) {
        uint32_t v;ok=rd(p,CCU+0xd1c,&v)&&(v&0x10001u)==1;
        if(ok&&p->last_now-p->wait_since>=10)p->stage=RESET_RELEASE;
    } else if(p->stage==RESET_RELEASE) {
        ok=update(p,CCU+0xd1c,0x10001,1,0x10001);
        if(ok)p->stage=PREPARE;
    } else ok=0;
    return ok&&timely(p)?A733_RADIO_PENDING:fail(p);
}
static enum a733_radio_platform_result controller(struct a733_radio_platform *p)
{
    /* Check shared pin health before each bounded controller/card step. */
    int locked=lock(p);
    if(locked<0)return fail(p);
    if(!locked)return A733_RADIO_BUSY;
    if(p->pins->failed||!timely(p))return fail(p);
    p->ops.leave(p->cookie);p->held=0;
    if(p->stage==PREPARE) {
        if(!sunxi_sdio_prepare(&p->host,p->deadline))return fail(p);
        p->stage=PREPARING;
    } else if(p->stage==PREPARING) {
        p->core_call=1;sunxi_sdio_poll(&p->host);p->core_call=0;
        if(p->host.state==SUNXI_SDIO_FAULT)return fail(p);
        if(p->host.state==SUNXI_SDIO_READY)p->stage=ENUMERATE;
    } else if(p->stage==ENUMERATE) {
        if(!sdio_card_start(&p->card,p->deadline))return fail(p);
        p->stage=ENUMERATING;
    } else if(p->stage==ENUMERATING) {
        p->core_call=1;sdio_card_poll(&p->card);p->core_call=0;
        if(p->card.state==SDIO_CARD_FAULT||p->host.state==SUNXI_SDIO_FAULT)return fail(p);
        if(p->card.state==SDIO_CARD_READY)p->stage=READY;
    } else return fail(p);
    if(!timely(p)||p->fault)return fail(p);
    return p->stage==READY?A733_RADIO_READY:A733_RADIO_PENDING;
}
enum a733_radio_platform_result a733_radio_platform_poll(struct a733_radio_platform *p)
{
    if(!p||!p->initialized)return A733_RADIO_INVALID;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return A733_RADIO_BUSY;
    if(p->fault)return finish(p,fail(p));
    if(p->ops.current(p->cookie)!=1)return finish(p,A733_RADIO_INVALID);
    if(p->stage==READY)return finish(p,p->host.state==SUNXI_SDIO_FAULT?fail(p):A733_RADIO_READY);
    if(!timely(p))return finish(p,fail(p));
    if(p->stage>RAILS) {
        int health=supply_live(p);
        if(health<0)return finish(p,fail(p));
        if(!health)return finish(p,A733_RADIO_BUSY);
    }
    enum a733_radio_platform_result result;
    if(p->stage==ACQUIRE||p->stage==RESERVE_RAILS||p->stage==WAIT_RAILS||p->stage==ADMIT||p->stage==RAILS||p->stage==PINS||
        p->stage==SUPPLY_WAIT||p->stage==ENABLE_MODULE||p->stage==MODULE_SETTLE)result=metadata(p);
    else {
        int live=clock_live(p);
        result=live<0?fail(p):!live?A733_RADIO_BUSY:p->stage>=PREPARE?controller(p):registers(p);
    }
    return finish(p,result);
}
int a733_radio_platform_borrow(struct a733_radio_platform *p,uint64_t owner,
    struct a733_radio_borrow *out,struct sdio_card **card)
{
    if(!p||!p->initialized||!owner||!output_disjoint(p,out,sizeof(*out))||
        !output_disjoint(p,card,sizeof(*card))||!separate(out,sizeof(*out),card,sizeof(*card)))return 0;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return 0;
    int ok=!p->fault&&p->ops.current(p->cookie)==1&&p->stage==READY&&!p->borrower.generation&&
        p->host.state==SUNXI_SDIO_READY&&p->card.state==SDIO_CARD_READY;
    if(ok&&!observe(p)){fail(p);ok=0;}
    if(ok){p->borrower=(struct a733_radio_borrow){p->clock.id,owner};*out=p->borrower;*card=&p->card;}
    (void)finish(p,A733_RADIO_PENDING);return ok;
}
enum a733_radio_platform_result a733_radio_platform_live(struct a733_radio_platform *p,
    const struct a733_radio_borrow *token,uint64_t deadline)
{
    if(!p||!p->initialized||!deadline||!separate(p,sizeof(*p),token,sizeof(*token)))return A733_RADIO_INVALID;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return A733_RADIO_BUSY;
    if(p->fault)return finish(p,fail(p));
    if(p->ops.current(p->cookie)!=1||p->stage!=READY||!token->generation||
        token->generation!=p->borrower.generation||token->owner!=p->borrower.owner)return finish(p,A733_RADIO_INVALID);
    /* Fixed authority outlives individual completed-and-consumed requests.
     * Active or unconsumed work keeps its existing operation deadline. */
    if(!observe(p)||p->host.state==SUNXI_SDIO_FAULT||p->card.state!=SDIO_CARD_READY)return finish(p,fail(p));
    if(p->host.state!=SUNXI_SDIO_READY) {
        if(p->last_now>=p->deadline)return finish(p,fail(p));
        if(deadline!=p->deadline)return finish(p,A733_RADIO_INVALID);
    }
    if(deadline<=p->last_now||deadline>p->config.lifetime_deadline_us)return finish(p,A733_RADIO_INVALID);
    p->deadline=deadline;
    int live=clock_live(p),held=0;
    if(live<0)return finish(p,fail(p));if(!live)return finish(p,A733_RADIO_BUSY);
    int supply=supply_live(p);
    if(supply<0)return finish(p,fail(p));if(!supply)return finish(p,A733_RADIO_BUSY);
    enum a733_pins_result r=a733_pins_held_banked(p->pins,&p->pin_lease,&held,deadline);
    if(r==A733_PINS_BUSY)return finish(p,A733_RADIO_BUSY);
    if(r!=A733_PINS_OK||!held||!timely(p))return finish(p,fail(p));
    int locked=lock(p);if(locked<0)return finish(p,fail(p));if(!locked)return finish(p,A733_RADIO_BUSY);
    if(p->pins->failed||!timely(p))return finish(p,fail(p));
    return finish(p,A733_RADIO_READY);
}
enum a733_radio_platform_result a733_radio_platform_wake(struct a733_radio_platform *p,
    const struct a733_radio_borrow *token,uint64_t deadline,unsigned *level)
{
    if(!p||!p->initialized||!output_disjoint(p,level,sizeof(*level))||
        !separate(token,sizeof(*token),level,sizeof(*level)))return A733_RADIO_INVALID;
    enum a733_radio_platform_result live=a733_radio_platform_live(p,token,deadline);
    if(live!=A733_RADIO_READY)return live;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return A733_RADIO_BUSY;
    uint32_t value;
    enum a733_pins_result r=a733_pins_read_inputs_banked(p->pins,&p->pin_lease,A733_PIN_M,1,&value,deadline);
    if(!timely(p))return finish(p,fail(p));
    if(r==A733_PINS_BUSY)return finish(p,A733_RADIO_BUSY);
    if(r!=A733_PINS_OK)return finish(p,fail(p));
    *level=value&1u;return finish(p,A733_RADIO_READY);
}
enum a733_radio_platform_result a733_radio_platform_bluetooth_wake(struct a733_radio_platform *p,
    const struct a733_radio_borrow *token,uint64_t deadline,unsigned *level)
{
    if(!p||!p->initialized||!p->bluetooth.authority||!output_disjoint(p,level,sizeof(*level))||
        !separate(token,sizeof(*token),level,sizeof(*level)))return A733_RADIO_INVALID;
    enum a733_radio_platform_result live=a733_radio_platform_live(p,token,deadline);
    if(live!=A733_RADIO_READY)return live;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return A733_RADIO_BUSY;
    uint32_t value;
    enum a733_pins_result r=a733_pins_read_inputs_banked(p->pins,&p->pin_lease,A733_PIN_M,16,&value,deadline);
    if(!timely(p))return finish(p,fail(p));if(r==A733_PINS_BUSY)return finish(p,A733_RADIO_BUSY);
    if(r!=A733_PINS_OK)return finish(p,fail(p));*level=(value>>4)&1;return finish(p,A733_RADIO_READY);
}
