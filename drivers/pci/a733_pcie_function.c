/* BAR sizing and placement follow the pinned U-Boot dm_pciauto_setup_device()
 * (drivers/pci/pci_auto.c). INTx status/mask and DesignWare MSI programming
 * follow the board kernel's pcie-sunxi-rc.c and pcie-sunxi-plat.c. Hashes and
 * licenses are in source.json beside this file. */
#include "a733_pcie_function.h"
#include "a733_range.h"
#define CFG UINT32_C(0x20000000)
#define DBI UINT32_C(0x06000000)
#define APP UINT32_C(0x06400000)
#define ATU0 A733_PCIE_ATU_OUTBOUND(0)
#define ATU1 A733_PCIE_ATU_OUTBOUND(1)
#define MEM A733_PCIE_MEM
#define B(n) (UINT32_C(1)<<(n))
#define GRANULE UINT32_C(0x100000) /* Type 1 memory base/limit granularity. */
#define CLOSED UINT32_C(0x0000fff0) /* Memory base above limit: no forwarding. */
#define INTX_ASSERT UINT32_C(0x1e0) /* SII INTA..INTD assert bits 5..8. */
enum { CLAIM,SCAN,BARS,WINDOW,IRQ,ARM,READY,SUSPEND,SUSPEND_WAIT,SUSPENDED,
    RESUME,RESUME_WAIT,RESTORE,STOP,STOPPED };
