/* pci-epf-test wire layout and COPY semantics follow the pinned GPL-2.0 Linux
 * endpoint function and host test driver in sources.lock.json. */
#include "a733_pcie_epf_test.h"
#include <string.h>
#define CFG UINT32_C(0x20000000)
#define DBI UINT32_C(0x06000000)
#define ATU A733_PCIE_ATU_OUTBOUND(1)
#define MEM A733_PCIE_TEST_WINDOW
#define B(n) (UINT32_C(1)<<(n))
enum { CLAIM,BAR,WINDOW,MSI,PREPARE,ARM,SUBMIT,WAIT,DONE };
static int separate(const void *a,size_t n,const void *b,size_t m)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a&&b&&n<=UINTPTR_MAX-x&&m<=UINTPTR_MAX-y&&(x+n<=y||y+m<=x);}
static int rd(struct a733_pcie_epf_test *t,uintptr_t a,unsigned w,uint32_t *v)
{return a733_pcie_read(t->port,a,w,v);}
static int wr(struct a733_pcie_epf_test *t,uintptr_t a,unsigned w,uint32_t v)
{return a733_pcie_write(t->port,a,w,v);}
static int equal(struct a733_pcie_epf_test *t,uintptr_t a,unsigned w,uint32_t mask,uint32_t v)
{uint32_t got;return rd(t,a,w,&got)&&(got&mask)==v;}
static uint8_t pattern(const struct a733_pcie_epf_test *t,size_t n)
{return (uint8_t)((n*37u+(n>>7)*19u+(uint32_t)t->config.identity*13u)^0x5bu);}
static void retain_memory(struct a733_pcie_epf_test *t)
{for(unsigned i=0;i<3;++i)if(t->leases[i].generation)(void)device_memory_retain(t->pool,t->leases[i]);}
void a733_pcie_epf_stop(struct a733_pcie_epf_test *t)
{if(!t||t->self!=t)return;t->failed=1;t->irq.stop(t->irq.cookie);retain_memory(t);}
static int buffers(struct a733_pcie_epf_test *t)
{
    for(unsigned i=0;i<3;++i){struct device_memory_extent e;enum device_memory_state state;
        if(!device_memory_describe(t->pool,t->leases[i],&e,&state)||e.cpu!=t->memory[i].cpu||
            e.physical!=t->memory[i].physical||e.bytes!=t->memory[i].bytes||
            (state!=DEVICE_MEMORY_CPU&&state!=DEVICE_MEMORY_DEVICE))return 0;
    }
    return 1;
}
int a733_pcie_epf_init(struct a733_pcie_epf_test *t,struct a733_pcie *p,
    const struct a733_pcie_epf_config *c,struct device_memory_pool *pool,const struct a733_pcie_epf_irq *irq)
{
    if(!t||t->self||!p||p->self!=p||p->test||!c||!c->identity||!c->timeout_us||c->timeout_us>5000000||
        !c->vendor||c->vendor==65535||!c->device||c->device==65535||c->bar>5||!pool||!pool->initialized||
        !irq||!irq->arm||!irq->event||!irq->stop||!separate(t,sizeof(*t),p,sizeof(*p))||
        !separate(t,sizeof(*t),pool,sizeof(*pool))||!separate(t,sizeof(*t),c,sizeof(*c))||
        !separate(t,sizeof(*t),irq,sizeof(*irq))||a733_pcie_output_outside(p,t,sizeof(*t))!=1)return 0;
    *t=(struct a733_pcie_epf_test){.self=t,.port=p,.pool=pool,.config=*c,.irq=*irq};
    for(unsigned i=0;i<3;++i){
        size_t bytes=i==2?4096:A733_PCIE_TEST_ALLOCATION;
        if(!device_memory_acquire(pool,p->config.owner,bytes,&t->leases[i],&t->memory[i])||
            t->memory[i].cpu!=t->memory[i].physical||t->memory[i].physical>UINT32_MAX-bytes){
            a733_pcie_epf_stop(t);return 0;
        }
    }
    for(size_t n=0;n<A733_PCIE_TEST_ALLOCATION;++n){
        ((uint8_t *)t->memory[0].cpu)[n]=n>=4096&&n<8192?pattern(t,n-4096):0x73;
        ((uint8_t *)t->memory[1].cpu)[n]=0xa5;
    }
    memset((void *)t->memory[2].cpu,0,4096);return 1;
}
static int bar(struct a733_pcie_epf_test *t)
{
    uint32_t original,lo,hi=0,bars[6];unsigned at=0x10+4*t->config.bar;
    if(!rd(t,CFG+4,2,&t->command)||(t->command&7))return 0;
    for(unsigned i=0;i<6;++i)if(!rd(t,CFG+0x10+4*i,4,&bars[i]))return 0;
    /* MSE enables every memory BAR: admit one cold unallocated function,
     * including every paired upper dword, before assigning only our BAR. */
    for(unsigned i=0;i<6;++i){
        uint32_t value=bars[i];unsigned type=value&6;
        if((value&1)||type==2||type==6||(value&~15u))return 0;
        if(type==4){
            if(i==5||bars[i+1]||t->config.bar==i+1)return 0;
            ++i;
        }
    }
    original=bars[t->config.bar];unsigned wide=(original&6)==4;t->bar64=wide;
    if(!wr(t,CFG+at,4,UINT32_MAX)||(wide&&!wr(t,CFG+at+4,4,UINT32_MAX))||
       !rd(t,CFG+at,4,&lo)||(wide&&!rd(t,CFG+at+4,4,&hi)))return 0;
    uint64_t mask=wide?((uint64_t)hi<<32)|(lo&~UINT64_C(15)):(lo&~UINT32_C(15));
    uint64_t bytes=wide?~mask+1:(uint32_t)(~(uint32_t)mask+1);
    if(!mask||(lo&15)!=(original&15)||bytes<48||bytes>A733_PCIE_TEST_WINDOW_BYTES||
       (bytes&(bytes-1))||(MEM&(bytes-1)))return 0;
    if(!wr(t,CFG+at,4,MEM|(original&15))||(wide&&!wr(t,CFG+at+4,4,0))||
       !equal(t,CFG+at,4,UINT32_MAX,MEM|(original&15))||
       (wide&&!equal(t,CFG+at+4,4,UINT32_MAX,0)))return 0;
    t->bar_bytes=bytes;return 1;
}
static int window(struct a733_pcie_epf_test *t)
{
    if(!a733_pcie_atu_program(t->port,1,0,MEM,MEM,A733_PCIE_TEST_WINDOW_BYTES)||
       !equal(t,ATU+4,4,B(31),B(31))||
       !wr(t,DBI+0x20,4,0x22002200)||!equal(t,DBI+0x20,4,UINT32_MAX,0x22002200)||
       !wr(t,CFG+4,2,(t->command&~0x407u)|0x402)||
       !equal(t,CFG+4,2,0xffff,(t->command&~0x407u)|0x402))return 0;
    uint32_t magic;
    if(!rd(t,MEM,4,&magic)||!wr(t,MEM,4,0xa0a0a0a0)||!equal(t,MEM,4,UINT32_MAX,0xa0a0a0a0)||
       !wr(t,MEM,4,magic)||!equal(t,MEM+4,4,UINT32_MAX,0)||!equal(t,MEM+8,4,UINT32_MAX,0))return 0;
    return 1;
}
static int msi(struct a733_pcie_epf_test *t)
{
    unsigned cap=t->port->endpoint.msi_cap;uint32_t flags,devctl;
    if(!cap||!rd(t,CFG+cap+2,2,&flags)||(flags&0x71)||
       !rd(t,CFG+t->port->endpoint.pcie_cap+8,2,&devctl)||(devctl&(B(4)|B(11))))return 0;
    uint32_t capabilities;
    unsigned pcie=t->port->endpoint.pcie_cap;
    if(!rd(t,CFG+pcie+2,2,&capabilities))return 0;
    if((capabilities&15)>=2&&(pcie>0xc4||!rd(t,CFG+pcie+0x28,2,&devctl)||(devctl&(B(8)|B(9)))))return 0;
    t->msi_control=flags;t->msi64=(flags>>7)&1;
    if(flags&B(8)){unsigned at=cap+(t->msi64?16:12);uint32_t mask;if(!rd(t,CFG+at,4,&mask)||(mask&1))return 0;}
    for(unsigned i=0;i<8;++i)if(!equal(t,DBI+0x830+12*i,4,UINT32_MAX,0))return 0;
    uint32_t target=(uint32_t)t->memory[2].physical;
    if(!wr(t,DBI+0x820,4,target)||!wr(t,DBI+0x824,4,0)||
       !equal(t,DBI+0x820,4,UINT32_MAX,target)||!equal(t,DBI+0x824,4,UINT32_MAX,0)||
       !wr(t,CFG+cap+4,4,target)||(t->msi64&&!wr(t,CFG+cap+8,4,0))||
       !wr(t,CFG+cap+(t->msi64?12:8),2,0)||!wr(t,CFG+cap+2,2,flags|1)||
       !equal(t,CFG+cap+2,2,0xffff,flags|1)||!equal(t,CFG+cap+4,4,UINT32_MAX,target)||
       (t->msi64&&!equal(t,CFG+cap+8,4,UINT32_MAX,0))||
       !equal(t,CFG+cap+(t->msi64?12:8),2,0xffff,0)||
       !wr(t,DBI+0x828,4,1)||!wr(t,DBI+0x82c,4,UINT32_MAX-1)||
       !equal(t,DBI+0x828,4,UINT32_MAX,1)||!equal(t,DBI+0x82c,4,UINT32_MAX,UINT32_MAX-1)||
       !wr(t,DBI+4,2,0x106)||!equal(t,DBI+4,2,0xffff,0x106))return 0;
    return 1;
}
static int prepare(struct a733_pcie_epf_test *t)
{
    uint64_t src=t->memory[0].physical+4096,dst=t->memory[1].physical+4096;
    return buffers(t)&&wr(t,MEM+12,4,(uint32_t)src)&&wr(t,MEM+16,4,0)&&
        wr(t,MEM+20,4,(uint32_t)dst)&&wr(t,MEM+24,4,0)&&wr(t,MEM+28,4,4096)&&
        wr(t,MEM+32,4,0)&&wr(t,MEM+36,4,1)&&wr(t,MEM+40,4,1)&&wr(t,MEM+44,4,1)&&
        equal(t,MEM+12,4,UINT32_MAX,(uint32_t)src)&&equal(t,MEM+16,4,UINT32_MAX,0)&&
        equal(t,MEM+20,4,UINT32_MAX,(uint32_t)dst)&&equal(t,MEM+24,4,UINT32_MAX,0)&&
        equal(t,MEM+28,4,UINT32_MAX,4096)&&equal(t,MEM+36,4,UINT32_MAX,1)&&
        equal(t,MEM+40,4,UINT32_MAX,1)&&equal(t,MEM+44,4,UINT32_MAX,1);
}
static int metadata(struct a733_pcie_epf_test *t)
{
    return equal(t,CFG,4,UINT32_MAX,((uint32_t)t->config.device<<16)|t->config.vendor)&&
        equal(t,CFG+4,2,0xffff,(t->command&~0x407u)|(t->submitted?0x406:0x402))&&
        equal(t,CFG+t->port->endpoint.msi_cap+2,2,0xffff,t->msi_control|1)&&
        equal(t,DBI+4,2,0xffff,0x106)&&equal(t,ATU+4,4,B(31),B(31))&&
        equal(t,DBI+t->port->root_cap+0x12,2,0x3ff,t->port->endpoint.link_status&0x3ffu)&&
        equal(t,CFG+t->port->endpoint.pcie_cap+0x12,2,0x3ff,t->port->endpoint.link_status&0x3ffu);
}
static int contents(struct a733_pcie_epf_test *t)
{
    const volatile uint8_t *src=(const void *)t->memory[0].cpu,*dst=(const void *)t->memory[1].cpu;
    for(size_t n=0;n<A733_PCIE_TEST_ALLOCATION;++n){uint8_t expected=n>=4096&&n<8192?pattern(t,n-4096):0x73;
        if(src[n]!=expected||dst[n]!=(n>=4096&&n<8192?pattern(t,n-4096):0xa5))return 0;}
    return 1;
}
enum a733_pcie_result a733_pcie_epf_poll(struct a733_pcie_epf_test *t)
{
    if(!t||t->self!=t)return A733_PCIE_INVALID;
    if(t->failed)return A733_PCIE_RETAINED;
    struct a733_pcie *p=t->port;
    uint64_t now;
    if(p->ops.current(p->cookie)!=1||p->ops.now(p->cookie,&now)!=1||now>=p->config.lifetime_deadline){
        a733_pcie_epf_stop(t);return A733_PCIE_RETAINED;}

