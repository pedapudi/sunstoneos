/* Register layout, queue setup and command fields follow the pinned GPL-2.0+
 * U-Boot polled driver (drivers/nvme/nvme.c and nvme.h); see source.json here.
 * Queue memory and doorbells follow its two-entry admin and I/O queues. */
#include "a733_pcie_nvme.h"
#include "a733_range.h"
#include "dma_address_width.h"
#include <string.h>
#define DEPTH A733_PCIE_NVME_DEPTH
#define CC_ENABLE UINT32_C(0x00460001) /* EN, 4 KiB pages, NVM, IOSQES 6, IOCQES 4. */
#define CC_SHUTDOWN (CC_ENABLE|UINT32_C(0x4000)) /* SHN normal. */
#define AQA ((DEPTH-1)<<16|(DEPTH-1))
enum { FUNCTION,CHECK,CONFIGURE,ENABLING,IDENTIFY_CONTROLLER,IDENTIFY_NAMESPACE,
    QUEUES,CREATE_CQ,CREATE_SQ,READY,READ,DONE,DELETE_SQ,DELETE_CQ,SHUTDOWN,
    SHUTDOWN_WAIT,STOP,PORT,STOPPED };
enum { ASQ,ACQ,IOSQ,IOCQ,IDENTIFY,DATA,PAGES };
_Static_assert(PAGES*4096u==A733_PCIE_NVME_POOL_BYTES,"NVMe pool size");
static volatile uint32_t *page(const struct a733_pcie_nvme *n,unsigned index)
{return (volatile uint32_t *)(n->memory.cpu+(uintptr_t)index*4096);}
static uint64_t physical(const struct a733_pcie_nvme *n,unsigned index)
{return n->memory.physical+(uint64_t)index*4096;}
static uint8_t byte(const struct a733_pcie_nvme *n,unsigned at)
{return ((const volatile uint8_t *)page(n,IDENTIFY))[at];}
static uint32_t doorbell(const struct a733_pcie_nvme *n,unsigned queue,unsigned completion)
{return 0x1000+(2*queue+completion)*n->stride;}
static int rd(struct a733_pcie_nvme *n,uint32_t offset,uint32_t *v)
{return a733_pcie_function_read(&n->function,0,offset,v);}
static int wr(struct a733_pcie_nvme *n,uint32_t offset,uint32_t v)
{uint32_t got;return a733_pcie_function_write(&n->function,0,offset,v)&&rd(n,offset,&got)&&got==v;}
static void fail(struct a733_pcie_nvme *n)
{n->failed=1;if(n->lease.generation)(void)device_memory_retain(n->pool,n->lease);}
static void clear(struct a733_pcie_nvme *n,unsigned index)
{volatile uint32_t *p=page(n,index);for(unsigned i=0;i<1024;++i)p[i]=0;}
/* The one command each stage may submit. Returns its queue, or -1. */
static int build(const struct a733_pcie_nvme *n,uint32_t c[16])
{
    unsigned opcode,queue=0,prp=0;
    for(unsigned i=0;i<16;++i)c[i]=0;
    switch(n->stage){
    case IDENTIFY_CONTROLLER:opcode=6;prp=IDENTIFY;c[10]=1;break;
    case IDENTIFY_NAMESPACE:opcode=6;prp=IDENTIFY;c[1]=n->config.namespace_id;break;
    case QUEUES:opcode=9;c[10]=7;break; /* Number of Queues: one SQ and one CQ. */
    case CREATE_CQ:
        opcode=5;prp=IOCQ;c[10]=(DEPTH-1)<<16|1;
        c[11]=(n->function.config.irq!=A733_PCIE_IRQ_POLL?2u:0)|1u; /* Vector 0, contiguous. */
        break;
    case CREATE_SQ:opcode=1;prp=IOSQ;c[10]=(DEPTH-1)<<16|1;c[11]=1u<<16|1u;break;
    case READ:
        opcode=2;queue=1;prp=DATA;c[1]=n->config.namespace_id;
        c[10]=(uint32_t)n->lba;c[11]=(uint32_t)(n->lba>>32);
        c[12]=(A733_PCIE_NVME_BYTES>>n->identity.lba_shift)-1;
        break;
    case DELETE_SQ:opcode=0;c[10]=1;break;
    case DELETE_CQ:opcode=4;c[10]=1;break;
    default:return -1;
    }
    c[0]=opcode|(uint32_t)n->command_id<<16;
    if(prp){c[6]=(uint32_t)physical(n,prp);c[7]=(uint32_t)(physical(n,prp)>>32);}
    return (int)queue;
}
static int submit(struct a733_pcie_nvme *n,uint64_t now)
{
    uint32_t c[16];int queue=build(n,c);
    if(queue<0)return 0;
    volatile uint32_t *entry=page(n,queue?IOSQ:ASQ)+16*n->tail[queue];
    for(unsigned i=0;i<16;++i)entry[i]=c[i];
    n->queue=(unsigned)queue;
    uint16_t next=(uint16_t)((n->tail[queue]+1)%DEPTH);
    if(!a733_pcie_function_write(&n->function,0,doorbell(n,(unsigned)queue,0),next))return 0;
    n->tail[queue]=next;n->pending=1;n->deadline=now+n->config.timeout_us;return 1;
}
/* 1 complete, 0 pending, -1 fault. The controller-status read between the
 * phase observation and the entry reads supplies the native barriers. */