static int rd(struct a733_pcie_function *f,uintptr_t a,unsigned w,uint32_t *v)
{return a733_pcie_read(f->port,a,w,v);}
static int wr(struct a733_pcie_function *f,uintptr_t a,unsigned w,uint32_t v)
{return a733_pcie_write(f->port,a,w,v);}
static int equal(struct a733_pcie_function *f,uintptr_t a,unsigned w,uint32_t mask,uint32_t v)
{uint32_t got;return rd(f,a,w,&got)&&(got&mask)==v;}
static const struct a733_pcie_function_id *claimed(const struct a733_pcie_function *f)
{return &f->ids[f->config.function];}
/* Memory decode only with assigned BARs; INTx disabled unless it is the mode. */
static uint32_t command(const struct a733_pcie_function *f,unsigned master)
{
    return (f->command&~UINT32_C(0x407))|(f->window_bytes?2u:0)|(master?4u:0)|
        (f->config.irq==A733_PCIE_IRQ_INTX?0:0x400u);
}
static uint32_t stopped_command(const struct a733_pcie_function *f)
{return (f->command&~UINT32_C(0x407))|0x400u;}
static uint32_t window(const struct a733_pcie_function *f)
{uint32_t last=MEM+f->window_bytes-1;return ((last>>16)&0xfff0u)<<16|((MEM>>16)&0xfff0u);}
/* Device 0 on the root bus: INTx pin n arrives as SII line n-1, unswizzled. */
static uint32_t intx(const struct a733_pcie_function *f)
{return B(5+claimed(f)->interrupt_pin-1);}
static uint32_t vectors(const struct a733_pcie_function *f)
{return B(f->config.vectors)-1;}
static void fault(struct a733_pcie_function *f)
{
    if(f->failed)return;
    f->failed=1;
    if(f->config.irq!=A733_PCIE_IRQ_POLL)f->irq.stop(f->irq.cookie);
    if(f->msi_lease.generation)(void)device_memory_retain(f->pool,f->msi_lease);
}
int a733_pcie_function_init(struct a733_pcie_function *f,struct a733_pcie *p,
    const struct a733_pcie_function_config *c,struct device_memory_pool *pool,
    const struct a733_pcie_irq_ops *irq)
{
    if(!f||f->self||!p||p->self!=p||p->test||p->function||!c||!c->identity||!c->vendor||
       c->vendor==65535||!c->device||c->device==65535||c->class_code>0xffffff||c->function>7||
       c->irq>A733_PCIE_IRQ_MSIX||!c->write_allowed||!pool||!pool->initialized||
       (c->irq==A733_PCIE_IRQ_MSIX?!c->vectors||c->vectors>A733_PCIE_MSIX_VECTORS:c->vectors!=0)||
       (c->irq!=A733_PCIE_IRQ_POLL&&(!irq||!irq->arm||!irq->event||!irq->stop))||
       !a733_ranges_disjoint(f,sizeof(*f),p,sizeof(*p))||!a733_ranges_disjoint(f,sizeof(*f),pool,sizeof(*pool))||
       !a733_ranges_disjoint(f,sizeof(*f),c,sizeof(*c))||(irq&&!a733_ranges_disjoint(f,sizeof(*f),irq,sizeof(*irq)))||
       a733_pcie_output_outside(p,f,sizeof(*f))!=1)return 0;
    *f=(struct a733_pcie_function){.self=f,.port=p,.pool=pool,.config=*c};
    if(irq)f->irq=*irq;
    /* A pool page reserves the MSI target address; the controller absorbs the writes. */
    if(c->irq==A733_PCIE_IRQ_MSIX&&(!device_memory_acquire(pool,p->config.owner,4096,
       &f->msi_lease,&f->msi_memory)||f->msi_memory.physical>UINT32_MAX-4096)){
        f->config.irq=A733_PCIE_IRQ_POLL;fault(f);return 0;
    }
    return 1;
}
static int target(struct a733_pcie_function *f,unsigned function)
{
    return a733_pcie_atu_program(f->port,0,4,CFG,0x01000000u|(function<<16),4096)&&
        equal(f,ATU0+4,4,B(31),B(31));
}
/* Bounded capability walk; a cold function has MSI, MSI-X and PME disabled in D0. */
static int capabilities(struct a733_pcie_function *f,struct a733_pcie_function_id *id)
{
    uint32_t v;uint64_t seen=0;
    if(!rd(f,CFG+6,2,&v))return 0;
    if(!(v&B(4)))return 1;
    if(!rd(f,CFG+0x34,1,&v))return 0;
    for(unsigned i=0;v&&i<48;++i){
        if(v<0x40||v>0xfc||(v&3)||(seen&(UINT64_C(1)<<(v/4))))return 0;
        seen|=UINT64_C(1)<<(v/4);unsigned at=v;
        if(!rd(f,CFG+at,2,&v))return 0;
        unsigned capability=v&255,next=(v>>8)&255,last=0;uint8_t *slot=NULL;
        switch(capability){
        case 0x10:slot=&id->pcie_cap;last=0xc4;break;
        case 5:slot=&id->msi_cap;last=0xe8;break;
        case 0x11:slot=&id->msix_cap;last=0xf4;break;
        case 1:slot=&id->pm_cap;last=0xf8;break;
        default:break;
        }
        if(slot){if(*slot||at>last)return 0;*slot=(uint8_t)at;}
        if((capability==5&&(!rd(f,CFG+at+2,2,&v)||(v&1)))||
           (capability==0x11&&(!rd(f,CFG+at+2,2,&v)||(v&0xc000)))||
           (capability==1&&(!rd(f,CFG+at+4,2,&v)||(v&0x103))))return 0;
        v=next;
    }
    return !v;
}
/* Function 0, then 1..7 when it is multifunction. Every present function must
 * be a cold type-0 header; bridges would need CFG1 routing and bus numbers. */
