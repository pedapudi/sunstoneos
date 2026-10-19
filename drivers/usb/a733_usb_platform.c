#include "a733_usb_platform.h"
#include "a733_clock_reference.h"
#include <stddef.h>
#define BIT(n) (UINT32_C(1)<<(n))
#define CCU UINT32_C(0x02002000)
#define USB UINT32_C(0x04100000)
#define REF_MASK UINT32_C(0x87000000)
#define W1C UINT32_C(0x70)
enum { ACQUIRE, AUTHORIZE, BASELINE, REF_OFF, REF_SELECT, REF_ENABLE, WAIT_REF,
    DEVICE_RESET, DEVICE_GATE, WAIT_DEVICE, CORE_CHECK, PHY_CONTROL, PHY_RESET,
    ROUTE, TUNE, FORCE_DEVICE, VERIFY, PUBLISH, COMPLETE, FAULTED, ROM_CHECK, ROM_MASK, ROM_DISCONNECT, ROM_WAIT_DISCONNECT,
    ROM_RESET_ASSERT, ROM_WAIT_RESET, ROM_RESET_RELEASE, ROM_WAIT_RELEASE, ROM_CORE_CHECK };
static const uint32_t fabric_offsets[]={0x500,0x580,0x588};
static const uint32_t fabric_masks[]={0x0300001f,0x8700001f,0x8700001f};
static const unsigned parent_mhz[3][8]={
    {24,0,0,600,0,0,0,0},{24,0,800,600,480,0,0,0},{24,600,0,480,400,0,0,0}
};
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
int a733_usb_platform_init(struct a733_usb_platform *p,const struct a733_usb_platform_config *c,
    const struct a733_usb_platform_ops *o,void *cookie,struct a733_clock_leases *registry)
{
    if (!p || p->initialized || !c || !o || !registry || !c->owner || !c->handoff ||
        !c->deadline_us || c->cold_reference>A733_USB_ROM_DEVICE || !o->now_us || !o->authorize ||
        !o->enter || !o->leave || !o->retain || !o->read || !o->write) return 0;
    const void *objects[]={p,c,o,registry};
    const size_t sizes[]={sizeof(*p),sizeof(*c),sizeof(*o),sizeof(*registry)};
    for (unsigned i=0;i<4;++i) for (unsigned j=i+1;j<4;++j)
        if (!separate(objects[i],sizes[i],objects[j],sizes[j])) return 0;
    a733_clock_mask_t shared=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC;
    unsigned mhz[3];
    for (unsigned i=0;i<3;++i) {
        if ((c->fabric[i]&~fabric_masks[i]) || (i && !(c->fabric[i]&BIT(31)))) return 0;
        unsigned div=(c->fabric[i]&31)+1;
        mhz[i]=parent_mhz[i][(c->fabric[i]>>24)&7];
        if (!mhz[i] || (mhz[i]*1000000u)%div || c->rate_hz[i]!=(mhz[i]*1000000u)/div) return 0;
        if (mhz[i]!=24) shared|=A733_CLOCK_PERI0;
    }
    a733_clock_mask_t exclusive=A733_CLOCK_USB0;
    if (c->cold_reference==A733_USB_COLD_REFERENCE) exclusive|=A733_CLOCK_USB01_REF;
    else shared|=A733_CLOCK_USB01_REF;
    *p=(struct a733_usb_platform){.config=*c,.ops=*o,.registry=registry,.cookie=cookie,
        .shared=shared,.exclusive=exclusive,.initialized=1,.self=(uintptr_t)p};
    for (unsigned i=0;i<3;++i) p->parent_mhz[i]=mhz[i];
    atomic_init(&p->busy,0);return 1;
}
static void record(struct a733_usb_platform *p,unsigned kind,uintptr_t address,
    uint32_t mask,uint32_t expected,uint32_t observed,unsigned valid,int result)
{
    if(p->failure.kind)return;
    p->failure=(struct a733_usb_failure){.kind=kind,.stage=p->stage,.step=p->step,
        .wrote=p->wrote,.address=address,.mask=mask,.expected=expected,
        .observed=observed,.observed_valid=valid,.callback_result=result,
        .writes_attempted=p->writes_attempted,.writes_completed=p->writes_completed,
        .last_us=p->last_now,.sample_us=p->last_sample_us,.deadline_us=p->config.deadline_us};
}
static int timely(struct a733_usb_platform *p)
{
    uint64_t now=0;int result=p->ops.now_us(p->cookie,&now);p->last_sample_us=now;
    if (result!=1 || now<p->last_now || now>=p->config.deadline_us) {
        record(p,A733_USB_FAILURE_TIME,p->last_address,0,0,p->last_observed,
            p->last_observed_valid,result);return 0;
    }
    p->last_now=now;return 1;
}
static int rd(struct a733_usb_platform *p,uintptr_t a,uint32_t *v)
{
    p->last_address=a;p->last_observed_valid=0;
    if(!timely(p))return 0;
    int result=p->ops.read(p->cookie,a,v);p->last_callback_result=result;
    if(result==A733_USB_PLATFORM_IO_BUSY){p->io_busy=1;return 0;}
    if(result!=1){record(p,A733_USB_FAILURE_READ,a,0,0,0,0,result);return 0;}
    p->last_observed=*v;p->last_observed_valid=1;return timely(p);
}
static int value_bits(struct a733_usb_platform *p,uintptr_t a,uint32_t v,uint32_t mask,uint32_t value)
{
    if((v&mask)==value)return 1;
    record(p,A733_USB_FAILURE_VALUE,a,mask,value,v,1,1);return 0;
}
static int bits(struct a733_usb_platform *p,uintptr_t a,uint32_t mask,uint32_t value)
{uint32_t v;return rd(p,a,&v)&&value_bits(p,a,v,mask,value);}
static int update(struct a733_usb_platform *p,uintptr_t a,uint32_t mask,uint32_t value,uint32_t w1c)
{
    uint32_t old;
    if (!rd(p,a,&old)) return 0;
    uint32_t next=((old&~mask)|value)&~w1c;
    if (next==old) return 1;
    if(!timely(p))return 0;
    p->wrote=1;++p->writes_attempted;
    int result=p->ops.write(p->cookie,a,next);p->last_callback_result=result;
    if(result==A733_USB_PLATFORM_IO_BUSY){p->io_busy=1;return 0;}
    if(result!=1){record(p,A733_USB_FAILURE_WRITE,a,mask,next,old,1,result);return 0;}
    ++p->poll_writes;++p->writes_completed;
    return timely(p)&&bits(p,a,mask,value);
}
static int wait_start(struct a733_usb_platform *p,uint64_t delay)
{
    if(!timely(p))return 0;
    if(delay>=p->config.deadline_us-p->last_now){
        record(p,A733_USB_FAILURE_WAIT,0,0,(uint32_t)delay,0,0,0);return 0;}
    p->wait_until=p->last_now+delay;return 1;
}
static int peri_rate(struct a733_usb_platform *p,uint32_t v,unsigned mhz)
{
    if (mhz==24) return 1;
    unsigned div,mask,n=((v>>8)&255)+1;
    switch (mhz) {
    case 600:div=(((v>>20)&7)+1)*2;mask=BIT(11)|BIT(9);break;
    case 400:div=(((v>>20)&7)+1)*3;mask=BIT(11)|BIT(2)|BIT(1);break;
    case 800:div=((v>>16)&7)+1;mask=BIT(10);break;
    case 480:div=((v>>2)&7)+1;mask=BIT(8)|BIT(7);break;
    default:return 0;
    }
    if(24u*n!=mhz*div){record(p,A733_USB_FAILURE_PERI_RATE,CCU+0xa0,0,mhz,v,1,1);return 0;}
    return bits(p,CCU+0x1908,mask|(mask<<16),mask|(mask<<16)) &&
        bits(p,CCU+0x1988,mask<<16,mask<<16);
}
static int stable(struct a733_usb_platform *p)
{
    uint32_t v,osc;
    if (!rd(p,UINT32_C(0x07090160),&osc) ||
        !bits(p,UINT32_C(0x070101ac),1,1)) return 0;
    /* Manual11.3.3 defines SYS_CLK24M as fixed24MHz from the DCXO/PLL_REF
     * mux. Decode PLL_REF from the actual DCXO rate; DCXO need not be24MHz. */
    if(!rd(p,CCU,&v))return 0;
    if(!a733_clock_ref24_valid(osc,v)){record(p,A733_USB_FAILURE_REFERENCE,CCU,0,24000000,v,1,1);return 0;}
    if (p->shared&A733_CLOCK_PERI0) {
        if(!rd(p,CCU+0xa0,&v)||!value_bits(p,CCU+0xa0,v,0xfe000002u,0xfe000000u))return 0;
        if((((v>>8)&255)+1)<53 || (((v>>8)&255)+1)>105){
            record(p,A733_USB_FAILURE_PERI_PROFILE,CCU+0xa0,0,0,v,1,1);return 0;}
        if(!bits(p,CCU+0xa8,BIT(31),0)||!bits(p,CCU+0xac,0x8e000000u,0))return 0;
        for (unsigned i=0;i<3;++i) if (!peri_rate(p,v,p->parent_mhz[i])) return 0;
    }
    for (unsigned i=0;i<3;++i)
        if (!bits(p,CCU+fabric_offsets[i],fabric_masks[i],p->config.fabric[i])) return 0;
    /* USB0/1 use MSI_LITE2, not the similarly named MSI_LITE0 gate. */
    return bits(p,CCU+0x5a4,0x30001,0x30001) && bits(p,CCU+0x5c0,BIT(9),BIT(9)) &&
        bits(p,CCU+0x5e0,BIT(31),BIT(31));
}
static int reference_valid(struct a733_usb_platform *p)
{
    uint32_t osc;
    if(!bits(p,CCU+0x1340,REF_MASK,p->reference))return 0;
    if(p->reference==BIT(31))return 1;
    if(!rd(p,UINT32_C(0x07090160),&osc))return 0;
    if(a733_clock_dcxo_hz(osc)==24000000)return 1;
    record(p,A733_USB_FAILURE_DIRECT_OSC,UINT32_C(0x07090160),0,24000000,osc,1,1);return 0;
}
static int reference_selected(struct a733_usb_platform *p,uint32_t v)
{
    p->reference=v&REF_MASK;
    if(p->reference!=BIT(31)&&p->reference!=(BIT(31)|BIT(24))){
        record(p,A733_USB_FAILURE_REFERENCE_SELECT,CCU+0x1340,REF_MASK&~BIT(24),BIT(31),v,1,1);return 0;}
    return reference_valid(p);
}
static int cold(struct a733_usb_platform *p)
{
    uint32_t osc,v;
    if (!bits(p,CCU+0x1300,0xc0000000u,0) || !bits(p,CCU+0x1304,0x01110111,0)) return 0;
    if (p->config.cold_reference==A733_USB_COLD_REFERENCE) {
        if (!bits(p,CCU+0x1308,0xc0000000u,0) || !bits(p,CCU+0x130c,0x00110011,0) ||
            !rd(p,UINT32_C(0x07090160),&osc)) return 0;
        /* Manual4.1.6.198: selector0 SYS_CLK24M; selector1 raw DCXO. */
        p->reference=BIT(31)|(a733_clock_dcxo_hz(osc)==24000000?BIT(24):0);
        return 1;
    }
    if (!rd(p,CCU+0x1340,&v)) return 0;
    return reference_selected(p,v);
}
/* A device-only, PIO ROM handoff. These observations reject a running DMA
 * engine; they do not establish completion of previously issued bus writes. */
