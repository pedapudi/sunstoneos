/* SPDX-License-Identifier: GPL-2.0-only */
#include "a733_usb2_native.h"
#include "a733_platform_locks.h"
#include "aarch64_device_memory.h"
#include "task.h"
#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1
#error "USB2 requires scheduler, current-task and mutex-owner APIs"
#endif
static int separate(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);}
static int current(void *cookie)
{
    struct a733_usb2_native *n=cookie;uint64_t el,flags,nesting,critical;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, daif":"=r"(el),"=r"(flags));
    if(!n||n->self!=n||!n->initialized||el!=4||(flags&0x80))return 0;
    __asm__ volatile("msr daifset, #2":::"memory");
#if configNUMBER_OF_CORES > 1
    extern volatile uint64_t ullPortInterruptNestings[],ullCriticalNestings[];
    uint64_t core;__asm__ volatile("mrs %0, tpidr_el1":"=r"(core));
    nesting=core<configNUMBER_OF_CORES?ullPortInterruptNestings[core]:1;
    critical=core<configNUMBER_OF_CORES?ullCriticalNestings[core]:1;
#else
    extern uint64_t ullPortInterruptNesting;
    extern volatile uint64_t ullCriticalNesting;
    nesting=ullPortInterruptNesting;critical=ullCriticalNesting;
#endif
    __asm__ volatile("msr daif, %0"::"r"(flags):"memory");
    return !nesting&&!critical&&xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&
        xTaskGetCurrentTaskHandle()==(TaskHandle_t)n->owner_task;
}
static int now(void *cookie,uint64_t *value)
{struct a733_usb2_native *n=cookie;return n->locks->now(n->locks->context,value);}
static int admit(void *cookie,const struct a733_usb2_platform_config *c)
{struct a733_usb2_native *n=cookie;return current(n)&&c==&n->platform.config?n->admit(n->admission_context,c):-1;}
static int enter(void *cookie,uint64_t d)
{struct a733_usb2_native *n=cookie;return current(n)?a733_platform_lock_enter(n->locks,A733_PLATFORM_REGISTER_LOCK,d):-1;}
static void leave(void *cookie)
{struct a733_usb2_native *n=cookie;a733_platform_lock_leave(n->locks,A733_PLATFORM_REGISTER_LOCK);}
static void retain(void *cookie)
{struct a733_usb2_native *n=cookie;configASSERT(a733_platform_lock_retain(n->locks,A733_PLATFORM_REGISTER_LOCK)==1);}
static int held(struct a733_usb2_native *n)
{return current(n)&&!(atomic_load_explicit(&n->locks->retained,memory_order_acquire)&1)&&
    xSemaphoreGetMutexHolder(n->locks->mutex[A733_PLATFORM_REGISTER_LOCK])==(TaskHandle_t)n->owner_task;}
static int read(void *cookie,uintptr_t a,uint32_t *v)
{
    struct a733_usb2_native *n=cookie;if(!v||!held(n)||!a733_usb2_platform_address(a,0))return 0;
    __asm__ volatile("dsb sy":::"memory");*v=*(volatile const uint32_t *)a;
    __asm__ volatile("dsb sy":::"memory");return 1;
}
static int write(void *cookie,uintptr_t a,uint32_t v)
{
    struct a733_usb2_native *n=cookie;uint32_t mask=a733_usb2_platform_mask(a);
    if(!mask||!held(n)||!a733_usb2_platform_address(a,1))return 0;
    __asm__ volatile("dsb sy":::"memory");uint32_t old=*(volatile const uint32_t *)a;
    if((v^old)&~mask)return 0;*(volatile uint32_t *)a=v;
    __asm__ volatile("dsb sy":::"memory");return 1;
}
/* The embedded platform itself is an intentional constructor input. Other
 * caller buffers must exclude every other native field and the DMA parent. */