static int scan(struct a733_pcie_function *f)
{
    const struct a733_pcie *p=f->port;uint32_t v;unsigned last=0;
    for(unsigned function=0;function<=last;++function){
        struct a733_pcie_function_id id={0};
        if(!target(f,function)||!rd(f,CFG,4,&v))return 0;
        if(function&&(v&65535)==65535)continue;
        if(!(v&65535)||(v&65535)==1||(v&65535)==65535)return 0;
        id.vendor=(uint16_t)v;id.device=(uint16_t)(v>>16);
        if(!rd(f,CFG+4,2,&v)||(v&7))return 0;
        if(function==f->config.function)f->command=v;
        if(!rd(f,CFG+8,4,&id.class_revision)||!rd(f,CFG+14,1,&v)||(v&0x7f))return 0;
        id.header_type=(uint8_t)v;
        if(!function&&(v&0x80))last=7;
        if(!rd(f,CFG+0x3d,1,&v)||v>4)return 0;
        id.interrupt_pin=(uint8_t)v;
        if(!capabilities(f,&id))return 0;
        f->ids[function]=id;f->present|=(uint8_t)B(function);
    }
    const struct a733_pcie_function_id *id=claimed(f);
    if(!(f->present&B(f->config.function))||f->ids[0].vendor!=p->endpoint.vendor||
       f->ids[0].device!=p->endpoint.device||id->vendor!=f->config.vendor||
       id->device!=f->config.device||(id->class_revision>>8)!=f->config.class_code||!id->pcie_cap||
       (f->config.irq==A733_PCIE_IRQ_INTX&&!id->interrupt_pin)||
       (f->config.irq==A733_PCIE_IRQ_MSIX&&!id->msix_cap))return 0;
    return target(f,f->config.function);
}
static int restore(struct a733_pcie_function *f)
{
    for(unsigned i=0;i<6;++i){
        const struct a733_pcie_bar *b=&f->bars[i];
        if(!b->bytes)continue;
        uintptr_t at=CFG+0x10+4*i;unsigned wide=(b->flags&6)==4;
        if(!wr(f,at,4,b->base|b->flags)||(wide&&!wr(f,at+4,4,0))||
           !equal(f,at,4,UINT32_MAX,b->base|b->flags)||(wide&&!equal(f,at+4,4,UINT32_MAX,0)))return 0;
    }
    return 1;
}
/* Every slot must be unassigned. Memory BARs are sized with decode off, then
 * placed largest first at natural alignment from the aperture base; 64-bit
 * BARs stay below 4 GiB. I/O BARs are recorded and never written. */
static int bars(struct a733_pcie_function *f)
{
    uint32_t value[6],size[6]={0};
    for(unsigned i=0;i<6;++i)if(!rd(f,CFG+0x10+4*i,4,&value[i]))return 0;
    f->io=f->upper=0;
    for(unsigned i=0;i<6;++i){
        if(value[i]&1){if(value[i]&~3u)return 0;f->io|=(uint8_t)B(i);continue;}
        unsigned type=value[i]&6;
        if(type==2||type==6||(value[i]&~15u))return 0;
        if(type==4){if(i==5||value[i+1])return 0;f->upper|=(uint8_t)B(i+1);++i;}
    }
    for(unsigned i=0;i<6;++i){
        f->bars[i]=(struct a733_pcie_bar){0};
        if((f->io|f->upper)&B(i))continue;
        uintptr_t at=CFG+0x10+4*i;unsigned wide=(value[i]&6)==4;uint32_t lo,hi=UINT32_MAX;
        if(!wr(f,at,4,UINT32_MAX)||(wide&&!wr(f,at+4,4,UINT32_MAX))||
           !rd(f,at,4,&lo)||(wide&&!rd(f,at+4,4,&hi)))return 0;
        if(!lo&&!wide)continue; /* Unimplemented slot. */
        uint32_t mask=lo&~15u,bytes=~mask+1;
        if((lo&15)!=(value[i]&15)||hi!=UINT32_MAX||!mask||(bytes&(bytes-1))||
           bytes>A733_PCIE_MEM_BYTES)return 0;
        size[i]=bytes;f->bars[i].flags=value[i]&15;
    }
    uint32_t cursor=MEM;unsigned placed=0;
    for(;;){
        unsigned best=6;
        for(unsigned i=0;i<6;++i)
            if(size[i]&&!(placed&B(i))&&(best==6||size[i]>size[best]))best=i;
        if(best==6)break;
        uint32_t bytes=size[best],base=(cursor+bytes-1)&~(bytes-1);
        if(bytes>A733_PCIE_MEM_BYTES||base<cursor||base-MEM>A733_PCIE_MEM_BYTES-bytes)return 0;
        f->bars[best].base=base;f->bars[best].bytes=bytes;placed|=B(best);cursor=base+bytes;
    }
    f->window_bytes=(cursor-MEM+GRANULE-1)&~(GRANULE-1);
    return f->window_bytes<=A733_PCIE_MEM_BYTES&&restore(f);
}
/* Region one, the root bridge memory window and root bus mastering, then the
 * function's memory decode with bus mastering still clear. */