static int rom_baseline(struct a733_usb_platform *p)
{
    uint32_t v;
    if (!bits(p,CCU+0x1300,0xc0000000u,BIT(30)) ||
        !bits(p,CCU+0x1304,0x01110111,0x01000100) ||
        !rd(p,CCU+0x1340,&v)) return 0;
    return reference_selected(p,v);
}
static int rom_pio(struct a733_usb_platform *p)
{
    /* Packed PCTL/DEVCTL: require B-device, no HOSTREQ/HOSTMODE.
     * DEVCTL.SESSION is valid device-session state, not DMA activity. The
     * matched ROM tests BDEVICE/VBUS without requiring SESSION clear.
     * VEND0 bus selection is allowed for PIO (pinned USBC_SelectBus). */
    if (!bits(p,USB+0x40,BIT(9)|BIT(10)|BIT(15),BIT(15))) return 0;
    for (unsigned i=0;i<16;++i) if (!bits(p,USB+0x540+i*16,BIT(31),0)) return 0;
    return 1;
}
static int core_detached(struct a733_usb_platform *p)
{
    if (!bits(p,USB+0x40,BIT(6)|BIT(9)|BIT(10),0) ||
        !bits(p,USB+0x48,UINT32_MAX,0) || !bits(p,USB+0x50,255,0) ||
        !bits(p,USB+0x500,65535,0) || !bits(p,USB+0x400,15,0)) return 0;
    for (unsigned i=0;i<16;++i) if (!bits(p,USB+0x540+i*16,BIT(31),0)) return 0;
    return 1;
}
static enum a733_usb_platform_result finish(struct a733_usb_platform *p,enum a733_usb_platform_result r)
{atomic_store_explicit(&p->busy,0,memory_order_release);return r;}
static enum a733_usb_platform_result failed(struct a733_usb_platform *p)
{
    if(!p->failure.kind)record(p,A733_USB_FAILURE_STAGE,p->last_address,0,0,
        p->last_observed,p->last_observed_valid,p->last_callback_result);
    p->fault=1;
    if (p->held) {p->ops.retain(p->cookie);p->held=0;}
    if (p->token.id && a733_clock_retain(p->registry,&p->token)==A733_CLOCK_BUSY)
        return finish(p,A733_USB_PLATFORM_BUSY);
    p->stage=FAULTED;return finish(p,A733_USB_PLATFORM_RETAINED);
}
enum a733_usb_platform_result a733_usb_platform_poll(struct a733_usb_platform *p)
{
    if (!p || !p->initialized || p->self!=(uintptr_t)p) return A733_USB_PLATFORM_INVALID;
    unsigned expected=0;
    if (!atomic_compare_exchange_strong_explicit(&p->busy,&expected,1,memory_order_acquire,memory_order_relaxed))
        return A733_USB_PLATFORM_BUSY;
    if (p->stage==FAULTED) return finish(p,A733_USB_PLATFORM_RETAINED);
    if (p->fault) return failed(p);
    if (p->stage==COMPLETE) {
        enum a733_clock_result r=a733_clock_token_status(p->registry,&p->token);
        if (r==A733_CLOCK_BUSY) return finish(p,A733_USB_PLATFORM_BUSY);
        if(r!=A733_CLOCK_OK){record(p,A733_USB_FAILURE_CLOCK_TOKEN,0,0,0,0,0,r);return failed(p);}
        return finish(p,A733_USB_PLATFORM_PREPARED);
    }
    p->io_busy=0;p->poll_writes=0;
    if (!timely(p)) return failed(p);
    if (p->stage==ACQUIRE) {
        enum a733_clock_result r=a733_clock_acquire(p->registry,p->config.owner,p->shared,p->exclusive,
            p->config.deadline_us,&p->token);
        if (r==A733_CLOCK_BUSY) return finish(p,A733_USB_PLATFORM_BUSY);
        if(r!=A733_CLOCK_OK){record(p,A733_USB_FAILURE_CLOCK_ACQUIRE,0,0,0,0,0,r);return failed(p);}
        if(!timely(p))return failed(p);
        p->stage=AUTHORIZE;return finish(p,A733_USB_PLATFORM_PENDING);
    }
    enum a733_clock_result cr=a733_clock_token_status(p->registry,&p->token);
    if (cr==A733_CLOCK_BUSY) return finish(p,A733_USB_PLATFORM_BUSY);
    if(cr!=A733_CLOCK_OK){record(p,A733_USB_FAILURE_CLOCK_TOKEN,0,0,0,0,0,cr);return failed(p);}
    if (p->stage==AUTHORIZE) {
        int r=p->ops.authorize(p->cookie,&p->config,&p->token);
        if(r!=0&&r!=1){record(p,A733_USB_FAILURE_AUTHORITY,0,0,0,0,0,r);return failed(p);}
        if(!timely(p))return failed(p);
        if (r) p->stage=BASELINE;
        return finish(p,A733_USB_PLATFORM_PENDING);
    }
    if (p->stage==PUBLISH) {
        if (p->config.cold_reference==A733_USB_COLD_REFERENCE) {
            struct a733_clock_token replacement;
            cr=a733_clock_publish_stable(p->registry,&p->token,A733_CLOCK_USB01_REF,
                p->config.deadline_us,&replacement);
            if (cr==A733_CLOCK_BUSY) return finish(p,A733_USB_PLATFORM_BUSY);
            if(cr!=A733_CLOCK_OK){record(p,A733_USB_FAILURE_CLOCK_TOKEN,0,0,0,0,0,cr);return failed(p);}
            p->token=replacement; /* Adopt the sole live token before callbacks. */
        }
        if (!timely(p)) return failed(p);
        p->stage=COMPLETE;return finish(p,A733_USB_PLATFORM_PREPARED);
    }
    if (p->stage==WAIT_REF || p->stage==WAIT_DEVICE ||
        p->stage==ROM_WAIT_DISCONNECT || p->stage==ROM_WAIT_RESET || p->stage==ROM_WAIT_RELEASE) {
        if (p->last_now<p->wait_until) return finish(p,A733_USB_PLATFORM_PENDING);
        ++p->stage;return finish(p,A733_USB_PLATFORM_PENDING);
    }
    int entered=p->ops.enter(p->cookie,p->config.deadline_us);
    if (entered!=0) p->held=1;
    if (entered!=1) {
        if(entered!=0){record(p,A733_USB_FAILURE_LOCK,0,0,0,0,0,entered);return failed(p);}
        if(!timely(p))return failed(p);
        return finish(p,A733_USB_PLATFORM_BUSY);
    }
    if (!timely(p)) return failed(p);
    int ok=0;
    switch (p->stage) {
    case BASELINE:ok=stable(p)&&(p->config.cold_reference==A733_USB_ROM_DEVICE?rom_baseline(p):cold(p));break;
    case ROM_CHECK:ok=rom_baseline(p)&&rom_pio(p);break;
    case ROM_MASK:ok=rom_pio(p)&&update(p,USB+0x48,UINT32_MAX,0,0)&&
        update(p,USB+0x50,255,0,0)&&update(p,USB+0x500,65535,0,0)&&
        update(p,USB+0x400,15,0,W1C);break;
    /* Same disconnect/reset intervals as the physically exercised PIO path in
     * a733_usb_prepare(). The disconnect interval is not a bus-drain proof. */
    case ROM_DISCONNECT:ok=rom_pio(p)&&update(p,USB+0x40,BIT(6),0,0)&&wait_start(p,150000);break;
    case ROM_RESET_ASSERT:ok=rom_pio(p)&&core_detached(p)&&
        update(p,CCU+0x1304,BIT(24),0,0)&&wait_start(p,1000);break;
    case ROM_RESET_RELEASE:ok=bits(p,CCU+0x1304,0x01110111,0x100)&&
        update(p,CCU+0x1304,BIT(24),BIT(24),0)&&wait_start(p,40);break;
    case ROM_CORE_CHECK:ok=core_detached(p);break;
    case REF_OFF:ok=update(p,CCU+0x1340,BIT(31),0,0);break;
    case REF_SELECT:ok=bits(p,CCU+0x1340,BIT(31),0)&&update(p,CCU+0x1340,0x07000000,p->reference&0x07000000,0);break;
    case REF_ENABLE:ok=update(p,CCU+0x1340,BIT(31),BIT(31),0)&&wait_start(p,10);break;
    case DEVICE_RESET:ok=update(p,CCU+0x1304,BIT(24),BIT(24),0);break;
    case DEVICE_GATE:ok=update(p,CCU+0x1304,BIT(8),BIT(8),0)&&wait_start(p,40);break;
    case CORE_CHECK:ok=core_detached(p);break;
    case PHY_CONTROL:ok=update(p,USB+0x410,BIT(7)|BIT(5)|BIT(3),BIT(5),0);break;
    case PHY_RESET:ok=update(p,CCU+0x1300,BIT(30),BIT(30),0);break;
    case ROUTE:ok=update(p,USB+0x420,1,1,0);break;
    /* Board phy_range 0x188 maps to TXVREF=8, RISE=0, PREEMP=2, RES=1. */
    case TUNE:ok=update(p,USB+0x418,0xf3f,0x806,0);break;
    case FORCE_DEVICE:ok=update(p,USB+0x400,0xf000,0xf000,W1C);break;
    case VERIFY:ok=stable(p)&&core_detached(p)&&reference_valid(p)&&
        bits(p,CCU+0x1300,0xc0000000u,BIT(30))&&bits(p,CCU+0x1304,0x01110111,0x01000100)&&
        bits(p,USB+0x410,0xa8,0x20)&&bits(p,USB+0x420,1,1)&&bits(p,USB+0x418,0xf3f,0x806)&&
        bits(p,USB+0x400,0xf000,0xf000);break;
    default:break;
    }
    if(p->io_busy&&!p->failure.kind){
        if(!p->poll_writes){
            /* All whitelist reads are non-acknowledging. No successful write
             * occurred in this poll, so retrying this unchanged stage cannot
             * replay a device-reset assertion or a partial write sequence. */
            p->ops.leave(p->cookie);p->held=0;
            if(!timely(p))return failed(p);
            return finish(p,A733_USB_PLATFORM_BUSY);
        }
        record(p,A733_USB_FAILURE_BUSY_AFTER_WRITE,p->last_address,0,0,
            p->last_observed,p->last_observed_valid,p->last_callback_result);
    }
    if (!ok || !timely(p)) return failed(p);
    p->ops.leave(p->cookie);p->held=0;
    if (!timely(p)) return failed(p);
    if (p->stage==BASELINE && p->config.cold_reference==A733_USB_ROM_DEVICE) p->stage=ROM_CHECK;
    else if (p->stage==ROM_CORE_CHECK) p->stage=PHY_CONTROL;
    else if (p->stage==BASELINE && !p->config.cold_reference) p->stage=DEVICE_RESET;
    else ++p->stage;
    return finish(p,A733_USB_PLATFORM_PENDING);
}