static int platform_outside(void *cookie,const void *p,size_t z)
{
    struct a733_usb2_native *n=cookie;
    return separate(n,offsetof(struct a733_usb2_native,platform),p,z)&&
        separate((uint8_t *)n+offsetof(struct a733_usb2_native,platform_ops),
            sizeof(*n)-offsetof(struct a733_usb2_native,platform_ops),p,z)&&
        separate(n->locks,sizeof(*n->locks),p,z)&&separate(n->parent_pool,sizeof(*n->parent_pool),p,z)&&
        (!n->backing.bytes||separate((void *)n->backing.cpu,n->backing.bytes,p,z));
}
static int core_outside(void *cookie,const void *p,size_t z)
{
    struct a733_usb2_native *n=cookie;
    /* TinyUSB and the application sector are CPU-only copied buffers. */
    int own_sector=p==n->sector&&z<=sizeof(n->sector);
    int own_core=p==&n->core&&z==sizeof(n->core);
    int own_dma=p==(void *)n->backing.cpu&&z==n->backing.bytes;
    return (own_sector||own_core||own_dma||separate(n,sizeof(*n),p,z))&&
        separate(n->locks,sizeof(*n->locks),p,z)&&separate(n->parent_pool,sizeof(*n->parent_pool),p,z)&&
        (own_dma||separate((void *)n->backing.cpu,n->backing.bytes,p,z))&&
        separate(n->platform.clocks,sizeof(*n->platform.clocks),p,z)&&
        separate(n->platform.rails,sizeof(*n->platform.rails),p,z)&&
        separate(n->platform.pmic,sizeof(*n->platform.pmic),p,z)&&
        separate(n->platform.pck,sizeof(*n->platform.pck),p,z)&&
        separate(n->platform.typec,sizeof(*n->platform.typec),p,z)&&
        n->platform.pmic->ops.outside(n->platform.pmic->cookie,p,z)==1&&
        n->platform.typec->config.ops.outside(n->platform.typec->config.context,p,z)==1;
}
static int live(void *cookie,uint64_t generation,uint64_t d)
{struct a733_usb2_native *n=cookie;return current(n)&&!n->failed?a733_usb2_platform_live(&n->platform,generation,d):-1;}
static int core_allowed(struct a733_usb2_native *n,uint32_t off,unsigned w)
{return current(n)&&n->core_started&&!n->failed&&!n->platform.failed&&off<=0x8000-w&&!(off&(w-1));}
static int read32(void *cookie,uint32_t off,uint32_t *v)
{
    struct a733_usb2_native *n=cookie;if(!v||!core_allowed(n,off,4))return 0;
    __asm__ volatile("dsb sy":::"memory");*v=*(volatile const uint32_t *)(uintptr_t)(0x06a00000u+off);
    __asm__ volatile("dsb sy":::"memory");return 1;
}
static int write32(void *cookie,uint32_t off,uint32_t v)
{
    struct a733_usb2_native *n=cookie;if(!core_allowed(n,off,4))return 0;
    struct a733_xhci *h=&n->core;unsigned op=h->operational,rt=h->runtime,db=h->doorbell;
    int allowed=op&&(off==op||off==op+4||off==op+0x38||
        (h->usb2_port&&off==op+0x400+16*(h->usb2_port-1)));
    allowed|=rt&&(off==rt+0x20||off==rt+0x24||off==rt+0x28);
    allowed|=db&&(off==db||(h->slot&&off==db+4*h->slot));
    if(!allowed)return 0;
    __asm__ volatile("dsb sy":::"memory");*(volatile uint32_t *)(uintptr_t)(0x06a00000u+off)=v;
    __asm__ volatile("dsb sy":::"memory");return 1;
}
static int write64(void *cookie,uint32_t off,uint64_t v)
{
    struct a733_usb2_native *n=cookie;if(!core_allowed(n,off,8))return 0;
    struct a733_xhci *h=&n->core;
    if(!((h->operational&&(off==h->operational+0x18||off==h->operational+0x30))||
        (h->runtime&&(off==h->runtime+0x30||off==h->runtime+0x38))))return 0;
    __asm__ volatile("dsb sy":::"memory");*(volatile uint64_t *)(uintptr_t)(0x06a00000u+off)=v;
    __asm__ volatile("dsb sy":::"memory");return 1;
}
static int barrier(void *cookie)
{if(!current(cookie))return 0;__asm__ volatile("dsb sy":::"memory");return 1;}
static int mappings(uintptr_t a,size_t bytes,unsigned attr)
{
    if(!bytes||bytes>UINTPTR_MAX-a)return 0;
    for(size_t i=0;i<bytes;i+=4096){struct aarch64_translation t;
        if(!aarch64_device_translate(a+i,1,&t)||t.physical!=a+i||t.attributes!=attr||
            (attr==0x44&&t.shareability!=2&&t.shareability!=3))return 0;}
    return 1;
}
int a733_usb2_native_init(struct a733_usb2_native *n,const struct a733_usb2_platform_config *c,
    struct a733_platform_locks *locks,void *task,struct device_memory_pool *pool,
    struct a733_clock_leases *clocks,struct a733_rail_leases *rails,struct a733_pmic *pmic,
    struct a733_gpu_resource_manager *pck,struct a733_typec *typec,
    int (*admission)(void *,const struct a733_usb2_platform_config *),void *context)
{
    uint64_t el,ctl;__asm__ volatile("mrs %0, CurrentEL":"=r"(el));if(el!=4)return 0;
    __asm__ volatile("mrs %0, sctlr_el1":"=r"(ctl)); /* EL2/stage2 exclusion is immutable boot authority. */
    if(!n||n->self||!c||!locks||!locks->initialized||!task||!pool||!pool->initialized||!admission||
        (ctl&5)!=5||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||
        !separate(n,sizeof(*n),c,sizeof(*c))||!separate(n,sizeof(*n),locks,sizeof(*locks))||
        !separate(n,sizeof(*n),pool,sizeof(*pool))||!clocks||!rails||!pmic||!pck||!typec||
        !clocks->initialized||rails->self!=rails||pmic->self!=pmic||!pck->initialized||typec->self!=typec||
        !separate(n,sizeof(*n),clocks,sizeof(*clocks))||!separate(n,sizeof(*n),rails,sizeof(*rails))||
        !separate(n,sizeof(*n),pmic,sizeof(*pmic))||!separate(n,sizeof(*n),pck,sizeof(*pck))||
        !separate(n,sizeof(*n),typec,sizeof(*typec))||
        pmic->ops.outside(pmic->cookie,n,sizeof(*n))!=1||
        typec->config.ops.outside(typec->config.context,n,sizeof(*n))!=1)return 0;
    static const uintptr_t pages[]={0x02002000,0x02003000,0x03400000,0x06b00000,0x06c00000,
        0x07010000,0x07067000,0x07068000,0x07090000,0x06a0c000};
    for(unsigned i=0;i<sizeof(pages)/sizeof(pages[0]);++i)if(!mappings(pages[i],4096,0))return 0;
    if(!mappings(0x06a00000,0x8000,0))return 0;
    *n=(struct a733_usb2_native){.self=n,.locks=locks,.owner_task=task,.parent_pool=pool,
        .admit=admission,.admission_context=context};
    if(!device_memory_acquire(pool,c->owner,A733_XHCI_MEMORY_BYTES,&n->parent,&n->backing))return 0;
    if(!mappings(n->backing.cpu,n->backing.bytes,0x44)||n->backing.cpu!=n->backing.physical||
        !separate(n,sizeof(*n),(void *)n->backing.cpu,n->backing.bytes)||
        !device_memory_pool_init(&n->private_pool,n->backing.cpu,n->backing.physical,n->backing.bytes)||
        !device_memory_acquire(&n->private_pool,c->owner,n->backing.bytes,&n->memory,&n->backing)){
        device_memory_retain(pool,n->parent);return 0;}
    struct device_memory_allocation *a=device_memory_lookup(pool,n->parent);
    if(!a||a->state!=DEVICE_MEMORY_CPU){device_memory_retain(pool,n->parent);return 0;}
    a->state=DEVICE_MEMORY_DEVICE;
    n->platform_ops=(struct a733_usb2_platform_ops){now,current,admit,enter,leave,retain,read,write,platform_outside};
    n->core_ops=(struct a733_xhci_ops){now,current,live,read32,write32,write64,barrier,core_outside,a733_usb2_host_notice};
    if(!a733_usb2_platform_init(&n->platform,c,&n->platform_ops,n,clocks,rails,pmic,pck,typec)){
        device_memory_retain(pool,n->parent);return 0;}
    n->initialized=1;return 1;
}
void a733_usb2_native_lost(struct a733_usb2_native *n)
{
    if(!current(n))return;n->failed=1;
    if(n->core_started)a733_xhci_lost(&n->core);
    a733_usb2_platform_lost(&n->platform);n->read_failed=1;
}
enum a733_usb2_result a733_usb2_native_poll(struct a733_usb2_native *n)
{
    if(!current(n))return A733_USB2_INVALID;if(n->failed)return A733_USB2_RETAINED;
    if(!n->core_started){enum a733_usb2_result r=a733_usb2_platform_poll(&n->platform);
        if(r!=A733_USB2_READY)return r;
        n->core_started=1;
        if(!a733_xhci_init(&n->core,&n->core_ops,n,&n->private_pool,n->memory,
            n->platform.attachment.generation,n->platform.config.deadline,n->platform.config.lifetime_deadline)){
            a733_usb2_native_lost(n);return A733_USB2_RETAINED;}}
    if(!n->stack_started){if(!a733_usb2_host_bind(n)){a733_usb2_native_lost(n);return A733_USB2_RETAINED;}n->stack_started=1;}
    int r=a733_usb2_host_poll(n);if(r<0){a733_usb2_native_lost(n);return A733_USB2_RETAINED;}
    return n->mounted?A733_USB2_READY:A733_USB2_PENDING;
}