static int open(struct a733_pcie_function *f)
{
    if(f->window_bytes){
        if((!f->configured&&!a733_pcie_atu_program(f->port,1,0,MEM,MEM,f->window_bytes))||
           !equal(f,ATU1+4,4,B(31),B(31))||!wr(f,DBI+0x20,4,window(f))||
           !equal(f,DBI+0x20,4,UINT32_MAX,window(f)))return 0;
    }
    return wr(f,DBI+4,2,0x106)&&equal(f,DBI+4,2,0xffff,0x106)&&
        wr(f,CFG+4,2,command(f,0))&&equal(f,CFG+4,2,0xffff,command(f,0));
}
static int interrupts(struct a733_pcie_function *f)
{
    uint32_t v;
    if(f->config.irq==A733_PCIE_IRQ_INTX){
        /* Clear a stale assert latch, then unmask only this line. */
        uint32_t bit=intx(f);
        if(!rd(f,APP+0xe00,4,&v))return 0;
        if(!f->configured){if(v&INTX_ASSERT)return 0;f->sii=v;}
        else if(v!=(f->sii|bit))return 0;
        return wr(f,APP+0xe08,4,bit)&&wr(f,APP+0xe00,4,f->sii|bit)&&
            equal(f,APP+0xe00,4,UINT32_MAX,f->sii|bit);
    }
    if(f->config.irq!=A733_PCIE_IRQ_MSIX)return 1;
    unsigned cap=claimed(f)->msix_cap;uint32_t control,table,pba;
    if(!rd(f,CFG+cap+2,2,&control)||(control&0xc000)||!rd(f,CFG+cap+4,4,&table)||
       !rd(f,CFG+cap+8,4,&pba))return 0;
    unsigned entries=(control&0x7ff)+1,tb=table&7,pb=pba&7;
    table&=~7u;pba&=~7u;
    if(tb>5||pb>5||!f->bars[tb].bytes||!f->bars[pb].bytes||entries<f->config.vectors||
       table>f->bars[tb].bytes||entries*16>f->bars[tb].bytes-table||
       pba>f->bars[pb].bytes||(entries+63)/64*8>f->bars[pb].bytes-pba)return 0;
    f->msix_control=(uint16_t)control;f->msix_bar=(uint8_t)tb;f->pba_bar=(uint8_t)pb;
    f->msix_table=table;f->msix_pba=pba;f->msix_entries=entries;
    for(unsigned i=0;i<8;++i)if(!equal(f,DBI+0x830+12*i,4,UINT32_MAX,0))return 0;
    uint32_t msi=(uint32_t)f->msi_memory.physical,mask=vectors(f),base=control&0x3fffu;
    if(!wr(f,DBI+0x820,4,msi)||!wr(f,DBI+0x824,4,0)||!equal(f,DBI+0x820,4,UINT32_MAX,msi)||
       !equal(f,DBI+0x824,4,UINT32_MAX,0)||!wr(f,DBI+0x828,4,mask)||!wr(f,DBI+0x82c,4,~mask)||
       !equal(f,DBI+0x828,4,UINT32_MAX,mask)||!equal(f,DBI+0x82c,4,UINT32_MAX,~mask))return 0;
    /* Enable with the whole function masked, so no entry can fire while it is
     * half written; program each entry masked, then unmask the function. */
    if(!wr(f,CFG+cap+2,2,base|0xc000)||!equal(f,CFG+cap+2,2,0xc000,0xc000))return 0;
    uintptr_t at=f->bars[tb].base+table;
    for(unsigned i=0;i<entries;++i,at+=16){
        if(i>=f->config.vectors){if(!equal(f,at+12,4,1,1))return 0;continue;}
        if(!wr(f,at+12,4,1)||!wr(f,at,4,msi)||!wr(f,at+4,4,0)||!wr(f,at+8,4,i)||!wr(f,at+12,4,0)||
           !equal(f,at,4,UINT32_MAX,msi)||!equal(f,at+4,4,UINT32_MAX,0)||
           !equal(f,at+8,4,UINT32_MAX,i)||!equal(f,at+12,4,1,0))return 0;
    }
    return wr(f,CFG+cap+2,2,base|0x8000)&&equal(f,CFG+cap+2,2,0xc000,0x8000);
}
/* Bus mastering is already clear; mask delivery, stop decode, close windows. */
static int quiesce(struct a733_pcie_function *f)
{
    if(f->master)return 0;
    if(f->config.irq==A733_PCIE_IRQ_MSIX){
        unsigned cap=claimed(f)->msix_cap;uint32_t off=(f->msix_control&0x3fffu)|0x4000;
        if(!wr(f,CFG+cap+2,2,off)||!equal(f,CFG+cap+2,2,0xc000,0x4000)||
           !wr(f,DBI+0x828,4,0)||!wr(f,DBI+0x82c,4,UINT32_MAX)||
           !equal(f,DBI+0x828,4,UINT32_MAX,0)||!equal(f,DBI+0x82c,4,UINT32_MAX,UINT32_MAX))return 0;
    }
    if(f->config.irq==A733_PCIE_IRQ_INTX&&(!wr(f,APP+0xe00,4,f->sii)||
       !equal(f,APP+0xe00,4,UINT32_MAX,f->sii)||!wr(f,APP+0xe08,4,intx(f))))return 0;
    if(!wr(f,CFG+4,2,stopped_command(f))||!equal(f,CFG+4,2,0xffff,stopped_command(f)))return 0;
    if(f->window_bytes&&(!wr(f,DBI+0x20,4,CLOSED)||!equal(f,DBI+0x20,4,UINT32_MAX,CLOSED)||
       !wr(f,ATU1+4,4,0)||!equal(f,ATU1+4,4,B(31),0)))return 0;
    return wr(f,DBI+4,2,0x102)&&equal(f,DBI+4,2,0xffff,0x102);
}
static int intact(struct a733_pcie_function *f)
{
    for(unsigned i=0;i<6;++i)
        if(f->bars[i].bytes&&!equal(f,CFG+0x10+4*i,4,UINT32_MAX,f->bars[i].base|f->bars[i].flags))return 0;
    return equal(f,CFG+4,2,0xffff,command(f,f->master));
}
enum a733_pcie_result a733_pcie_function_poll(struct a733_pcie_function *f)
{
    if(!f||f->self!=f||f->locked)return A733_PCIE_INVALID;
    if(f->failed)return A733_PCIE_RETAINED;
    if(f->stage==STOPPED)return A733_PCIE_STOPPED;
    struct a733_pcie *p=f->port;uint64_t now;
    if(p->ops.current(p->cookie)!=1||p->ops.now(p->cookie,&now)!=1||now>=p->config.lifetime_deadline){
        fault(f);return A733_PCIE_RETAINED;
    }
    if(f->stage==CLAIM){
        enum a733_pcie_result r=a733_pcie_function_claim(p,f);
        if(r==A733_PCIE_READY)f->stage=SCAN;
        else if(r!=A733_PCIE_BUSY&&r!=A733_PCIE_PENDING)fault(f);
        return f->failed?A733_PCIE_RETAINED:A733_PCIE_PENDING;
    }
    if(f->stage==ARM){
        int r=f->irq.arm(f->irq.cookie);
        if(r<0){fault(f);return A733_PCIE_RETAINED;}
        if(!r)return A733_PCIE_PENDING;
        f->stage=READY;
    }
    if(f->stage==SUSPENDED)return A733_PCIE_PENDING;
    if((f->stage==SUSPEND_WAIT||f->stage==RESUME_WAIT)&&now<f->wait_until)return A733_PCIE_PENDING;
    enum a733_pcie_result r=a733_pcie_function_lock(p,f,p->config.lifetime_deadline);
    if(r!=A733_PCIE_READY){
        if(r!=A733_PCIE_BUSY&&r!=A733_PCIE_PENDING)fault(f);
        return f->failed?A733_PCIE_RETAINED:r;
    }
    unsigned pm=claimed(f)->pm_cap;uint32_t v;int ok=1;
    switch(f->stage){
    case SCAN:ok=scan(f);if(ok)f->stage=BARS;break;
    case BARS:ok=bars(f);if(ok)f->stage=WINDOW;break;
    case RESTORE:ok=restore(f);if(ok)f->stage=WINDOW;break;
    case WINDOW:ok=open(f);if(ok)f->stage=IRQ;break;
    case IRQ:
        ok=interrupts(f);
        if(ok){f->configured=1;f->stage=f->config.irq==A733_PCIE_IRQ_POLL?READY:ARM;}
        break;
    case READY:ok=intact(f)&&(!f->window_bytes||equal(f,ATU1+4,4,B(31),B(31)));break;
    case SUSPEND:
        ok=!f->master&&pm&&wr(f,CFG+pm+4,2,3);
        if(ok){f->wait_until=p->last_now+A733_PCIE_D3_WAIT_US;f->stage=SUSPEND_WAIT;}
        break;
    case SUSPEND_WAIT:
        ok=rd(f,CFG+pm+4,2,&v)&&(v&3)==3;
        if(ok){f->soft_reset=!(v&8);f->stage=SUSPENDED;}
        break;
    case RESUME:
        ok=wr(f,CFG+pm+4,2,0);
        if(ok){f->wait_until=p->last_now+A733_PCIE_D3_WAIT_US;f->stage=RESUME_WAIT;}
        break;
    case RESUME_WAIT:
        /* Without No_Soft_Reset the function lost BARs, decode and MSI-X. */
        ok=rd(f,CFG+pm+4,2,&v)&&(v&3)==0;
        if(ok&&f->soft_reset)f->stage=RESTORE;
        else if(ok){ok=intact(f);f->stage=READY;}
        break;
    case STOP:
        ok=quiesce(f);
        if(ok){f->stage=STOPPED;p->client_stopped=1;}
        break;
    default:ok=0;break;
    }
    r=a733_pcie_client_end(p,ok);
    if(r!=A733_PCIE_READY&&r!=A733_PCIE_BUSY){fault(f);return A733_PCIE_RETAINED;}
    if(f->stage==STOPPED){
        if(f->config.irq!=A733_PCIE_IRQ_POLL)f->irq.stop(f->irq.cookie);
        if(f->msi_lease.generation)(void)device_memory_retain(f->pool,f->msi_lease);
        return A733_PCIE_STOPPED;
    }
    return r==A733_PCIE_READY&&f->stage==READY?A733_PCIE_READY:A733_PCIE_PENDING;
}
enum a733_pcie_result a733_pcie_function_begin(struct a733_pcie_function *f,uint64_t deadline,uint32_t *events)
{
    if(!f||f->self!=f||f->locked||!events||!a733_ranges_disjoint(events,sizeof(*events),f,sizeof(*f))||
       a733_pcie_output_outside(f->port,events,sizeof(*events))!=1)return A733_PCIE_INVALID;
    if(f->failed)return A733_PCIE_RETAINED;
    if(f->stage!=READY)return f->stage==STOPPED?A733_PCIE_STOPPED:A733_PCIE_INVALID;
    struct a733_pcie *p=f->port;
    enum a733_pcie_result r=a733_pcie_function_lock(p,f,deadline);
    if(r!=A733_PCIE_READY){
        if(r!=A733_PCIE_BUSY&&r!=A733_PCIE_PENDING)fault(f);
        return f->failed?A733_PCIE_RETAINED:r;
    }
    uint32_t v,seen=0,count=0;int ok=equal(f,CFG+4,2,0xffff,command(f,f->master));
    if(ok&&f->config.irq==A733_PCIE_IRQ_MSIX){
        ok=rd(f,DBI+0x830,4,&v)&&!(v&~vectors(f));
        for(unsigned i=1;ok&&i<8;++i)ok=equal(f,DBI+0x830+12*i,4,UINT32_MAX,0);
        if(ok&&v){ok=wr(f,DBI+0x830,4,v);seen=v;}
    }
    if(ok&&f->config.irq==A733_PCIE_IRQ_INTX){
        ok=rd(f,APP+0xe08,4,&v)&&!(v&INTX_ASSERT&~intx(f));
        if(ok&&(v&intx(f))){ok=wr(f,APP+0xe08,4,intx(f));seen=1;}
    }
    if(ok&&f->config.irq!=A733_PCIE_IRQ_POLL)ok=f->irq.event(f->irq.cookie,&count)>=0;
    if(!ok){(void)a733_pcie_client_end(p,0);fault(f);return A733_PCIE_RETAINED;}
    f->notifications+=count;f->locked=1;*events=seen;return A733_PCIE_READY;
}
static int bar_write(const struct a733_pcie_function *f,unsigned bar,uint32_t offset,uint32_t v)
{
    if(f->config.irq==A733_PCIE_IRQ_MSIX){
        if(bar==f->msix_bar&&offset>=f->msix_table&&offset-f->msix_table<f->msix_entries*16){
            uint32_t at=offset-f->msix_table,entry=at/16;
            if(f->stage!=IRQ||entry>=f->config.vectors)return 0;
            switch(at%16){
            case 0:return v==(uint32_t)f->msi_memory.physical;
            case 4:return !v;
            case 8:return v==entry;
            default:return v<=1;
            }
        }
        if(bar==f->pba_bar&&offset>=f->msix_pba&&offset-f->msix_pba<(f->msix_entries+63)/64*8)return 0;
    }
    return f->locked&&f->stage==READY&&f->config.write_allowed(f->config.driver,bar,offset,v);
}
static int in_bar(const struct a733_pcie_function *f,unsigned bar,uint32_t offset)
{return f->locked&&f->stage==READY&&bar<6&&f->bars[bar].bytes&&!(offset&3)&&offset<f->bars[bar].bytes;}
int a733_pcie_function_read(struct a733_pcie_function *f,unsigned bar,uint32_t offset,uint32_t *v)
{return f&&f->self==f&&v&&in_bar(f,bar,offset)&&rd(f,f->bars[bar].base+offset,4,v);}
int a733_pcie_function_write(struct a733_pcie_function *f,unsigned bar,uint32_t offset,uint32_t v)
{return f&&f->self==f&&in_bar(f,bar,offset)&&bar_write(f,bar,offset,v)&&wr(f,f->bars[bar].base+offset,4,v);}
int a733_pcie_function_master(struct a733_pcie_function *f,int enable)
{
    if(!f||f->self!=f||!f->locked||f->stage!=READY)return 0;
    uint32_t v=command(f,enable!=0);
    if(!wr(f,CFG+4,2,v)||!equal(f,CFG+4,2,0xffff,v))return 0;
    f->master=enable!=0;return 1;
}
enum a733_pcie_result a733_pcie_function_end(struct a733_pcie_function *f,int ok)
{
    if(!f||f->self!=f||!f->locked)return A733_PCIE_INVALID;
    f->locked=0;
    enum a733_pcie_result r=a733_pcie_client_end(f->port,ok);
    if(r!=A733_PCIE_READY&&r!=A733_PCIE_BUSY){fault(f);return A733_PCIE_RETAINED;}
    return r;
}
static int request(struct a733_pcie_function *f,unsigned from,unsigned to)
{
    if(!f||f->self!=f||f->failed||f->locked||f->master||f->stage!=from||
       (to!=STOP&&!claimed(f)->pm_cap))return 0;
    f->stage=to;return 1;
}
int a733_pcie_function_suspend(struct a733_pcie_function *f){return request(f,READY,SUSPEND);}
int a733_pcie_function_resume(struct a733_pcie_function *f){return request(f,SUSPENDED,RESUME);}
int a733_pcie_function_stop(struct a733_pcie_function *f)
{return request(f,READY,STOP)||request(f,SUSPENDED,STOP);}
static const struct a733_pcie_function *native_function(const struct a733_pcie *p)
{const struct a733_pcie_function *f=p?p->function:NULL;return f&&f->self==f&&f->port==p&&!f->failed?f:NULL;}
static int bar_of(const struct a733_pcie_function *f,uintptr_t a,unsigned *bar,uint32_t *offset)
{
    for(unsigned i=0;i<6;++i)
        if(f->bars[i].bytes&&a>=f->bars[i].base&&a-f->bars[i].base<f->bars[i].bytes){
            *bar=i;*offset=(uint32_t)(a-f->bars[i].base);return 1;
        }
    return 0;
}
int a733_pcie_function_native_address(const struct a733_pcie *p,uintptr_t a,unsigned w,unsigned writing)
{
    const struct a733_pcie_function *f=native_function(p);unsigned bar;uint32_t offset;
    if(!f||!w||(a&(w-1)))return 0;
    if(w==4&&bar_of(f,a,&bar,&offset))return 1;
    if(w==4&&a>=ATU1&&a<=ATU1+24)return writing||a==ATU1+4;
    if(w==4&&(a==DBI+0x20||(a>=DBI+0x820&&a<=DBI+0x830)))return 1;
    if(w==2&&a==DBI+4)return 1;
    if(w==4&&(a==APP+0xe00||a==APP+0xe08))return f->config.irq==A733_PCIE_IRQ_INTX;
    if(!writing)return 0;
    if(w==4&&a>=CFG+0x10&&a<=CFG+0x24)return 1;
    const struct a733_pcie_function_id *id=claimed(f);
    return w==2&&(a==CFG+4||(id->msix_cap&&a==CFG+id->msix_cap+2u)||(id->pm_cap&&a==CFG+id->pm_cap+4u));
}
int a733_pcie_function_native_write(const struct a733_pcie *p,uintptr_t a,unsigned w,uint32_t v)
{
    const struct a733_pcie_function *f=native_function(p);unsigned bar;uint32_t offset;
    if(!f)return 0;
    /* Core writes while claimed: region-zero retargeting and the port stop. */
    if(a>=ATU0&&a<=ATU1+24)return w==4&&a733_pcie_atu_write_allowed(p,a,v);
    if(a==APP+0xc00)return w==4&&p->client_stopped&&!(v&1);
    if(!a733_pcie_function_native_address(p,a,w,1))return 0;
    if(bar_of(f,a,&bar,&offset))return bar_write(f,bar,offset,v);
    unsigned msix=f->config.irq==A733_PCIE_IRQ_MSIX;
    switch(a){
    case DBI+0x20:return v==window(f)||v==CLOSED;
    case DBI+4:return v==0x102||v==0x106;
    case DBI+0x820:return msix&&v==(uint32_t)f->msi_memory.physical;
    case DBI+0x824:return msix&&!v;
    case DBI+0x828:return msix&&(v==vectors(f)||!v);
    case DBI+0x82c:return msix&&(v==~vectors(f)||v==UINT32_MAX);
    case DBI+0x830:return msix&&v&&!(v&~vectors(f));
    case APP+0xe00:return v==f->sii||v==(f->sii|intx(f));
    case APP+0xe08:return v==intx(f);
    default:break;
    }
    if(a>=DBI+0x820&&a<=DBI+0x830)return 0;
    /* Configuration writes reach only the claimed function. */
    if(!p->windows[0].bytes||((p->windows[0].pci>>16)&7)!=f->config.function)return 0;
    const struct a733_pcie_function_id *id=claimed(f);
    if(a==CFG+4)return v==command(f,0)||v==command(f,1)||v==stopped_command(f);
    if(id->pm_cap&&a==CFG+id->pm_cap+4u)return v==0||v==3;
    if(id->msix_cap&&a==CFG+id->msix_cap+2u)
        return msix&&(v&0x3fffu)==(f->msix_control&0x3fffu)&&(v&0xc000);
    unsigned i=(unsigned)(a-CFG-0x10)/4;
    if(i>5||(f->io&B(i)))return 0;
    if(v==UINT32_MAX)return f->stage==BARS;
    if(f->upper&B(i))return !v;
    return f->bars[i].bytes&&v==(f->bars[i].base|f->bars[i].flags);
}
