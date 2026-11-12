#include "a733_clock_reference.h"
#include "a733_radio_platform.h"
#include "a733_range.h"
#define CCU UINT32_C(0x02002000)
#define SDC UINT32_C(0x04021000)
#define GIC UINT32_C(0x03400000)
#define BIT(n) (UINT32_C(1)<<(n))

#define MODULE_24M (BIT(31)|29) /* SYS24M, N=1, M=30: 800 kHz module clock. */
#define RADIO_IO_CODE 13u /* BLDO5 selector for 1.8 V, as the vendor wlan_power_vol = 1800000. */
#define WIDE_DIVISOR 8u /* PERI0 400 MHz / 8: 50 MHz module, 25 MHz card clock. */
static const uint32_t fabric_off[]={0x500,0x580,0x588};
static const uint32_t fabric_mask[]={0x0300001f,0x8700001f,0x8700001f};
static const uint32_t parent_mhz[3][8]={
    {24,0,0,600,0,0,0,0},{24,0,800,600,480,0,0,0},{24,600,0,480,400,0,0,0}
};
static int output_disjoint(const struct a733_radio_platform *p,const void *out,size_t bytes)
{
    return a733_ranges_disjoint(p,sizeof(*p),out,bytes)&&a733_ranges_disjoint(p->clocks,sizeof(*p->clocks),out,bytes)&&
        a733_ranges_disjoint(p->pins,sizeof(*p->pins),out,bytes)&&a733_ranges_disjoint(p->rails,sizeof(*p->rails),out,bytes)&&
        a733_ranges_disjoint(p->pmic,sizeof(*p->pmic),out,bytes)&&
        a733_ranges_disjoint(p->rails->binding.gpu,sizeof(*p->rails->binding.gpu),out,bytes);
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
        c->topology.vbat!=A733_RADIO_DCDC1||c->topology.pg!=A733_RADIO_BLDO5||c->topology.pm!=A733_RADIO_CLDO1||
        (c->topology.vddio!=A733_RADIO_BLDO5&&c->topology.vddio!=A733_RADIO_CLDO1))return 0;
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
#define ABORT_US 2000000u /* The failure shutdown has this long, within the lifetime. */
static int supply_live(struct a733_radio_platform *p);
static int abort_possible(struct a733_radio_platform *p)
{
    if(p->fault||p->stop_requested||p->aborting||p->abort_done||!(p->power_on||p->already_on)||
       p->stage<RSTAGE_SUPPLY_WAIT||p->stage>RSTAGE_READY||!p->pin_lease.id||
       p->last_now+ABORT_US>p->config.lifetime_deadline_us||p->ops.current(p->cookie)!=1||
       a733_rail_status(p->rails,&p->rail)!=A733_RAIL_READY||
       a733_pmic_live(p->pmic,&p->pmic_lifetime)!=A733_PMIC_READY)return 0;
    return 1;
}
static enum a733_radio_platform_result fail_at(struct a733_radio_platform *p,unsigned line)
{
    if(line&&!p->fail_line){p->fail_line=line;p->fail_stage=p->stage;}
    /* An abort is never cut short: while it runs inside its deadline the failure waits for it. */
    if(p->aborting&&!p->stop_requested&&p->last_now<p->deadline)return A733_RADIO_PENDING;
    if(a733_radio_platform_clean(p)){
        /* Nothing is powered: leave the register lock and keep every lease as it is. */
        p->fault=1;
        if(p->host.initialized)sunxi_sdio_stop(&p->host,SUNXI_SDIO_IO_ERROR);
        if(p->held){p->ops.leave(p->cookie);p->held=0;}
        return A733_RADIO_RETAINED;
    }
    if(abort_possible(p)){
        /* The module is powered: undo that before retaining. The controller is stopped now (software
         * only); the abort steps run from the next polls with a fresh deadline. */
        p->aborting=1;p->abort_from=p->stage;p->abort_flags=1;
        if(p->host.initialized)sunxi_sdio_stop(&p->host,SUNXI_SDIO_IO_ERROR);
        p->deadline=p->last_now+ABORT_US;p->stage=RSTAGE_ABORT_PM1;
        return A733_RADIO_PENDING;
    }
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
#define fail(p) fail_at((p),(unsigned)__LINE__)
static int observe(struct a733_radio_platform *p)
{
    uint64_t now;
    return p->ops.now_us(p->cookie,&now)==1&&a733_deadline_admit(now,&p->last_now,p->config.lifetime_deadline_us);
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
    return finish(p,fail_at(p,0)); /* An outside retain is not a platform failure site. */
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
static unsigned profile_line(struct a733_radio_platform *p)
{
    uint32_t osc,ref,pll,a,b,gate,status;
    /* Selected fabric and SMHC1 routes use SYS24M/PERI0, not raw DCXO. */
    if(!rd(p,UINT32_C(0x07090160),&osc)||!rd(p,CCU,&ref)||!a733_clock_ref24_valid(osc,ref))return __LINE__;
    unsigned n;
    for(unsigned i=0;i<3;++i)
        if(!rd(p,CCU+fabric_off[i],&a)||(a&fabric_mask[i])!=p->config.fabric[i])return __LINE__;
    if(!rd(p,CCU+0x5c0,&a)||(a&(BIT(29)|BIT(24)))!=BIT(24)||
        !rd(p,CCU+0x5e0,&a)||!(a&BIT(30))||
        !rd(p,CCU+0x59c,&a)||(a&0x30001u)!=0x30001u)return __LINE__;
    if(!(p->shared&A733_CLOCK_PERI0))return 0;
    /* The shared validator ignores the reserved bits 24:23; bit 24 reads 1 on both boards (0xff126310). */
    if(!rd(p,CCU+0xa0,&pll)||!a733_clock_peri0_valid(pll)||
        !rd(p,CCU+0xa8,&a)||(a&BIT(31))||!rd(p,CCU+0xac,&b)||(b&BIT(27))||
        !rd(p,CCU+0x1908,&gate)||!rd(p,CCU+0x1988,&status))return __LINE__;
    n=((pll>>8)&255)+1;if(n<53||n>105)return __LINE__;
    if(p->four_bit) {
        /* The four-bit profile's SMHC1 parent is the same 400 MHz branch the
         * SD/eMMC platform requires for its negotiated rates. */
        uint32_t required=BIT(11)|BIT(2)|BIT(1);unsigned div=(((pll>>20)&7)+1)*3;
        if((gate&(required|(required<<16)))!=(required|(required<<16))||
            (status&(required<<16))!=(required<<16)||24u*n!=400u*div)return __LINE__;
    }
    for(unsigned i=0;i<3;++i) {
        unsigned mhz=p->selected_mhz[i],div,branches;
        switch(mhz) {
        case 24:continue;
        case 600:div=(((pll>>20)&7)+1)*2;branches=BIT(11)|BIT(9);break;
        case 400:div=(((pll>>20)&7)+1)*3;branches=BIT(11)|BIT(2)|BIT(1);break;
        case 800:div=((pll>>16)&7)+1;branches=BIT(10);break;
        case 480:div=((pll>>2)&7)+1;branches=BIT(8)|BIT(7);break;
        default:return __LINE__;
        }
        uint32_t enabled=branches<<16,required=branches|enabled;
        if((gate&required)!=required||(status&enabled)!=enabled||24u*n!=mhz*div)return __LINE__;
    }
    return 0;
}
static int profile(struct a733_radio_platform *p)
{
    unsigned line=profile_line(p);
    if(line&&!p->fail_line){p->fail_line=line;p->fail_stage=p->stage;}
    return !line;
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
        snapshot->page_after||snapshot->chip!=3||snapshot->revision)return -1;
    /* Before power-on, BLDO5 may be off at any selector; the radio power-on
     * then programs it. Every other supply, and BLDO5 once on, must be enabled
     * at its programmed voltage. */
    if(rail==A733_RAIL_DCDC8)/* Guard observation: the raw bytes are recorded, never required. */;
    else if(rail==A733_RAIL_BLDO5&&!p->rail_verify&&!snapshot->enabled)p->power_on=1;
    else if(snapshot->enabled!=1||snapshot->decoded!=1||
        snapshot->programmed_uv!=(rail==A733_RAIL_DCDC1?3300000u:1800000u))return -1;
    else if(rail==A733_RAIL_BLDO5&&!p->rail_verify&&p->first_run_guard)p->already_on=1; /* Left on by a reset. */
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
        p->host.state==SUNXI_SDIO_ACTIVE||p->host.state==SUNXI_SDIO_DONE||p->host.state==SUNXI_SDIO_WIDENING||
        p->host.state==SUNXI_SDIO_QUIESCING)&&
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
        if(p->stop_requested||p->stage!=RSTAGE_READY||!p->borrower.generation||
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
/* Runs only from the widening controller poll, with the platform register
 * lock held. Card output is stopped; the shared PERI0 parent is never written.
 * Each readback becomes live state, so a failed later step retains it. */
static int host_module(void *cookie,unsigned divisor)
{
    struct a733_radio_platform *p=cookie;
    if(p->fault||p->ops.current(p->cookie)!=1||!p->core_call||!p->held||!p->four_bit||
        p->stage!=RSTAGE_WIDENING||divisor!=WIDE_DIVISOR||p->pins->failed||!timely(p)||!profile(p)){fail(p);return 0;}
    uint32_t old=p->running_module,next=UINT32_C(0x81000000)|(divisor-1);
    if(!update(p,CCU+0xd10,0x87001f1f,old,old&~BIT(31))||
        !update(p,CCU+0xd10,0x87001f1f,old&~BIT(31),next&~BIT(31))||
        !update(p,CCU+0xd10,0x87001f1f,next&~BIT(31),next)||!profile(p)||p->pins->failed||!timely(p)){fail(p);return 0;}
    p->running_module=next;return 1;
}
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
        !a733_ranges_disjoint(p,sizeof(*p),c,sizeof(*c))||!a733_ranges_disjoint(p,sizeof(*p),o,sizeof(*o))||
        !a733_ranges_disjoint(p,sizeof(*p),clocks,sizeof(*clocks))||!a733_ranges_disjoint(p,sizeof(*p),pins,sizeof(*pins))||
        !a733_ranges_disjoint(p,sizeof(*p),rails,sizeof(*rails))||
        !a733_ranges_disjoint(p,sizeof(*p),rails->binding.gpu,sizeof(*rails->binding.gpu)))return 0;
    for(unsigned i=0;i<8;++i) {
        unsigned bank=i<6?A733_PIN_G:A733_PIN_M,pin=i<6?i:i-6;
        if((c->expected[i].function!=0&&c->expected[i].function!=15)||
            c->desired[i].function!=(i<6?2:i==6?0:1)||
            !a733_pin_config_valid_banked(bank,pin,c->expected[i])||
            !a733_pin_config_valid_banked(bank,pin,c->desired[i]))return 0;
    }
    struct a733_pmic_lifetime identity;
    if(!a733_ranges_disjoint(p,sizeof(*p),pmic,sizeof(*pmic))||pmic->clocks!=clocks||pmic->rails!=rails||
        pmic->pins!=pins||a733_pmic_identity(pmic,&identity)!=A733_PMIC_READY)return 0;
    p->config=*c;p->ops=*o;p->cookie=cookie;p->clocks=clocks;p->pins=pins;p->rails=rails;p->pmic=pmic;
    p->pmic_lifetime=identity;
    (void)a733_radio_platform_rails(c,&p->rail_scopes);
    for(unsigned rail=0;rail<A733_RAIL_COUNT;++rail)
        if(p->rail_scopes&A733_RAIL_BIT(rail))p->rail_sequence[p->rail_count++]=(enum a733_rail)rail;
    p->shared=shared;p->deadline=c->deadline_us;
    for(unsigned i=0;i<3;++i)p->selected_mhz[i]=parent_mhz[i][(c->fabric[i]>>24)&7];
    struct sunxi_sdio_config host={{host_read,host_write,host_barrier,host_time,host_module},p,800000};
    if(!sunxi_sdio_init(&p->host,&host)||!sdio_card_init(&p->card,&p->host,&c->card))return 0;
    atomic_init(&p->busy,0);p->initialized=1;return 1;
}
int a733_radio_platform_four_bit(struct a733_radio_platform *p)
{
    if(!p||!p->initialized||p->stage!=RSTAGE_ACQUIRE||p->clock.id||p->fault||p->stop_requested||p->four_bit)return 0;
    p->four_bit=1;p->shared|=A733_CLOCK_PERI0;return 1;
}
int a733_radio_platform_first_run_guard(struct a733_radio_platform *p)
{
    if(!p||!p->initialized||p->stage!=RSTAGE_ACQUIRE||p->clock.id||p->fault||p->stop_requested||p->first_run_guard||p->rail_count>=4)return 0;
    /* DCDC8 joins the shared lease as an observation only; rebuild the sequence. */
    p->first_run_guard=1;p->rail_scopes|=rail_scope(A733_RADIO_DCDC8);p->rail_count=0;
    for(unsigned rail=0;rail<A733_RAIL_COUNT;++rail)
        if(p->rail_scopes&A733_RAIL_BIT(rail))p->rail_sequence[p->rail_count++]=(enum a733_rail)rail;
    return 1;
}
int a733_radio_platform_bluetooth(struct a733_radio_platform *p,const struct a733_radio_bluetooth_pins *b)
{
    if(!p||!p->initialized||p->stage!=RSTAGE_ACQUIRE||p->clock.id||p->fault||p->stop_requested||p->bluetooth.authority||
        !b||!b->authority||!a733_ranges_disjoint(p,sizeof(*p),b,sizeof(*b)))return 0;
    for(unsigned i=0;i<2;++i)
        if((b->expected[i].function!=0&&b->expected[i].function!=15)||
            b->desired[i].function!=(i?0u:1u)||
            !a733_pin_config_valid_banked(A733_PIN_M,3+i,b->expected[i])||
            !a733_pin_config_valid_banked(A733_PIN_M,3+i,b->desired[i]))return 0;
    p->bluetooth=*b;return 1;
}
int a733_radio_platform_bluetooth_stop(struct a733_radio_platform *p,const struct a733_radio_bluetooth_stop *b)
{
    if(!p||!p->initialized||p->stage!=RSTAGE_ACQUIRE||p->clock.id||p->fault||p->stop_requested||
       !p->bluetooth.authority||p->bluetooth_stop.authority||!b||!output_disjoint(p,b,sizeof(*b))||
       !b->authority||b->topology!=p->config.topology.identity||b->expected.function!=1||
       !a733_pin_config_valid_banked(A733_PIN_M,2,b->expected)||b->inherited_level>1||b->reserved)return 0;
    unsigned evidence=0;
    for(unsigned i=0;i<sizeof(b->wiring_evidence);++i)evidence|=b->wiring_evidence[i];
    if(!evidence||!(p->pins->handoff.authorized[A733_PIN_M]&4)||
       !(p->pins->handoff.latch[1].known_mask&4)||
       !!(p->pins->handoff.latch[1].value&4)!=b->inherited_level)return 0;
    p->bluetooth_stop=*b;return 1;
}
static enum a733_radio_platform_result metadata(struct a733_radio_platform *p)
{
    if(p->stage==RSTAGE_ACQUIRE) {
        enum a733_clock_result r=a733_clock_acquire(p->clocks,p->config.owner,p->shared,
            A733_CLOCK_SDC1|A733_CLOCK_RADIO_MODULE,p->deadline,&p->clock);
        if(r==A733_CLOCK_BUSY)return A733_RADIO_BUSY;
        if(r!=A733_CLOCK_OK||!timely(p))return fail(p);
        p->stage=RSTAGE_RESERVE_RAILS;return A733_RADIO_PENDING;
    }
    int live=clock_live(p);
    if(live<0)return fail(p);
    if(!live)return A733_RADIO_BUSY;
    if(p->stage==RSTAGE_RESERVE_RAILS) {
        /* BLDO5 is exclusive: only an exclusive holder may program it. */
        const a733_rail_mask_t io=A733_RAIL_BIT(A733_RAIL_BLDO5);
        enum a733_rail_result r=a733_rail_acquire(p->rails,p->config.owner,p->rail_scopes&~io,p->rail_scopes&io,p->deadline,&p->rail);
        if(r==A733_RAIL_BUSY)return A733_RADIO_BUSY;
        if(r!=A733_RAIL_PENDING||!p->rail.id||!timely(p))return fail(p);
        p->stage=RSTAGE_WAIT_RAILS;return A733_RADIO_PENDING;
    }
    if(p->stage==RSTAGE_WAIT_RAILS) {
        enum a733_rail_result r=a733_rail_poll(p->rails,&p->rail);
        if(!timely(p))return fail(p);
        if(r==A733_RAIL_BUSY)return A733_RADIO_BUSY;
        if(r==A733_RAIL_PENDING)return A733_RADIO_PENDING;
        if(r!=A733_RAIL_READY)return fail(p);
        p->stage=RSTAGE_ADMIT;return A733_RADIO_PENDING;
    }
    if(p->stage==RSTAGE_ADMIT) {
        int r=p->ops.admit(p->cookie,&p->config,&p->clock);
        if((r!=0&&r!=1)||!timely(p))return fail(p);
        if(r==1)p->stage=RSTAGE_RAILS;
        return A733_RADIO_PENDING;
    }
    if(p->stage==RSTAGE_RAILS) {
        int r=supply_poll(p);
        if(r<0||!timely(p))return fail(p);
        if(r==1)p->stage=RSTAGE_BASELINE;
        return A733_RADIO_PENDING;
    }
    if(p->stage==RSTAGE_PINS) {
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
        /* Vendor order: PM1 is low before any supply is enabled. */
        p->wait_since=p->last_now;p->stage=p->power_on?RSTAGE_POWER_ON:RSTAGE_SUPPLY_WAIT;return A733_RADIO_PENDING;
    }
    if(p->stage==RSTAGE_POWER_ON) {
        enum a733_pmic_status r=a733_pmic_submit_power_on(p->pmic,p->config.owner,&p->rail,A733_RAIL_BLDO5,
            RADIO_IO_CODE,p->deadline,&p->pmic_request);
        if(!timely(p))return fail(p);
        if(r==A733_PMIC_BUSY)return A733_RADIO_BUSY;
        if(r!=A733_PMIC_PENDING||!p->pmic_request.id)return fail(p);
        p->stage=RSTAGE_POWER_WAIT;return A733_RADIO_PENDING;
    }
    if(p->stage==RSTAGE_POWER_WAIT) {
        struct a733_pmic_snapshot written;
        enum a733_pmic_status r=a733_pmic_result(p->pmic,&p->pmic_request,&written);
        if(!timely(p))return fail(p);
        if(r==A733_PMIC_PENDING||r==A733_PMIC_BUSY)return A733_RADIO_PENDING;
        if(r!=A733_PMIC_COMPLETE||written.rail!=A733_RAIL_BLDO5||written.page_before||written.page_after||
            written.chip!=3||written.revision||written.enabled!=1||written.decoded!=1||
            (written.selector&31u)!=RADIO_IO_CODE)return fail(p);
        r=a733_pmic_reap(p->pmic,&p->pmic_request);
        if(r==A733_PMIC_BUSY)return A733_RADIO_PENDING;
        if(r!=A733_PMIC_COMPLETE)return fail(p);
        p->pmic_request=(struct a733_pmic_request){0};
        /* Read every supply back once more, now strictly, before PM1 rises. */
        p->rail_verify=1;p->rail_index=0;p->stage=RSTAGE_VERIFY_RAILS;return A733_RADIO_PENDING;
    }
    if(p->stage==RSTAGE_VERIFY_RAILS) {
        int r=supply_poll(p);
        if(r<0||!timely(p))return fail(p);
        if(r==1){p->wait_since=p->last_now;p->stage=RSTAGE_SUPPLY_WAIT;}
        return A733_RADIO_PENDING;
    }
    if(p->stage==RSTAGE_SUPPLY_WAIT) {
        if(p->last_now-p->wait_since>10000)p->stage=RSTAGE_ENABLE_MODULE;
        return A733_RADIO_PENDING;
    }
    if(p->stage==RSTAGE_ENABLE_MODULE) {
        enum a733_pins_result r=a733_pins_write_outputs(p->pins,&p->pin_lease,A733_PIN_M,2,2,p->deadline);
        if(!timely(p))return fail(p);
        if(r==A733_PINS_BUSY)return A733_RADIO_BUSY;
        if(r!=A733_PINS_OK)return fail(p);
        p->wait_since=p->last_now;p->stage=RSTAGE_MODULE_SETTLE;return A733_RADIO_PENDING;
    }
    if(p->stage==RSTAGE_MODULE_SETTLE) {
        /* The pinned AIC BSP waits 50ms after WLAN power-on before card rescan. */
        if(p->last_now-p->wait_since>50000)p->stage=RSTAGE_BUS_GATE;
        return A733_RADIO_PENDING;
    }
    return A733_RADIO_INVALID;
}
static enum a733_radio_platform_result registers(struct a733_radio_platform *p)
{
    int r=lock(p);if(r<0)return fail(p);if(!r)return A733_RADIO_BUSY;
    if((p->pin_lease.id&&p->pins->failed)||!profile(p))return fail(p);
    int ok=1;
    if(p->stage==RSTAGE_BASELINE) {
        ok=rd(p,CCU+0xd10,&p->initial_module)&&!(p->initial_module&BIT(31))&&
            rd(p,CCU+0xd1c,&p->initial_bus)&&!(p->initial_bus&0x10001u)&&wr(p,GIC+0x198,4);
        if(ok)p->stage=RSTAGE_GIC_WAIT;
    } else if(p->stage==RSTAGE_GIC_WAIT) {
        r=masked(p);ok=r>=0;if(r==1)p->stage=p->first_run_guard?RSTAGE_GUARD:RSTAGE_PINS;
    } else if(p->stage==RSTAGE_GUARD) {
        /* Read-only: pad power words, then every condition before any write. */
        static const uintptr_t words[4]={UINT32_C(0x02000040),UINT32_C(0x02000048),UINT32_C(0x07025340),UINT32_C(0x07025348)};
        for(unsigned i=0;ok&&i<4;++i)ok=rd(p,words[i],&p->pad_power[i]);
        if(ok) {
            p->guard_done=1;
            /* Refuse only a PG reading of 3.3 V (10): the manual defines no "unsupplied" reading (00 and 01 are Invalid),
             * the board reads 11 with BLDO5 off and on, and 3.3 V would be another supply that could fight BLDO5.
             * BLDO5 must be off, or (left on by a reset) on at 1.8V. */
            unsigned pg=(p->pad_power[1]>>12)&3,unmet=pg!=2?0u:1u;
            for(unsigned i=0;i<p->rail_count;++i) {
                const struct a733_pmic_snapshot *s=&p->snapshots[i];
                if(s->rail==A733_RAIL_DCDC1)unmet|=(s->enabled&&(s->selector&31u)==23)?0u:2u;
                else if(s->rail==A733_RAIL_BLDO5)unmet|=(p->already_on?s->enabled:!s->enabled)?0u:4u;
                else if(s->rail==A733_RAIL_CLDO1)unmet|=(s->enabled&&s->decoded&&s->programmed_uv==1800000)?0u:8u;
            }
            if(unmet){
                /* Observations stay in the object; nothing was written. The refusal names the PG bits and the
                 * unmet conditions, and ends without retaining anything. */
                if(!p->fail_line){p->fail_line=0x400u|pg<<4|unmet;p->fail_stage=p->stage;}
                p->refused=1;ok=0;
            }
            else p->stage=RSTAGE_PINS;
        }
    } else if(p->stage==RSTAGE_BUS_GATE) {
        ok=update(p,CCU+0xd10,0x87001f1f,p->initial_module&0x87001f1f,29)&&
            update(p,CCU+0xd1c,0x10001,0,1)&&update(p,CCU+0xd10,0x87001f1f,29,MODULE_24M);
        if(ok){p->running_module=MODULE_24M;p->wait_since=p->last_now;p->stage=RSTAGE_RESET_WAIT;}
    } else if(p->stage==RSTAGE_RESET_WAIT) {
        uint32_t v;ok=rd(p,CCU+0xd1c,&v)&&(v&0x10001u)==1;
        if(ok&&p->last_now-p->wait_since>=10)p->stage=RSTAGE_RESET_RELEASE;
    } else if(p->stage==RSTAGE_RESET_RELEASE) {
        ok=update(p,CCU+0xd1c,0x10001,1,0x10001);
        if(ok)p->stage=RSTAGE_PREPARE;
    } else ok=0;
    return ok&&timely(p)?A733_RADIO_PENDING:fail(p);
}
/* One CCCR bus-interface CMD52 per call sequence: admit, then poll to result. */
static int cccr_width(struct a733_radio_platform *p,int write,uint8_t value,uint8_t *out)
{
    if(!p->card_pending) {
        struct sunxi_sdio_request r;
        if(!sdio_card_cmd52(&p->card,0,7,write,write,value,&r)||!sunxi_sdio_submit(&p->host,&r,p->deadline))return -1;
        p->card_pending=1;return 0;
    }
    p->core_call=1;sunxi_sdio_poll(&p->host);p->core_call=0;
    if(p->host.state==SUNXI_SDIO_FAULT)return -1;
    if(p->host.state!=SUNXI_SDIO_DONE)return 0;
    struct sunxi_sdio_result result;
    if(!sunxi_sdio_take_result(&p->host,&result))return -1;
    p->card_pending=0;*out=(uint8_t)result.response;return 1;
}
static enum a733_radio_platform_result controller(struct a733_radio_platform *p)
{
    /* Check shared pin health before each bounded controller/card step. */
    int locked=lock(p);
    if(locked<0)return fail(p);
    if(!locked)return A733_RADIO_BUSY;
    if(p->pins->failed||!timely(p))return fail(p);
    /* Only the module-clock change inside widening writes shared CCU state. */
    if(p->stage!=RSTAGE_WIDENING){p->ops.leave(p->cookie);p->held=0;}
    if(p->stage==RSTAGE_PREPARE) {
        if(!sunxi_sdio_prepare(&p->host,p->deadline))return fail(p);
        p->stage=RSTAGE_PREPARING;
    } else if(p->stage==RSTAGE_PREPARING) {
        p->core_call=1;sunxi_sdio_poll(&p->host);p->core_call=0;
        if(p->host.state==SUNXI_SDIO_FAULT)return fail(p);
        if(p->host.state==SUNXI_SDIO_READY)p->stage=RSTAGE_ENUMERATE;
    } else if(p->stage==RSTAGE_ENUMERATE) {
        if(!sdio_card_start(&p->card,p->deadline))return fail(p);
        p->stage=RSTAGE_ENUMERATING;
    } else if(p->stage==RSTAGE_ENUMERATING) {
        p->core_call=1;sdio_card_poll(&p->card);p->core_call=0;
        if(p->card.state==SDIO_CARD_FAULT||p->host.state==SUNXI_SDIO_FAULT)return fail(p);
        /* A low-speed card stays at the one-bit identification clock. */
        if(p->card.state==SDIO_CARD_READY)p->stage=p->four_bit&&!(p->card.capabilities&0x40)?RSTAGE_WIDE_READ:RSTAGE_READY;
    } else if(p->stage==RSTAGE_WIDE_READ||p->stage==RSTAGE_WIDE_WRITE||p->stage==RSTAGE_WIDE_VERIFY) {
        uint8_t value=0;
        int r=cccr_width(p,p->stage==RSTAGE_WIDE_WRITE,(uint8_t)((p->bus_interface&~3u)|2u),&value);
        if(r<0)return fail(p);
        if(r) {
            if(p->stage==RSTAGE_WIDE_READ){p->bus_interface=value;p->stage=RSTAGE_WIDE_WRITE;}
            else if((value&3u)!=2u)return fail(p);
            else p->stage=p->stage==RSTAGE_WIDE_WRITE?RSTAGE_WIDEN:RSTAGE_READY;
        }
    } else if(p->stage==RSTAGE_WIDEN) {
        if(!sunxi_sdio_widen(&p->host,WIDE_DIVISOR,p->deadline))return fail(p);
        p->stage=RSTAGE_WIDENING;
    } else if(p->stage==RSTAGE_WIDENING) {
        p->core_call=1;sunxi_sdio_poll(&p->host);p->core_call=0;
        if(p->host.state==SUNXI_SDIO_FAULT)return fail(p);
        if(p->host.state==SUNXI_SDIO_READY)p->stage=RSTAGE_WIDE_VERIFY;
    } else return fail(p);
    if(!timely(p)||p->fault)return fail(p);
    return p->stage==RSTAGE_READY?A733_RADIO_READY:A733_RADIO_PENDING;
}
int a733_radio_platform_shutdown(struct a733_radio_platform *p,const struct a733_radio_borrow *token,uint64_t deadline)
{
    if(!p||!p->initialized||!token||!a733_ranges_disjoint(p,sizeof(*p),token,sizeof(*token)))return 0;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return 0;
    int ok=!p->fault&&!p->stop_requested&&p->ops.current(p->cookie)==1&&p->stage==RSTAGE_READY&&token->generation&&
        token->generation==p->borrower.generation&&token->owner==p->borrower.owner&&
        p->host.state==SUNXI_SDIO_READY&&p->card.state==SDIO_CARD_READY&&observe(p)&&
        deadline>p->last_now&&deadline<=p->config.lifetime_deadline_us;
    if(ok){p->deadline=deadline;p->stage=RSTAGE_SHUTDOWN_PM1;}
    (void)finish(p,A733_RADIO_PENDING);return ok;
}
int a733_radio_platform_stop(struct a733_radio_platform *p,const struct a733_radio_borrow *token,uint64_t deadline)
{
    if(!p||!p->initialized||!token||!output_disjoint(p,token,sizeof(*token)))return 0;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return 0;
    int ok=p->ops.current(p->cookie)==1&&token->generation==p->borrower.generation&&
        token->owner==p->borrower.owner&&deadline&&deadline<=p->config.lifetime_deadline_us;
    if(ok){
        /* Admission closes even if the clock or existing lifetime has failed.
         * That failure must not obtain the legacy fresh abort allowance. */
        if(!p->stop_requested){p->stop_deadline=deadline;p->stop_released=!p->bluetooth.authority;}
        else if(deadline<p->stop_deadline)p->stop_deadline=deadline;
        p->stop_requested=1;
        if(p->stage>RSTAGE_READY&&p->stage<=RSTAGE_DOWN){
            p->stop_phase=3;p->stop_bus=0x10001;p->stop_from=RSTAGE_READY;
            for(unsigned i=0;i<p->rail_count;++i)if(p->snapshots[i].rail==A733_RAIL_BLDO5)
                p->stop_powered=p->snapshots[i].enabled;
        }
        if(!observe(p)||p->last_now>=p->stop_deadline||p->fault||p->aborting||p->abort_done)
            (void)fail(p);
        else if(p->stage<=RSTAGE_READY)p->deadline=p->stop_deadline;
        else if(p->deadline>p->stop_deadline)p->deadline=p->stop_deadline;
    }
    (void)finish(p,A733_RADIO_PENDING);return ok;
}
int a733_radio_platform_stop_release(struct a733_radio_platform *p,const struct a733_radio_borrow *token)
{
    if(!p||!p->initialized||!token||!output_disjoint(p,token,sizeof(*token)))return 0;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return 0;
    int ok=p->ops.current(p->cookie)==1&&p->stop_requested&&!p->fault&&
        token->generation==p->borrower.generation&&token->owner==p->borrower.owner&&
        observe(p)&&p->last_now<p->stop_deadline;
    if(ok)p->stop_released=1;
    (void)finish(p,A733_RADIO_PENDING);return ok;
}
/* Finish only the accepted host operation. The original stage stays intact:
 * widening's module callback authenticates that stage and the register lock.
 * Higher protocol state and all buffers remain pinned and cannot resume. */
