/* Actual PMIC, rail, pin, clock and MMC core; only MMIO/card/time are modeled. */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
#include "a733_mmc_platform_native.h"
#include "mmc-card-model.h"
struct storage_model {
    struct pmic_model power;
    struct a733_mmc_platform platform;
    struct model card;
    unsigned accesses,writes,admitted,register_retained,fail_write,late_take,late_calls;
    uint64_t module_at;
};
static struct storage_model m;
static int storage_now(void *v,uint64_t *out)
{
    struct storage_model *s=v;
    if(s->late_take&&!s->platform.core.borrowed){++s->late_calls;*out=10000001;return 1;}
    *out=s->power.now;return 1;
}
static int storage_current(void *v){return !((struct storage_model *)v)->power.worker;}
static int storage_admit(void *v,const struct a733_mmc_platform_config *c,const struct a733_clock_token *t)
{
    struct storage_model *s=v;assert(c->handoff==200&&c->topology==201&&t->owner==42);
    assert(a733_clock_token_status(&s->power.clocks,t)==A733_CLOCK_OK);++s->admitted;return 1;
}
static int storage_enter(void *v,uint64_t d)
{struct storage_model *s=v;return s->register_retained?0:pmic_model_pin_enter(&s->power,d);}
static void storage_leave(void *v){pmic_model_pin_leave(&((struct storage_model *)v)->power);}
static void storage_retain(void *v)
{struct storage_model *s=v;assert(s->power.register_held);s->register_retained=1;}
static int storage_outside(void *v,const void *p,size_t n)
{
    struct storage_model *s=v;
    return p&&pmic_model_separate(p,n,&s->card,sizeof(s->card))&&
        pmic_model_separate(p,n,&s->accesses,sizeof(*s)-(size_t)((char *)&s->accesses-(char *)s));
}
static void accessible(struct storage_model *s)
{
    assert(s->admitted&&s->power.register_held&&s->platform.pin.id&&!s->power.pins.failed);
    assert(a733_rail_status(&s->power.rails,&s->platform.rail)==A733_RAIL_READY);
    assert(s->module_at&&s->power.now-s->module_at>=10);
    assert((*pmic_model_word(&s->power,s->platform.module)&0x87001f1f)==0x8000001d);
    assert((*pmic_model_word(&s->power,s->platform.bus)&0x10001)==0x10001);
}
static int storage_read(void *v,uintptr_t a,uint32_t *out)
{
    struct storage_model *s=v;++s->accesses;
    assert(s->power.register_held&&a733_mmc_platform_native_address_selected(s->platform.config.kind,s->platform.config.emmc_host,a,0));
    if(a>=s->platform.base&&a<=s->platform.base+0x300) {
        accessible(s);return read32(&s->card,(uint32_t)(a-s->platform.base),out);
    }
    *out=*pmic_model_word(&s->power,a);return 1;
}
static int storage_write(void *v,uintptr_t a,uint32_t value)
{
    struct storage_model *s=v;++s->accesses;++s->writes;
    assert(s->power.register_held&&s->admitted&&a733_mmc_platform_native_address_selected(s->platform.config.kind,s->platform.config.emmc_host,a,1));
    if(a>=s->platform.base&&a<=s->platform.base+0x300) {
        accessible(s);return write32(&s->card,(uint32_t)(a-s->platform.base),value);
    }
    if(a==0x03400198) {
        assert(value==(s->platform.config.kind==SUNXI_MMC_SD?2u:s->platform.config.emmc_host==A733_MMC_EMMC_SDC3?16u:8u));
        *pmic_model_word(&s->power,0x03400118)&=~value;
    } else {
        assert(a==s->platform.module||a==s->platform.bus);
        if(a==s->platform.module&&(value&(1u<<31)))s->module_at=s->power.now;
        if(a==s->platform.bus&&(value&(1u<<16)))assert(s->module_at&&s->power.now-s->module_at>=10);
        *pmic_model_word(&s->power,a)=value;
        if(s->fail_write)return 0;
    }
    return 1;
}
/* Bursts repeat the installed per-word operation, so fixtures that replace
 * read or write also observe every FIFO word. */
