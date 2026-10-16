/* Focused cold-platform composition. Register behavior is modeled; electrical
 * PHY operation, physical DMA drain and actual connector power are not. */
#include "a733_usb_platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define C UINT32_C(0x02002000)
#define U UINT32_C(0x04100000)
struct word {uintptr_t a;uint32_t v;};
struct fixture {
    struct a733_usb_platform p;
    struct a733_clock_leases registry;
    struct a733_usb_platform_config config;
    struct word word[64];unsigned count,reads,writes,leaves,retains,authorized,locked;
    unsigned auth_result,fail_write,stick_write,expire_final,final_seen,tight_wait;
    uint64_t now,ref_enabled,device_enabled;
    struct a733_clock_token before;
};
static uint32_t *reg(struct fixture *f,uintptr_t a)
{
    for(unsigned i=0;i<f->count;++i)if(f->word[i].a==a)return &f->word[i].v;
    assert(f->count<64);f->word[f->count]=(struct word){a,0};return &f->word[f->count++].v;
}
static int now(void *c,uint64_t *v){*v=((struct fixture *)c)->now;return 1;}
static int authorize(void *c,const struct a733_usb_platform_config *cfg,const struct a733_clock_token *t)
{
    struct fixture *f=c;assert(cfg->handoff==12 && t->owner==11 && t->exclusive&A733_CLOCK_USB0);
    assert(a733_clock_token_status(&f->registry,t)==A733_CLOCK_OK);
    f->before=*t;f->authorized=f->auth_result==1;return (int)f->auth_result;
}
static int enter(void *c,uint64_t deadline)
{struct fixture *f=c;assert(f->authorized && !f->locked && f->now<deadline);f->locked=1;return 1;}
static void leave(void *c)
{
    struct fixture *f=c;assert(f->locked);f->locked=0;++f->leaves;
    if(f->expire_final && f->final_seen){f->now=f->config.deadline_us;atomic_store(&f->registry.busy,1);}
}
static void retain(void *c){struct fixture *f=c;assert(f->locked);++f->retains;}
static int readreg(void *c,uintptr_t a,uint32_t *v)
{
    struct fixture *f=c;assert(f->locked && f->authorized);++f->reads;
    if(a>=U && a<U+0x1000){
        assert((*reg(f,C+0x1304)&0x01000100)==0x01000100);
        assert(f->now-f->device_enabled>=40);
    }
    if(a==U+0x630 && (*reg(f,U+0x400)&0xf000)==0xf000)f->final_seen=1;
    *v=*reg(f,a);return 1;
}
static int writereg(void *c,uintptr_t a,uint32_t v)
{
    struct fixture *f=c;assert(f->locked && f->authorized);++f->writes;
    if(f->writes==f->fail_write)return -1;
    uint32_t old=*reg(f,a),mask=0;
    switch(a-C){
    case 0x1340:mask=0x87000000;assert(f->config.cold_reference);
        if((old^v)&0x07000000)assert(!((old|v)&0x80000000));
        if((v&0x81000000)==0x81000000){
            if(f->tight_wait)f->now=f->config.deadline_us-5;
            f->ref_enabled=f->now;
        }break;
    case 0x1304:mask=0x01000100;assert(f->now-f->ref_enabled>=10);
        if(v&0x100)f->device_enabled=f->now;break;
    case 0x1300:mask=0x40000000;assert((*reg(f,U+0x410)&0xa8)==0x20);break;
    default:
        if(a==U+0x410)mask=0xa8;
        else if(a==U+0x418)mask=0xf3f;
        else if(a==U+0x420)mask=1;
        else if(a==U+0x400){
            mask=0xf000;assert(!(v&0x70));
            /* Pending edge flags are W1C: zero must preserve them. */
            assert(((v^old)&~(mask|0x70))==0);
            *reg(f,a)=(v&~0x70)|(old&0x70);return 1;
        }else assert(!"Unexpected or unowned MMIO write");
    }
    assert(((v^old)&~mask)==0);
    if(f->writes!=f->stick_write)*reg(f,a)=v;
    return 1;
}
static const struct a733_usb_platform_ops ops={now,authorize,enter,leave,retain,readreg,writereg};
static void init(struct fixture *f,unsigned cold)
{
    memset(f,0,sizeof(*f));f->now=100;f->auth_result=1;
    f->config=(struct a733_usb_platform_config){.owner=11,.handoff=12,.deadline_us=100000,
        .fabric={0,0x80000000,0x80000000},.rate_hz={24000000,24000000,24000000},.cold_reference=cold};
    *reg(f,0x07090160)=0;*reg(f,0x070101ac)=1;
    *reg(f,C)=0xf8000000u|(99u<<8)|(99u<<16);
    *reg(f,C+0x580)=*reg(f,C+0x588)=0x80000000;
    *reg(f,C+0x5a4)=0x30001;*reg(f,C+0x5c0)=0x110103ff;*reg(f,C+0x5e0)=0x80000000;
    *reg(f,C+0x1340)=cold?0xa0000005:0xa1000005;
    *reg(f,C+0x1300)=0x10000000;*reg(f,C+0x1304)=0x20000000;
    *reg(f,C+0x1308)=0x10000000;*reg(f,C+0x130c)=0x20000000;
    *reg(f,U+0x410)=0x10000088;*reg(f,U+0x418)=0xface7040;
    *reg(f,U+0x420)=0x80000000;*reg(f,U+0x400)=0x80000070;
    assert(a733_clock_leases_init(&f->registry,now,f));
    assert(a733_usb_platform_init(&f->p,&f->config,&ops,f,&f->registry));
}
static enum a733_usb_platform_result run(struct fixture *f)
{
    for(unsigned i=0;i<300;++i){
        enum a733_usb_platform_result r=a733_usb_platform_poll(&f->p);
        if(r!=A733_USB_PLATFORM_PENDING)return r;
        f->now+=100;
    }
    assert(!"Unbounded initialization");return A733_USB_PLATFORM_INVALID;
}
static void terminal(struct fixture *f)
{
    unsigned r=f->reads,w=f->writes,l=f->leaves,t=f->retains;uint64_t last=f->p.last_now;
    assert(a733_usb_platform_poll(&f->p)==A733_USB_PLATFORM_RETAINED);
    assert(f->reads==r && f->writes==w && f->leaves==l && f->retains==t && f->p.last_now==last);
}
int main(void)
{
    struct fixture f;struct a733_clock_token peer;
    init(&f,1);assert(run(&f)==A733_USB_PLATFORM_PREPARED);
    assert(f.p.token.id!=f.before.id && f.p.token.owner==f.before.owner);
    assert(f.p.token.exclusive==A733_CLOCK_USB0 && f.p.token.shared&A733_CLOCK_USB01_REF);
    assert(a733_clock_token_status(&f.registry,&f.before)==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&f.registry,22,A733_CLOCK_FABRIC,A733_CLOCK_USB1,100000,&peer)==A733_CLOCK_OK);
    assert((*reg(&f,U+0x400)&0x70)==0x70 && *reg(&f,U+0x418)==0xface7846);
    assert(*reg(&f,C+0x5c0)==0x110103ff && *reg(&f,C+0x1308)==0x10000000);
    assert(f.now-f.device_enabled>=40 && !f.retains && !f.locked);
    unsigned writes=f.writes;assert(writes>5);
    struct a733_usb_platform copied;memcpy(&copied,&f.p,sizeof(copied));
    unsigned reads=f.reads;assert(a733_usb_platform_poll(&copied)==A733_USB_PLATFORM_INVALID);
    assert(f.reads==reads && f.writes==writes);
    f.now=100000;assert(a733_usb_platform_poll(&f.p)==A733_USB_PLATFORM_PREPARED);
    assert(a733_clock_retain(&f.registry,&f.p.token)==A733_CLOCK_FAULT);
    assert(a733_usb_platform_poll(&f.p)==A733_USB_PLATFORM_RETAINED);terminal(&f);
    init(&f,0);assert(run(&f)==A733_USB_PLATFORM_PREPARED && f.p.token.id==f.before.id);
    init(&f,1);memset(&f.p,0,sizeof(f.p));
    f.config.fabric[0]=0x03000002;f.config.fabric[1]=0x82000001;f.config.fabric[2]=0x81000002;
    f.config.rate_hz[0]=200000000;f.config.rate_hz[1]=400000000;f.config.rate_hz[2]=200000000;
    *reg(&f,C+0x500)=f.config.fabric[0];*reg(&f,C+0x580)=f.config.fabric[1];*reg(&f,C+0x588)=f.config.fabric[2];
    *reg(&f,C+0xa0)=0xfe000000u|(99u<<8)|(1u<<20)|(2u<<16)|(4u<<2);
    *reg(&f,C+0x1908)=0x0e000e00;*reg(&f,C+0x1988)=0x0e000000;
    assert(a733_usb_platform_init(&f.p,&f.config,&ops,&f,&f.registry));
    assert(run(&f)==A733_USB_PLATFORM_PREPARED && (f.p.token.shared&A733_CLOCK_PERI0));
    init(&f,1);f.auth_result=0;assert(a733_usb_platform_poll(&f.p)==A733_USB_PLATFORM_PENDING);
    assert(a733_usb_platform_poll(&f.p)==A733_USB_PLATFORM_PENDING && !f.reads && !f.writes);
    f.now=100000;assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);
    for(unsigned i=0;i<5;++i){
        init(&f,1);
        const uintptr_t address[]={C+0x1308,C+0x1304,C+0x5a4,C+0x5c0,C+0x5e0};
        const uint32_t value[]={0x40000000,0x01000100,1,0,0};*reg(&f,address[i])=value[i];
        assert(run(&f)==A733_USB_PLATFORM_RETAINED && !f.writes);terminal(&f);
    }
    init(&f,0);*reg(&f,C+0x1340)=0x80000000;
    assert(run(&f)==A733_USB_PLATFORM_RETAINED && !f.writes);
    for(unsigned i=0;i<16;++i){
        init(&f,1);*reg(&f,U+0x540+16*i)=0x80000000;
        assert(run(&f)==A733_USB_PLATFORM_RETAINED);
        assert(!(*reg(&f,C+0x1300)&0x40000000));terminal(&f);
    }
    for(unsigned i=0;i<5;++i){
        init(&f,1);const uintptr_t a[]={U+0x40,U+0x48,U+0x50,U+0x500,U+0x400};
        *reg(&f,a[i])=i?1:64;assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);
    }
    init(&f,1);f.fail_write=4;assert(run(&f)==A733_USB_PLATFORM_RETAINED && f.retains==1);terminal(&f);
    init(&f,1);f.stick_write=4;assert(run(&f)==A733_USB_PLATFORM_RETAINED && f.retains==1);
    init(&f,1);f.tight_wait=1;
    assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);
    init(&f,1);f.expire_final=1;assert(run(&f)==A733_USB_PLATFORM_BUSY);
    reads=f.reads;writes=f.writes;assert(a733_usb_platform_poll(&f.p)==A733_USB_PLATFORM_BUSY);
    assert(f.reads==reads && f.writes==writes && f.p.token.id==f.before.id);
    atomic_store(&f.registry.busy,0);assert(run(&f)==A733_USB_PLATFORM_RETAINED);terminal(&f);
    puts("PASS: cold/adopted reference, native ownership, source delays, detached core, all DMA refusals and retained failures");
}