    if(t->stage==CLAIM){enum a733_pcie_result r=a733_pcie_test_claim(p,t);
        if(r==A733_PCIE_READY){t->claimed=1;t->stage=BAR;}else if(r!=A733_PCIE_BUSY&&r!=A733_PCIE_PENDING)a733_pcie_epf_stop(t);
        return t->failed?A733_PCIE_RETAINED:A733_PCIE_PENDING;}
    if(t->stage==ARM){int r=t->irq.arm(t->irq.cookie);if(r<0){a733_pcie_epf_stop(t);return A733_PCIE_RETAINED;}if(!r)return A733_PCIE_PENDING;t->stage=SUBMIT;}
    if(t->stage==WAIT&&now>=t->operation_deadline){a733_pcie_epf_stop(t);return A733_PCIE_RETAINED;}
    if(t->stage>=WAIT){uint32_t count=0;int r=t->irq.event(t->irq.cookie,&count);
        if(r<0||count>1||(r==1&&count!=1)){a733_pcie_epf_stop(t);return A733_PCIE_RETAINED;}t->event_count=count;}
    uint64_t deadline=t->stage==WAIT?t->operation_deadline:p->config.lifetime_deadline;
    enum a733_pcie_result r=a733_pcie_test_lock(p,t,deadline);
    if(r!=A733_PCIE_READY){if(r!=A733_PCIE_BUSY&&r!=A733_PCIE_PENDING)a733_pcie_epf_stop(t);return r;}
    int ok=1;
    switch(t->stage){
    case BAR:ok=p->endpoint.vendor==t->config.vendor&&p->endpoint.device==t->config.device&&bar(t);if(ok)t->stage=WINDOW;break;
    case WINDOW:ok=window(t);if(ok)t->stage=MSI;break;
    case MSI:ok=msi(t);if(ok)t->stage=PREPARE;break;
    case PREPARE:ok=prepare(t);if(ok)t->stage=ARM;break;
    case SUBMIT:
        ok=metadata(t)&&equal(t,MEM+4,4,UINT32_MAX,0)&&equal(t,MEM+8,4,UINT32_MAX,0);
        if(ok){
            for(unsigned i=0;i<3;++i){struct device_memory_allocation *a=device_memory_lookup(t->pool,t->leases[i]);if(!a||a->state!=DEVICE_MEMORY_CPU){ok=0;break;}a->state=DEVICE_MEMORY_DEVICE;}
            if(ok){t->operation_deadline=p->last_now+t->config.timeout_us;
                if(t->operation_deadline<p->last_now||t->operation_deadline>p->config.lifetime_deadline)ok=0;}
            if(ok)ok=wr(t,CFG+4,2,(t->command&~0x407u)|0x406)&&wr(t,MEM+4,4,B(5));
            if(ok){t->submitted=1;t->stage=WAIT;}
        }
        break;
    case WAIT: {
        uint32_t pending=0;ok=metadata(t)&&rd(t,DBI+0x830,4,&pending)&&!(pending&~1u);
        for(unsigned i=1;ok&&i<8;++i)ok=equal(t,DBI+0x830+12*i,4,UINT32_MAX,0);
        if(ok&&t->event_count){
            ok=pending==1&&rd(t,MEM+8,4,&t->status)&&t->status==(B(4)|B(6))&&
                equal(t,MEM+4,4,UINT32_MAX,0)&&buffers(t)&&contents(t)&&wr(t,DBI+0x830,4,1)&&
                equal(t,DBI+0x830,4,UINT32_MAX,0);
            if(ok){t->stage=DONE;t->irq.stop(t->irq.cookie);}
        }
        break;
    }
    case DONE:ok=metadata(t)&&equal(t,MEM+8,4,UINT32_MAX,B(4)|B(6));
        for(unsigned i=0;ok&&i<8;++i)ok=equal(t,DBI+0x830+12*i,4,UINT32_MAX,0);
        break;
    default:ok=0;break;
    }
    r=a733_pcie_test_end(p,ok);
    if(r!=A733_PCIE_READY&&r!=A733_PCIE_BUSY){a733_pcie_epf_stop(t);return A733_PCIE_RETAINED;}
    return r==A733_PCIE_READY&&t->stage==DONE?A733_PCIE_READY:A733_PCIE_PENDING;
}
enum a733_pcie_result a733_pcie_epf_copy(struct a733_pcie_epf_test *t,uint8_t out[4096])
{
    if(!t||t->self!=t||!out||t->published||t->stage!=DONE||t->failed||
       !separate(out,4096,t,sizeof(*t))||!separate(out,4096,t->pool,sizeof(*t->pool))||
       !separate(out,4096,(void *)t->pool->backing.cpu,t->pool->backing.bytes)||
       a733_pcie_output_outside(t->port,out,4096)!=1)return A733_PCIE_INVALID;
    enum a733_pcie_result r=a733_pcie_epf_poll(t);if(r!=A733_PCIE_READY)return r;
    if(!contents(t)){a733_pcie_epf_stop(t);return A733_PCIE_RETAINED;}
    memcpy(out,(void *)(t->memory[1].cpu+4096),4096);
    r=a733_pcie_epf_poll(t);if(r!=A733_PCIE_READY)return r;
    t->published=1;retain_memory(t);return A733_PCIE_READY;
}
static const struct a733_pcie_epf_test *native_test(const struct a733_pcie *p)
{const struct a733_pcie_epf_test *t=p?p->test:NULL;return t&&t->self==t&&t->port==p&&t->claimed&&!t->failed?t:NULL;}
int a733_pcie_epf_native_address(const struct a733_pcie *p,uintptr_t a,unsigned w,unsigned writing)
{
    const struct a733_pcie_epf_test *t=native_test(p);if(!t||!w||(a&(w-1)))return 0;
    if(w==4&&a>=MEM&&a<=MEM+44)return 1;
    if(w==4&&a>=ATU&&a<=ATU+24)return writing||a==ATU+4;
    if(w==4&&(a==DBI+0x20||a==DBI+0x820||a==DBI+0x824||a==DBI+0x828||a==DBI+0x82c||a==DBI+0x830))return 1;
    if(w==2&&(a==DBI+4||a==CFG+4))return 1;
    unsigned bar=0x10+4*t->config.bar,cap=p->endpoint.msi_cap;
    if(w==4&&(a==CFG+bar||(t->bar64&&t->config.bar<5&&a==CFG+bar+4)))return writing;
    if(!cap)return 0;
    return (w==2&&(a==CFG+cap+2||a==CFG+cap+(t->msi64?12:8)))||
        (w==4&&(a==CFG+cap+4||(t->msi64&&a==CFG+cap+8)));
}
int a733_pcie_epf_native_write(const struct a733_pcie *p,uintptr_t a,unsigned w,uint32_t v)
{
    const struct a733_pcie_epf_test *t=native_test(p);
    if(!t||!a733_pcie_epf_native_address(p,a,w,1))return 0;
    if(a>=MEM&&a<=MEM+44){
        switch(a-MEM){
        case 0:return 1; /* Original magic or the fixed MMIO test pattern. */
        case 4:return t->stage==SUBMIT&&v==B(5)&&!t->submitted;
        case 12:return v==(uint32_t)(t->memory[0].physical+4096);
        case 20:return v==(uint32_t)(t->memory[1].physical+4096);
        case 16:case 24:case 32:return !v;
        case 28:return v==4096;
        case 36:case 40:case 44:return v==1;
        default:return 0;
        }
    }
    if(a>=ATU&&a<=ATU+24)return a733_pcie_atu_write_allowed(p,a,v);
    if(a==DBI+4)return v==0x106;
    if(a==DBI+0x20)return v==0x22002200;
    if(a==DBI+0x820)return v==(uint32_t)t->memory[2].physical;
    if(a==DBI+0x824)return !v;
    if(a==DBI+0x828||a==DBI+0x830)return v==1;
    if(a==DBI+0x82c)return v==UINT32_MAX-1;
    if(a==CFG+4)return v==((t->command&~0x407u)|0x402)||v==((t->command&~0x407u)|0x406);
    unsigned at=0x10+4*t->config.bar,cap=p->endpoint.msi_cap;
    if(a==CFG+at)return v==UINT32_MAX||(v&~15u)==MEM;
    if(a==CFG+at+4)return v==UINT32_MAX||!v;
    if(a==CFG+cap+2)return v==(t->msi_control|1);
    if(a==CFG+cap+4)return v==(uint32_t)t->memory[2].physical;
    return !v;
}