static enum a733_radio_platform_result stop_poll(struct a733_radio_platform *p)
{
    if(!p->stop_phase){
        if(p->host.state==SUNXI_SDIO_FAULT)return fail(p);
        if(p->host.state!=SUNXI_SDIO_READY&&p->host.state!=SUNXI_SDIO_UNPREPARED){
            int live=clock_live(p);if(live<0)return fail(p);if(!live)return A733_RADIO_BUSY;
            int locked=lock(p);if(locked<0)return fail(p);if(!locked)return A733_RADIO_BUSY;
            if(p->pins->failed)return fail(p);
            if(p->host.state!=SUNXI_SDIO_WIDENING){p->ops.leave(p->cookie);p->held=0;}
            p->core_call=1;sunxi_sdio_poll(&p->host);p->core_call=0;
            if(p->host.state==SUNXI_SDIO_FAULT||!timely(p))return fail(p);
            if(p->host.state==SUNXI_SDIO_DONE){
                if(!sunxi_sdio_take_result(&p->host,&p->stop_result)||p->stop_result.error)return fail(p);
                p->stop_completed=1;
            }
            return A733_RADIO_PENDING;
        }
        if(!p->pin_lease.id||p->pmic_request.id||
           (p->host.state==SUNXI_SDIO_UNPREPARED&&p->stage>RSTAGE_PREPARE))return fail(p);
        p->stop_unprepared=p->host.state==SUNXI_SDIO_UNPREPARED;
        p->stop_bus=p->stage<=RSTAGE_BUS_GATE?0:p->stage<=RSTAGE_RESET_RELEASE?1:0x10001;
        /* The latest completed supply observation proves whether an off
         * request is legal. Before POWER_ON, an observed off rail stays off. */
        unsigned observed=0;
        for(unsigned i=0;i<p->rail_count;++i)if(p->snapshots[i].rail==A733_RAIL_BLDO5){
            observed=1;p->stop_powered=p->snapshots[i].enabled;
        }
        if(!observed)return fail(p);
        p->stop_phase=1;return A733_RADIO_PENDING;
    }
    if(!p->stop_released)return A733_RADIO_PENDING;
    if(p->stop_phase==1&&p->bluetooth_stop.authority){
        const struct a733_radio_bluetooth_stop *b=&p->bluetooth_stop;
        struct a733_pin_request_banked request={A733_PIN_M,2,b->expected,b->expected,b->inherited_level,0,0};
        struct a733_pin_output_condition condition={0};
        condition.masks[A733_PIN_M]=4;condition.levels[A733_PIN_M]=(uint32_t)b->inherited_level<<2;
        enum a733_pins_result r=a733_pins_acquire_banked_checked(p->pins,p->config.owner,&request,1,
            &condition,&p->bluetooth_stop_lease,p->deadline);
        if(!timely(p))return fail(p);
        if(r==A733_PINS_BUSY)return A733_RADIO_BUSY;
        if(r!=A733_PINS_OK)return fail(p);
        p->stop_phase=2;return A733_RADIO_PENDING;
    }
    if(p->stop_phase==2){
        enum a733_pins_result r=a733_pins_write_outputs(p->pins,&p->bluetooth_stop_lease,A733_PIN_M,4,0,p->deadline);
        if(!timely(p))return fail(p);
        if(r==A733_PINS_BUSY)return A733_RADIO_BUSY;
        if(r!=A733_PINS_OK)return fail(p);
        p->peer_stopped=1;
    }
    p->stop_from=p->stage;p->stop_phase=3;p->stage=RSTAGE_SHUTDOWN_PM1;return A733_RADIO_PENDING;
}
/* The failure shutdown. Every step is best effort and read back where the hardware answers; a step that
 * cannot be done sets no success bit and the next still runs: PM1 low first (the module off), then the
 * SDC1 clocks, then BLDO5 off. Afterwards the platform retains as it always did. The caller's finish()
 * releases the register lock. */