static int complete(struct a733_pcie_nvme *n,uint32_t *result)
{
    unsigned q=n->queue;volatile uint32_t *entry=page(n,q?IOCQ:ACQ)+4*n->head[q];
    uint32_t status;
    if(((entry[3]>>16)&1)!=n->phase[q])return 0;
    if(!rd(n,0x1c,&status)||(status&0xf)!=1)return -1;
    uint32_t dw0=entry[0],dw2=entry[2],dw3=entry[3];
    if((dw3&0xffff)!=n->command_id||(dw2>>16)!=q||(dw2&0xffff)!=n->tail[q]||(dw3>>17))return -1;
    uint16_t next=(uint16_t)((n->head[q]+1)%DEPTH);
    if(!a733_pcie_function_write(&n->function,0,doorbell(n,q,1),next))return -1;
    if(!next)n->phase[q]^=1;
    n->head[q]=next;n->pending=0;++n->command_id;*result=dw0;return 1;
}
static void text(const struct a733_pcie_nvme *n,char *out,unsigned at,unsigned bytes)
{for(unsigned i=0;i<bytes;++i)out[i]=(char)byte(n,at+i);}
static uint32_t word(const struct a733_pcie_nvme *n,unsigned at)
{return byte(n,at)|(uint32_t)byte(n,at+1)<<8|(uint32_t)byte(n,at+2)<<16|(uint32_t)byte(n,at+3)<<24;}
/* Record one admin result and choose the next stage. */
static int completed(struct a733_pcie_nvme *n)
{
    struct a733_pcie_nvme_identity *id=&n->identity;
    switch(n->stage){
    case IDENTIFY_CONTROLLER:
        id->vendor=(uint16_t)(byte(n,0)|byte(n,1)<<8);
        id->subsystem_vendor=(uint16_t)(byte(n,2)|byte(n,3)<<8);
        text(n,id->serial,4,20);text(n,id->model,24,40);text(n,id->firmware,64,8);
        id->mdts=byte(n,77);id->version=word(n,80);id->namespaces=word(n,516);
        if(id->vendor!=n->config.vendor||id->namespaces<n->config.namespace_id)return 0;
        clear(n,IDENTIFY);n->stage=IDENTIFY_NAMESPACE;return 1;
    case IDENTIFY_NAMESPACE: {
        /* The formatted LBA size must be 512..4096 bytes without metadata. */
        id->blocks=word(n,0)|(uint64_t)word(n,4)<<32;
        unsigned format=byte(n,26)&15;uint32_t lbaf=word(n,128+4*format);
        id->lba_shift=(lbaf>>16)&255;
        if(!id->blocks||format>byte(n,25)||(lbaf&0xffff)||id->lba_shift<9||id->lba_shift>12||
           id->blocks<(A733_PCIE_NVME_BYTES>>id->lba_shift))return 0;
        n->stage=QUEUES;return 1;
    }
    case QUEUES:n->stage=CREATE_CQ;return 1;
    case CREATE_CQ:n->stage=CREATE_SQ;return 1;
    case CREATE_SQ:n->stage=READY;return 1;
    case READ:n->stage=DONE;return 1;
    case DELETE_SQ:n->stage=DELETE_CQ;return 1;
    case DELETE_CQ:n->stage=SHUTDOWN;return 1;
    default:return 0;
    }
}
/* A cold controller with the NVM command set, 4 KiB pages and doorbells
 * inside BAR0. CAP.TO bounds enabling in 500 ms units. */