static inline int storage_read_fifo(void *v,uintptr_t a,uint32_t *w,unsigned n)
{
    struct storage_model *s=v;assert(n&&n<=SUNXI_MMC_FIFO_WORDS&&a==s->platform.base+0x200);
    for(unsigned i=0;i<n;++i)if(s->platform.ops.read(v,a,&w[i])!=1)return 0;
    return 1;
}
static inline int storage_write_fifo(void *v,uintptr_t a,const uint32_t *w,unsigned n)
{
    struct storage_model *s=v;assert(n&&n<=SUNXI_MMC_FIFO_WORDS&&a==s->platform.base+0x200);
    for(unsigned i=0;i<n;++i)if(s->platform.ops.write(v,a,w[i])!=1)return 0;
    return 1;
}
static int storage_barrier(void *v)
{struct storage_model *s=v;assert(s->power.register_held);return 1;}
static void storage_setup_selected(enum sunxi_mmc_kind kind,enum a733_mmc_emmc_host host)
{
    memset(&m,0,sizeof(m));pmic_model_setup(&m.power);seed(&m.card,&m.platform.core,kind,1);
    m.power.pmic[0x11]=1u<<4;m.power.pmic[0x33]=13;
    *pmic_model_word(&m.power,0x02002000)=0xf8000000u|(99u<<16)|(99u<<8);
    *pmic_model_word(&m.power,0x02002580)=1u<<31;
    *pmic_model_word(&m.power,0x02002588)=1u<<31;
    *pmic_model_word(&m.power,0x020025c0)=1u<<24;
    *pmic_model_word(&m.power,0x020025e0)=1u<<30;
    *pmic_model_word(&m.power,0x0200259c)=0x30001;
    *pmic_model_word(&m.power,0x02000040)=(2u<<10)|(3u<<4);
    *pmic_model_word(&m.power,0x02000048)=2u<<10;
    *pmic_model_word(&m.power,0x02000070)=1;
    *pmic_model_word(&m.power,0x02000190)=2; /* Externally held eMMC reset. */
    struct a733_mmc_platform_config c={.owner=42,.handoff=200,.topology=201,
        .kind=kind,.emmc_host=host,.deadline_us=1000000,.lifetime_deadline_us=10000000,.voltage_mask=0x00300000,
        .fabric={0,1u<<31,1u<<31},.fabric_hz={24000000,24000000,24000000}};
    const unsigned pins[]={0,1,5,6,8,9,10,11,13,14,15,16};
    for(unsigned i=0;i<(kind==SUNXI_MMC_SD?7u:12u);++i) {
        unsigned pin=kind==SUNXI_MMC_SD?i:pins[i];
        c.desired[i].function=kind==SUNXI_MMC_SD?(pin>=1&&pin<=3?2:0):((pin==5||pin==6||pin==10)?3+(unsigned)host:0);
        c.desired[i].pull=1;
    }
    const struct a733_mmc_platform_ops o={storage_now,storage_current,storage_admit,storage_enter,
        storage_leave,storage_retain,storage_read,storage_write,storage_barrier,storage_outside,
        storage_read_fifo,storage_write_fifo,NULL};
    assert(a733_mmc_platform_init(&m.platform,&c,&o,&m,&m.power.clocks,&m.power.pins,&m.power.rails,&m.power.provider));
    assert(!m.accesses);
}
static void storage_setup(enum sunxi_mmc_kind kind)
{storage_setup_selected(kind,A733_MMC_EMMC_SDC2);}
static enum a733_mmc_platform_result step(void)
{
    m.power.now+=100;m.card.fifo_this_poll=0;
    assert(pmic_model_step(&m.power)!=A733_PMIC_RETAINED);
    return a733_mmc_platform_poll(&m.platform);
}
static void ready_storage(void)
{
    enum a733_mmc_platform_result r=A733_MMC_PENDING;
    for(unsigned i=0;i<2000&&r!=A733_MMC_READY;++i){r=step();assert(r!=A733_MMC_RETAINED);}
    assert(r==A733_MMC_READY&&m.platform.rail_index==3&&!m.platform.request.id);
    assert(m.platform.snapshots[0].programmed_uv==3300000&&m.platform.snapshots[1].programmed_uv==3300000&&
           m.platform.snapshots[2].programmed_uv==1800000);
}
static void retained(void)
{
    unsigned a=m.accesses;
    assert(a733_mmc_platform_poll(&m.platform)==A733_MMC_RETAINED&&a==m.accesses);
    assert(a733_clock_token_status(&m.power.clocks,&m.platform.clock)==A733_CLOCK_FAULT);
    if(m.platform.rail.id)assert(a733_rail_status(&m.power.rails,&m.platform.rail)==A733_RAIL_RETAINED);
}
int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"--alias")) {
        storage_setup(SUNXI_MMC_SD);
        /* Place the alias, at its own alignment, in the unused trace tail. */
        const uintptr_t align=_Alignof(struct a733_mmc_platform);
        struct a733_mmc_platform *alias=(void *)(((uintptr_t)&m.power.trace[512]+align-1)&~(align-1));
        _Static_assert(sizeof(*alias)+_Alignof(struct a733_mmc_platform)<=sizeof(m.power.trace)-512*sizeof(m.power.trace[0]),"alias storage fits");
        assert(m.power.trace_count<=512);
        assert(!alias->initialized);
        assert(storage_outside(&m,alias,sizeof(*alias))==1);
        assert(a733_pmic_outside(&m.power.provider,alias,sizeof(*alias))==0);
        assert(!a733_mmc_platform_init(alias,&m.platform.config,&m.platform.ops,&m,
            &m.power.clocks,&m.power.pins,&m.power.rails,&m.power.provider));
        assert(!alias->initialized&&!m.accesses);
        assert(a733_mmc_platform_read(&m.platform,0,1,(void *)&m.power.words[64],512,1000000)==A733_MMC_INVALID);
        assert(!m.accesses&&!m.platform.core.borrowed);
        puts("PASS: actual PMIC private-backing guard refuses aliased platform and read storage");
        return 0;
    }
    assert(argc==1);
    uint8_t buffer[512];struct sunxi_mmc_result result;struct sunxi_mmc_card card;
    for(unsigned k=SUNXI_MMC_SD;k<=SUNXI_MMC_EMMC;++k) {
        storage_setup((enum sunxi_mmc_kind)k);ready_storage();
        assert(a733_mmc_platform_card(&m.platform,&card,1000000)==A733_MMC_READY&&card.sectors);
        unsigned before=m.accesses;
        assert(a733_mmc_platform_read(&m.platform,card.sectors,1,buffer,sizeof(buffer),1000000)==A733_MMC_INVALID);
        assert(before==m.accesses&&!m.platform.core.borrowed);
        assert(a733_mmc_platform_read(&m.platform,0,1,buffer,sizeof(buffer),1000000)==A733_MMC_PENDING);
        enum a733_mmc_platform_result r=A733_MMC_PENDING;
        for(unsigned i=0;i<200&&r!=A733_MMC_COMPLETE;++i){r=step();assert(r!=A733_MMC_RETAINED);}
        assert(r==A733_MMC_COMPLETE&&m.platform.core.borrowed==buffer);
        m.late_take=1;
        assert(a733_mmc_platform_take(&m.platform,&result,1000000)==A733_MMC_READY);
        assert(!m.late_calls&&!m.platform.core.borrowed&&result.received==512);
        m.late_take=0;
        for(unsigned i=0;i<512;++i)assert(buffer[i]==(uint8_t)(i*37u+11u));
        unsigned a=m.accesses;m.power.now=2000000;
        assert(a733_mmc_platform_poll(&m.platform)==A733_MMC_READY&&a==m.accesses);
    }
    storage_setup(SUNXI_MMC_SD);
    struct a733_clock_token other={0};
    assert(a733_clock_acquire(&m.power.clocks,55,0,A733_CLOCK_SDC0,1000000,&other)==A733_CLOCK_OK);
    assert(a733_mmc_platform_poll(&m.platform)==A733_MMC_BUSY&&!m.accesses&&!m.platform.clock.id);
    assert(a733_clock_release(&m.power.clocks,&other,1000000)==A733_CLOCK_OK);
    ready_storage();
    assert(a733_mmc_platform_read(&m.platform,0,1,buffer,sizeof(buffer),1000000)==A733_MMC_PENDING);
    *pmic_model_word(&m.power,0x02000310)=1u<<6;
    unsigned a=m.accesses;
    assert(a733_mmc_platform_poll(&m.platform)==A733_MMC_RETAINED&&a==m.accesses&&m.platform.core.borrowed==buffer);
    retained();
    storage_setup(SUNXI_MMC_EMMC);ready_storage();m.power.pins.failed=1;
    assert(a733_mmc_platform_read(&m.platform,0,1,buffer,sizeof(buffer),1000000)==A733_MMC_RETAINED);
    assert(m.platform.pin.id&&m.platform.clock.id);retained();
    storage_setup(SUNXI_MMC_SD);m.power.pmic[0x11]=0;
    enum a733_mmc_platform_result r=A733_MMC_PENDING;
    for(unsigned i=0;i<1000&&r!=A733_MMC_RETAINED;++i)r=step();
    assert(r==A733_MMC_RETAINED&&!m.accesses&&!m.platform.pin.id);retained();
    storage_setup(SUNXI_MMC_SD);*pmic_model_word(&m.power,0x02000048)=3u<<10;
    r=A733_MMC_PENDING;for(unsigned i=0;i<1000&&r!=A733_MMC_RETAINED;++i)r=step();
    assert(r==A733_MMC_RETAINED&&!m.writes&&!m.platform.pin.id);retained();
    storage_setup(SUNXI_MMC_EMMC);*pmic_model_word(&m.power,0x02000190)=0;
    r=A733_MMC_PENDING;for(unsigned i=0;i<1000&&r!=A733_MMC_RETAINED;++i)r=step();
    assert(r==A733_MMC_RETAINED&&m.platform.pin.id&&!m.module_at&&!m.card.calls);retained();
    storage_setup(SUNXI_MMC_EMMC);m.fail_write=1;
    r=A733_MMC_PENDING;for(unsigned i=0;i<1000&&r!=A733_MMC_RETAINED;++i)r=step();
    assert(r==A733_MMC_RETAINED&&m.register_retained&&m.platform.pin.id);retained();
    for(unsigned k=SUNXI_MMC_SD;k<=SUNXI_MMC_EMMC;++k) {
        enum sunxi_mmc_kind kind=(enum sunxi_mmc_kind)k;
        assert(!a733_mmc_platform_native_address(kind,0x04021000,0));
        assert(!a733_mmc_platform_native_address(kind,0x04023000,1));
        assert(!a733_mmc_platform_native_address(kind,0x020025c0,1));
        assert(!a733_mmc_platform_native_address(kind,0x02000070,1));
        assert(!a733_mmc_platform_native_address(kind,(k==SUNXI_MMC_SD?0x04020000:0x04022000)+0x200,1));
    }
    puts("PASS: real MMC/clock/pin/rail/PMIC composition, selected routes, read release and retained faults");
    return 0;
}