#define ABORT_PM1_LOW 2u
#define ABORT_CLOCKS_OFF 4u
#define ABORT_BLDO5_OFF 8u
#define ABORT_MATCHED 16u
static enum a733_radio_platform_result abort_end(struct a733_radio_platform *p,unsigned wanted)
{
    if((p->abort_flags&wanted)==wanted)p->abort_flags|=ABORT_MATCHED;
    p->aborting=0;p->abort_done=1;
    return fail_at(p,0); /* Now the ordinary retention. */
}
static enum a733_radio_platform_result abort_poll(struct a733_radio_platform *p)
{
    unsigned wanted=ABORT_PM1_LOW|ABORT_BLDO5_OFF|(p->abort_from>=RSTAGE_BUS_GATE?ABORT_CLOCKS_OFF:0u);
    if(p->stage==RSTAGE_ABORT_PM1) {
        enum a733_pins_result r=a733_pins_write_outputs(p->pins,&p->pin_lease,A733_PIN_M,2,0,p->deadline);
        if(r==A733_PINS_BUSY&&timely(p))return A733_RADIO_BUSY;
        if(r==A733_PINS_OK)p->abort_flags|=ABORT_PM1_LOW;
        p->stage=RSTAGE_ABORT_CLOCKS;return A733_RADIO_PENDING;
    }
    if(p->stage==RSTAGE_ABORT_CLOCKS) {
        if(p->abort_from>=RSTAGE_BUS_GATE) {
            /* The module gate off, then the bus gate and reset asserted, each read back. */
            int locked=lock(p);
            if(locked==0)return A733_RADIO_BUSY;
            if(locked>0) {
                uint32_t old=p->running_module;
                if(update(p,CCU+0xd10,0x87001f1f,old,old&~BIT(31))&&update(p,CCU+0xd1c,0x10001,0x10001,0)) {
                    p->abort_flags|=ABORT_CLOCKS_OFF;p->running_module=old&~BIT(31);
                }
            }
        }
        p->stage=RSTAGE_ABORT_POWER;return A733_RADIO_PENDING;
    }
    if(p->stage==RSTAGE_ABORT_POWER) {
        enum a733_pmic_status r=a733_pmic_submit_power_off(p->pmic,p->config.owner,&p->rail,A733_RAIL_BLDO5,
            RADIO_IO_CODE,p->deadline,&p->pmic_request);
        if(r==A733_PMIC_BUSY&&timely(p))return A733_RADIO_BUSY;
        if(r!=A733_PMIC_PENDING||!p->pmic_request.id)return abort_end(p,wanted);
        p->stage=RSTAGE_ABORT_POWER_WAIT;return A733_RADIO_PENDING;
    }
    struct a733_pmic_snapshot off;
    enum a733_pmic_status r=a733_pmic_result(p->pmic,&p->pmic_request,&off);
    if(r==A733_PMIC_PENDING||r==A733_PMIC_BUSY)return A733_RADIO_PENDING;
    if(r==A733_PMIC_COMPLETE&&off.rail==A733_RAIL_BLDO5&&!off.page_before&&!off.page_after&&off.chip==3&&
       !off.revision&&!off.enabled&&(off.selector&31u)==RADIO_IO_CODE) {
        r=a733_pmic_reap(p->pmic,&p->pmic_request);
        if(r==A733_PMIC_BUSY)return A733_RADIO_PENDING;
        if(r==A733_PMIC_COMPLETE){p->pmic_request=(struct a733_pmic_request){0};p->abort_flags|=ABORT_BLDO5_OFF;}
    }
    return abort_end(p,wanted);
}
/* One bounded shutdown step; the caller's finish() releases the register lock. */
static enum a733_radio_platform_result shutdown_poll(struct a733_radio_platform *p)
{
    if(p->aborting)return abort_poll(p);
    if(p->stage==RSTAGE_DOWN)return A733_RADIO_DOWN;
    if(p->stage==RSTAGE_SHUTDOWN_PM1) {
        enum a733_pins_result r=a733_pins_write_outputs(p->pins,&p->pin_lease,A733_PIN_M,2,0,p->deadline);
        if(!timely(p))return fail(p);
        if(r==A733_PINS_BUSY)return A733_RADIO_BUSY;
        if(r!=A733_PINS_OK)return fail(p);
        p->stage=RSTAGE_SHUTDOWN_CONTROLLER;return A733_RADIO_PENDING;
    }
    if(p->stage==RSTAGE_SHUTDOWN_POWER) {
        if(p->stop_requested?!p->stop_powered:!(p->power_on||p->already_on)){p->stage=RSTAGE_DOWN;return A733_RADIO_DOWN;}
        enum a733_pmic_status r=a733_pmic_submit_power_off(p->pmic,p->config.owner,&p->rail,A733_RAIL_BLDO5,
            RADIO_IO_CODE,p->deadline,&p->pmic_request);
        if(!timely(p))return fail(p);
        if(r==A733_PMIC_BUSY)return A733_RADIO_BUSY;
        if(r!=A733_PMIC_PENDING||!p->pmic_request.id)return fail(p);
        p->stage=RSTAGE_SHUTDOWN_POWER_WAIT;return A733_RADIO_PENDING;
    }
    if(p->stage==RSTAGE_SHUTDOWN_POWER_WAIT) {
        struct a733_pmic_snapshot off;
        enum a733_pmic_status r=a733_pmic_result(p->pmic,&p->pmic_request,&off);
        if(!timely(p))return fail(p);
        if(r==A733_PMIC_PENDING||r==A733_PMIC_BUSY)return A733_RADIO_PENDING;
        if(r!=A733_PMIC_COMPLETE||off.rail!=A733_RAIL_BLDO5||off.page_before||off.page_after||off.chip!=3||
            off.revision||off.enabled||(off.selector&31u)!=RADIO_IO_CODE)return fail(p);
        r=a733_pmic_reap(p->pmic,&p->pmic_request);
        if(r==A733_PMIC_BUSY)return A733_RADIO_PENDING;
        if(r!=A733_PMIC_COMPLETE)return fail(p);
        p->pmic_request=(struct a733_pmic_request){0};
        p->stage=RSTAGE_DOWN;return A733_RADIO_DOWN;
    }
    /* Controller and clock steps hold the shared register lock. */
    int live=clock_live(p);
    if(live<0)return fail(p);
    if(!live)return A733_RADIO_BUSY;
    int locked=lock(p);
    if(locked<0)return fail(p);
    if(!locked)return A733_RADIO_BUSY;
    if(p->pins->failed||!timely(p))return fail(p);
    /* Only the clock step writes shared CCU state under the register lock. */
    if(p->stage!=RSTAGE_SHUTDOWN_CLOCKS){p->ops.leave(p->cookie);p->held=0;}
    if(p->stage==RSTAGE_SHUTDOWN_CONTROLLER) {
        if(p->stop_requested&&p->stop_unprepared){p->stage=RSTAGE_SHUTDOWN_CLOCKS;return A733_RADIO_PENDING;}
        if(!sunxi_sdio_quiesce(&p->host,p->deadline))return fail(p);
        p->stage=RSTAGE_SHUTDOWN_CONTROLLER_WAIT;
    } else if(p->stage==RSTAGE_SHUTDOWN_CONTROLLER_WAIT) {
        p->core_call=1;sunxi_sdio_poll(&p->host);p->core_call=0;
        if(p->host.state==SUNXI_SDIO_FAULT)return fail(p);
        if(p->host.state==SUNXI_SDIO_QUIESCED)p->stage=RSTAGE_SHUTDOWN_CLOCKS;
    } else if(p->stage==RSTAGE_SHUTDOWN_CLOCKS) {
        /* Module gate off, then the bus gate and reset asserted, each read back; the
         * interrupt stays masked and inactive. */
        uint32_t old=p->stop_requested&&!p->stop_bus?p->initial_module&0x87001f1f:p->running_module;
        int ok=update(p,CCU+0xd10,0x87001f1f,old,old&~BIT(31))&&
            update(p,CCU+0xd1c,0x10001,p->stop_requested?p->stop_bus:0x10001,0)&&masked(p)==1;
        if(!ok)return fail(p);
        p->running_module=old&~BIT(31);p->stage=RSTAGE_SHUTDOWN_POWER;
    } else return fail(p);
    if(!timely(p)||p->fault)return fail(p);
    return A733_RADIO_PENDING;
}
enum a733_radio_platform_result a733_radio_platform_poll(struct a733_radio_platform *p)
{
    if(!p||!p->initialized)return A733_RADIO_INVALID;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return A733_RADIO_BUSY;
    if(p->fault)return finish(p,fail(p));
    if(p->ops.current(p->cookie)!=1)return finish(p,A733_RADIO_INVALID);
    if(p->stage==RSTAGE_READY&&!p->stop_requested)return finish(p,p->host.state==SUNXI_SDIO_FAULT?fail(p):A733_RADIO_READY);
    if(p->stage==RSTAGE_DOWN)return finish(p,A733_RADIO_DOWN);
    if(!timely(p))return finish(p,fail(p));
    if(p->stop_requested&&p->stage<=RSTAGE_READY&&p->stage>RSTAGE_PINS&&
       p->stage!=RSTAGE_POWER_WAIT&&p->stage!=RSTAGE_VERIFY_RAILS){
        int health=supply_live(p);
        if(health<0)return finish(p,fail(p));
        if(!health)return finish(p,A733_RADIO_BUSY);
        return finish(p,stop_poll(p));
    }
    if(p->stage>RSTAGE_READY){
        int health=supply_live(p);
        if(health<0)return finish(p,fail(p));
        if(!health)return finish(p,A733_RADIO_BUSY);
        return finish(p,shutdown_poll(p));
    }
    if(p->stage>RSTAGE_RAILS) {
        int health=supply_live(p);
        if(health<0)return finish(p,fail(p));
        if(!health)return finish(p,A733_RADIO_BUSY);
    }
    enum a733_radio_platform_result result;
    if(p->stage==RSTAGE_ACQUIRE||p->stage==RSTAGE_RESERVE_RAILS||p->stage==RSTAGE_WAIT_RAILS||p->stage==RSTAGE_ADMIT||p->stage==RSTAGE_RAILS||p->stage==RSTAGE_PINS||
        p->stage==RSTAGE_POWER_ON||p->stage==RSTAGE_POWER_WAIT||p->stage==RSTAGE_VERIFY_RAILS||p->stage==RSTAGE_SUPPLY_WAIT||p->stage==RSTAGE_ENABLE_MODULE||p->stage==RSTAGE_MODULE_SETTLE)result=metadata(p);
    else {
        int live=clock_live(p);
        result=live<0?fail(p):!live?A733_RADIO_BUSY:p->stage>=RSTAGE_PREPARE?controller(p):registers(p);
    }
    return finish(p,result);
}
int a733_radio_platform_borrow(struct a733_radio_platform *p,uint64_t owner,
    struct a733_radio_borrow *out,struct sdio_card **card)
{
    if(!p||!p->initialized||!owner||!output_disjoint(p,out,sizeof(*out))||
        !output_disjoint(p,card,sizeof(*card))||!a733_ranges_disjoint(out,sizeof(*out),card,sizeof(*card)))return 0;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return 0;
    int ok=!p->fault&&!p->stop_requested&&p->ops.current(p->cookie)==1&&p->stage==RSTAGE_READY&&!p->borrower.generation&&
        p->host.state==SUNXI_SDIO_READY&&p->card.state==SDIO_CARD_READY;
    if(ok&&!observe(p)){fail(p);ok=0;}
    if(ok){p->borrower=(struct a733_radio_borrow){p->clock.id,owner};*out=p->borrower;*card=&p->card;}
    (void)finish(p,A733_RADIO_PENDING);return ok;
}
static enum a733_radio_platform_result live_proof(struct a733_radio_platform *p,
    const struct a733_radio_borrow *token,uint64_t deadline,unsigned stopping)
{
    if(!p||!p->initialized||!deadline||!a733_ranges_disjoint(p,sizeof(*p),token,sizeof(*token)))return A733_RADIO_INVALID;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return A733_RADIO_BUSY;
    if(p->fault)return finish(p,fail(p));
    if(p->stop_requested!=stopping||(stopping&&(p->stop_released||p->stop_phase>=3||deadline!=p->stop_deadline))||
        p->ops.current(p->cookie)!=1||p->stage!=RSTAGE_READY||!token->generation||
        token->generation!=p->borrower.generation||token->owner!=p->borrower.owner)return finish(p,A733_RADIO_INVALID);
    /* Fixed authority outlives individual completed-and-consumed requests.
     * Active or unconsumed work keeps its existing operation deadline. */
    if(!observe(p)||p->host.state==SUNXI_SDIO_FAULT||p->card.state!=SDIO_CARD_READY)return finish(p,fail(p));
    if(p->host.state!=SUNXI_SDIO_READY) {
        if(p->last_now>=p->deadline)return finish(p,fail(p));
        if(stopping&&p->host.deadline&&p->last_now>=p->host.deadline)return finish(p,fail(p));
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
enum a733_radio_platform_result a733_radio_platform_live(struct a733_radio_platform *p,
    const struct a733_radio_borrow *token,uint64_t deadline)
{return live_proof(p,token,deadline,0);}
enum a733_radio_platform_result a733_radio_platform_stop_held(struct a733_radio_platform *p,
    const struct a733_radio_borrow *token)
{return p?live_proof(p,token,p->stop_deadline,1):A733_RADIO_INVALID;}
enum a733_radio_platform_result a733_radio_platform_wake(struct a733_radio_platform *p,
    const struct a733_radio_borrow *token,uint64_t deadline,unsigned *level)
{
    if(!p||!p->initialized||!output_disjoint(p,level,sizeof(*level))||
        !a733_ranges_disjoint(token,sizeof(*token),level,sizeof(*level)))return A733_RADIO_INVALID;
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
        !a733_ranges_disjoint(token,sizeof(*token),level,sizeof(*level)))return A733_RADIO_INVALID;
    enum a733_radio_platform_result live=a733_radio_platform_live(p,token,deadline);
    if(live!=A733_RADIO_READY)return live;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))return A733_RADIO_BUSY;
    uint32_t value;
    enum a733_pins_result r=a733_pins_read_inputs_banked(p->pins,&p->pin_lease,A733_PIN_M,16,&value,deadline);
    if(!timely(p))return finish(p,fail(p));if(r==A733_PINS_BUSY)return finish(p,A733_RADIO_BUSY);
    if(r!=A733_PINS_OK)return finish(p,fail(p));*level=(value>>4)&1;return finish(p,A733_RADIO_READY);
}