static int check(struct a733_pcie_nvme *n)
{
    uint32_t lo,hi,version,cc,status;
    if(!rd(n,0,&lo)||!rd(n,4,&hi)||!rd(n,8,&version)||!rd(n,0x14,&cc)||!rd(n,0x1c,&status))return 0;
    uint64_t cap=(uint64_t)hi<<32|lo;
    n->stride=UINT32_C(4)<<((cap>>32)&15);n->ready_us=((cap>>24)&255)*UINT64_C(500000);
    if(!(cap&0xffff)||!n->ready_us||!((cap>>37)&1)||((cap>>48)&15)||version<0x00010000u||cc||status||
       n->function.bars[0].bytes<0x1000+4*n->stride)return 0;
    n->stage=CONFIGURE;return 1;
}
/* Admin queues, then memory and bus mastering to the device, then enable. */
static int configure(struct a733_pcie_nvme *n,uint64_t now)
{
    uint64_t sq=physical(n,ASQ),cq=physical(n,ACQ);
    if(!wr(n,0x24,AQA)||!wr(n,0x28,(uint32_t)sq)||!wr(n,0x2c,(uint32_t)(sq>>32))||
       !wr(n,0x30,(uint32_t)cq)||!wr(n,0x34,(uint32_t)(cq>>32)))return 0;
    struct device_memory_allocation *a=device_memory_lookup(n->pool,n->lease);
    if(!a||a->state!=DEVICE_MEMORY_CPU)return 0;
    a->state=DEVICE_MEMORY_DEVICE;
    if(!a733_pcie_function_master(&n->function,1)||!a733_pcie_function_write(&n->function,0,0x14,CC_ENABLE))return 0;
    n->deadline=now+n->ready_us;n->stage=ENABLING;return 1;
}
static int step(struct a733_pcie_nvme *n)
{
    uint64_t now=n->function.port->last_now;uint32_t status,result;
    switch(n->stage){
    case CHECK:return check(n);
    case CONFIGURE:return configure(n,now);
    case ENABLING:
        if(!rd(n,0x1c,&status)||(status&2))return 0;
        if(status&1)n->stage=IDENTIFY_CONTROLLER;
        return (status&1)||now<n->deadline;
    case READY:case DONE:return rd(n,0x1c,&status)&&(status&0xf)==1;
    case SHUTDOWN:
        if(!a733_pcie_function_write(&n->function,0,0x14,CC_SHUTDOWN))return 0;
        n->deadline=now+n->config.timeout_us;n->stage=SHUTDOWN_WAIT;return 1;
    case SHUTDOWN_WAIT:
        /* Shutdown complete: no further queue or data DMA; then clear BME. */
        if(!rd(n,0x1c,&status)||(status&2))return 0;
        if(((status>>2)&3)!=2)return now<n->deadline;
        if(!a733_pcie_function_master(&n->function,0))return 0;
        n->stage=STOP;return 1;
    default:
        if(!n->pending)return submit(n,now);
        int r=complete(n,&result);
        if(r<0)return 0;
        if(!r)return now<n->deadline;
        return completed(n);
    }
}
int a733_pcie_nvme_init(struct a733_pcie_nvme *n,struct a733_pcie *p,
    const struct a733_pcie_nvme_config *c,struct device_memory_pool *pool,
    const struct a733_pcie_irq_ops *irq)
{
    if(!n||n->self||!p||p->self!=p||!c||!c->identity||!c->timeout_us||c->timeout_us>30000000||
       !c->namespace_id||c->namespace_id==UINT32_MAX||!pool||!pool->initialized||
       !a733_ranges_disjoint(n,sizeof(*n),c,sizeof(*c))||!a733_ranges_disjoint(n,sizeof(*n),pool,sizeof(*pool))||
       !a733_ranges_disjoint(n,sizeof(*n),p,sizeof(*p))||a733_pcie_output_outside(p,n,sizeof(*n))!=1)return 0;
    *n=(struct a733_pcie_nvme){.self=n,.pool=pool,.config=*c,.phase={1,1}};
    /* Queue bases and PRP entries carry 64 bits, but the root port's inbound
     * width is unknown: a pool above 4 GiB could reach another address. */
    if(!device_memory_acquire(pool,p->config.owner,A733_PCIE_NVME_POOL_BYTES,&n->lease,&n->memory)||
       n->memory.cpu!=n->memory.physical||
       !a733_extent_within(0,DMA_ADDRESS_32BIT_LIMIT,n->memory.physical,A733_PCIE_NVME_POOL_BYTES)){
        if(n->lease.generation)(void)device_memory_release(pool,n->lease);
        n->self=NULL;return 0;
    }
    const struct a733_pcie_function_config f={.identity=c->identity,.vendor=c->vendor,
        .device=c->device,.class_code=A733_PCIE_NVME_CLASS,.function=c->function,.irq=c->irq,
        .vectors=c->vectors,.write_allowed=a733_pcie_nvme_write_allowed,.driver=n};
    if(!a733_pcie_function_init(&n->function,p,&f,pool,irq)){
        (void)device_memory_release(pool,n->lease);n->self=NULL;return 0;
    }
    for(unsigned i=0;i<PAGES;++i)clear(n,i);
    return 1;
}
enum a733_pcie_result a733_pcie_nvme_poll(struct a733_pcie_nvme *n)
{
    if(!n||n->self!=n)return A733_PCIE_INVALID;
    if(n->failed)return A733_PCIE_RETAINED;
    if(n->stage==STOPPED)return A733_PCIE_STOPPED;
    struct a733_pcie_function *f=&n->function;struct a733_pcie *p=f->port;
    enum a733_pcie_result r;
    if(n->stage==FUNCTION||n->stage==STOP){
        r=a733_pcie_function_poll(f);
        if(r==A733_PCIE_READY&&n->stage==FUNCTION)n->stage=CHECK;
        else if(r==A733_PCIE_STOPPED&&n->stage==STOP)n->stage=PORT;
        else if(r!=A733_PCIE_PENDING&&r!=A733_PCIE_BUSY){fail(n);return A733_PCIE_RETAINED;}
        return A733_PCIE_PENDING;
    }
    if(n->stage==PORT){
        r=a733_pcie_stop(p,p->config.lifetime_deadline);
        if(r==A733_PCIE_STOPPED){
            (void)device_memory_retain(n->pool,n->lease);n->stage=STOPPED;return A733_PCIE_STOPPED;
        }
        if(r!=A733_PCIE_PENDING&&r!=A733_PCIE_BUSY){fail(n);return A733_PCIE_RETAINED;}
        return A733_PCIE_PENDING;
    }
    uint32_t events=0;
    r=a733_pcie_function_begin(f,p->config.lifetime_deadline,&events);
    if(r!=A733_PCIE_READY){
        if(r!=A733_PCIE_BUSY&&r!=A733_PCIE_PENDING){fail(n);return A733_PCIE_RETAINED;}
        return A733_PCIE_PENDING;
    }
    n->events|=events;
    r=a733_pcie_function_end(f,step(n));
    if(r!=A733_PCIE_READY&&r!=A733_PCIE_BUSY){fail(n);return A733_PCIE_RETAINED;}
    if(n->stage==STOP&&!a733_pcie_function_stop(f)){fail(n);return A733_PCIE_RETAINED;}
    return n->stage==READY||n->stage==DONE?A733_PCIE_READY:A733_PCIE_PENDING;
}
enum a733_pcie_result a733_pcie_nvme_identity(struct a733_pcie_nvme *n,struct a733_pcie_nvme_identity *out)
{
    if(!n||n->self!=n||!out||!a733_ranges_disjoint(out,sizeof(*out),n,sizeof(*n))||
       a733_pcie_output_outside(n->function.port,out,sizeof(*out))!=1)return A733_PCIE_INVALID;
    if(n->failed)return A733_PCIE_RETAINED;
    if(n->stage!=READY&&n->stage!=DONE)return A733_PCIE_PENDING;
    *out=n->identity;return A733_PCIE_READY;
}
int a733_pcie_nvme_read(struct a733_pcie_nvme *n,uint64_t lba)
{
    if(!n||n->self!=n||n->failed||n->stage!=READY)return 0;
    uint64_t blocks=A733_PCIE_NVME_BYTES>>n->identity.lba_shift;
    if(lba>n->identity.blocks-blocks)return 0;
    clear(n,DATA);n->lba=lba;n->stage=READ;return 1;
}
enum a733_pcie_result a733_pcie_nvme_copy(struct a733_pcie_nvme *n,uint8_t out[4096])
{
    if(!n||n->self!=n||!out||!a733_ranges_disjoint(out,4096,n,sizeof(*n))||
       !a733_ranges_disjoint(out,4096,n->pool,sizeof(*n->pool))||
       !a733_ranges_disjoint(out,4096,(void *)n->pool->backing.cpu,n->pool->backing.bytes)||
       a733_pcie_output_outside(n->function.port,out,4096)!=1)return A733_PCIE_INVALID;
    if(n->failed)return A733_PCIE_RETAINED;
    if(n->stage!=DONE)return A733_PCIE_PENDING;
    const volatile uint8_t *data=(const volatile uint8_t *)page(n,DATA);
    for(unsigned i=0;i<A733_PCIE_NVME_BYTES;++i)out[i]=data[i];
    n->stage=READY;return A733_PCIE_READY;
}
int a733_pcie_nvme_shutdown(struct a733_pcie_nvme *n)
{
    if(!n||n->self!=n||n->failed||(n->stage!=READY&&n->stage!=DONE))return 0;
    n->stage=DELETE_SQ;return 1;
}
int a733_pcie_nvme_write_allowed(const void *driver,unsigned bar,uint32_t offset,uint32_t v)
{
    const struct a733_pcie_nvme *n=driver;
    if(!n||n->self!=n||n->failed||bar||!n->stride)return 0;
    uint64_t sq=physical(n,ASQ),cq=physical(n,ACQ);
    switch(offset){
    case 0x14:return (n->stage==CONFIGURE&&v==CC_ENABLE)||(n->stage==SHUTDOWN&&v==CC_SHUTDOWN);
    case 0x24:return n->stage==CONFIGURE&&v==AQA;
    case 0x28:return n->stage==CONFIGURE&&v==(uint32_t)sq;
    case 0x2c:return n->stage==CONFIGURE&&v==(uint32_t)(sq>>32);
    case 0x30:return n->stage==CONFIGURE&&v==(uint32_t)cq;
    case 0x34:return n->stage==CONFIGURE&&v==(uint32_t)(cq>>32);
    default:break;
    }
    for(unsigned q=0;q<2;++q){
        if(offset==doorbell(n,q,0)){
            /* The queued entry must equal the one command of this stage. */
            uint32_t c[16];const volatile uint32_t *entry=page(n,q?IOSQ:ASQ)+16*n->tail[q];
            if(n->pending||build(n,c)!=(int)q||v!=(n->tail[q]+1u)%DEPTH)return 0;
            for(unsigned i=0;i<16;++i)if(entry[i]!=c[i])return 0;
            return 1;
        }
        if(offset==doorbell(n,q,1))return n->pending&&n->queue==q&&v==(n->head[q]+1u)%DEPTH;
    }
    return 0;
}
