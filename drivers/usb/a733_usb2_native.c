/* SPDX-License-Identifier: GPL-2.0-only */
#include "a733_usb2_native.h"
#include "a733_usb3_phy.h"
#include "a733_platform_locks.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include "a733_range.h"
#include "a733_task_context.h"
#include "task.h"
#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1
#error "USB2 requires scheduler, current-task and mutex-owner APIs"
#endif
static int current(void *cookie)
{struct a733_usb2_native *n=cookie;return n&&n->self==n&&n->initialized&&a733_task_context_owner_el1((TaskHandle_t)n->owner_task);}
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
    struct a733_usb2_native *n=cookie;if(!v||!held(n)||!(a733_usb2_platform_address(a,0)||(n->platform.config.superspeed&&(a733_usb3_mask32(a)||a==0x06c01900))))return 0;
    a733_mmio_dsb_sy();*v=a733_mmio_read32(a);
    a733_mmio_dsb_sy();return 1;
}
static int write(void *cookie,uintptr_t a,uint32_t v)
{
    struct a733_usb2_native *n=cookie;uint32_t mask=a733_usb2_platform_mask(a);
    if(n->platform.config.superspeed&&a733_usb3_mask32(a))mask=a733_usb3_mask32(a);
    if(!mask||!held(n)||!(a733_usb2_platform_address(a,1)||(n->platform.config.superspeed&&a733_usb3_mask32(a))))return 0;
    a733_mmio_dsb_sy();uint32_t old=a733_mmio_read32(a);
    if((v^old)&~mask)return 0;a733_mmio_write32_dsb_sy_after(a,v);return 1;
}
static int read16(void *cookie,uintptr_t a,uint16_t *v)
{
    struct a733_usb2_native *n=cookie;
    if(!v||!held(n)||!n->platform.config.superspeed||!a733_usb3_address16(a))return 0;
    a733_mmio_dsb_sy();*v=a733_mmio_read16(a);
    a733_mmio_dsb_sy();return 1;
}
static int write16(void *cookie,uintptr_t a,uint16_t v)
{
    struct a733_usb2_native *n=cookie;
    if(!held(n)||!n->platform.config.superspeed||!a733_usb3_address16(a))return 0;
    a733_mmio_write16_dsb_sy_both(a,v);return a733_mmio_read16(a)==v;
}
/* The embedded platform itself is an intentional constructor input. Other
 * caller buffers must exclude every other native field and the DMA parent. */
static int platform_outside(void *cookie,const void *p,size_t z)
{
    struct a733_usb2_native *n=cookie;
    return a733_ranges_disjoint(n,offsetof(struct a733_usb2_native,platform),p,z)&&
        a733_ranges_disjoint((uint8_t *)n+offsetof(struct a733_usb2_native,platform_ops),
            sizeof(*n)-offsetof(struct a733_usb2_native,platform_ops),p,z)&&
        a733_ranges_disjoint(n->locks,sizeof(*n->locks),p,z)&&a733_ranges_disjoint(n->parent_pool,sizeof(*n->parent_pool),p,z)&&
        (!n->backing.bytes||a733_ranges_disjoint((void *)n->backing.cpu,n->backing.bytes,p,z));
}
static int core_outside(void *cookie,const void *p,size_t z)
{
    struct a733_usb2_native *n=cookie;
    /* TinyUSB and the application sector are CPU-only copied buffers. */
    int own_sector=p==n->sector&&z<=sizeof(n->sector);
    int own_core=p==&n->core&&z==sizeof(n->core);
    int own_dma=p==(void *)n->backing.cpu&&z==n->backing.bytes;
    return (own_sector||own_core||own_dma||a733_ranges_disjoint(n,sizeof(*n),p,z))&&
        a733_ranges_disjoint(n->locks,sizeof(*n->locks),p,z)&&a733_ranges_disjoint(n->parent_pool,sizeof(*n->parent_pool),p,z)&&
        (own_dma||a733_ranges_disjoint((void *)n->backing.cpu,n->backing.bytes,p,z))&&
        a733_ranges_disjoint(n->platform.clocks,sizeof(*n->platform.clocks),p,z)&&
        a733_ranges_disjoint(n->platform.rails,sizeof(*n->platform.rails),p,z)&&
        a733_ranges_disjoint(n->platform.pmic,sizeof(*n->platform.pmic),p,z)&&
        a733_ranges_disjoint(n->platform.pck,sizeof(*n->platform.pck),p,z)&&
        a733_ranges_disjoint(n->platform.typec,sizeof(*n->platform.typec),p,z)&&
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
    a733_mmio_dsb_sy();*v=a733_mmio_read32(0x06a00000u+off);
    a733_mmio_dsb_sy();return 1;
}
static int write32(void *cookie,uint32_t off,uint32_t v)
{
    struct a733_usb2_native *n=cookie;if(!core_allowed(n,off,4))return 0;
    struct a733_xhci *h=&n->core;unsigned op=h->operational,rt=h->runtime,db=h->doorbell;
    int allowed=op&&(off==op||off==op+4||off==op+0x38||
        (h->usb2_port&&off==op+0x400+16*(h->usb2_port-1))||
        ((h->require_ss||h->companion_root)&&h->usb2_companion_port&&off==op+0x400+16*(h->usb2_companion_port-1)));
    allowed|=rt&&(off==rt+0x20||off==rt+0x24||off==rt+0x28);
    allowed|=db&&off==db;
    for(unsigned i=0;i<A733_XHCI_DEVICES;++i)
        allowed|=db&&h->devices[i].slot&&off==db+4*h->devices[i].slot;
    if(!allowed)return 0;
    a733_mmio_write32_dsb_sy_both(0x06a00000u+off,v);return 1;
}
static int write64(void *cookie,uint32_t off,uint64_t v)
{
    struct a733_usb2_native *n=cookie;if(!core_allowed(n,off,8))return 0;
    struct a733_xhci *h=&n->core;
    if(!((h->operational&&(off==h->operational+0x18||off==h->operational+0x30))||
        (h->runtime&&(off==h->runtime+0x30||off==h->runtime+0x38))))return 0;
    a733_mmio_write64_dsb_sy_both(0x06a00000u+off,v);return 1;
}
static int barrier(void *cookie)
{if(!current(cookie))return 0;a733_mmio_dsb_sy();return 1;}
static int mappings(uintptr_t a,size_t bytes,unsigned attr)
{
    /* Identity write translations every 4 KiB; Normal-NC must be outer or inner shareable. */
    const struct a733_mapping_rule rule={A733_MAPPING_WRITE,1,
        {{(uint8_t)attr,attr==A733_MAIR_NORMAL_NC?A733_SH_OUTER|A733_SH_INNER:A733_SH_ANY}},0,1};
    return a733_mapping_check_stride(aarch64_device_translate,a,a,bytes,4096,&rule);
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
        !a733_ranges_disjoint(n,sizeof(*n),c,sizeof(*c))||!a733_ranges_disjoint(n,sizeof(*n),locks,sizeof(*locks))||
        !a733_ranges_disjoint(n,sizeof(*n),pool,sizeof(*pool))||!clocks||!rails||!pmic||!pck||!typec||
        !clocks->initialized||rails->self!=rails||pmic->self!=pmic||!pck->initialized||typec->self!=typec||
        !a733_ranges_disjoint(n,sizeof(*n),clocks,sizeof(*clocks))||!a733_ranges_disjoint(n,sizeof(*n),rails,sizeof(*rails))||
        !a733_ranges_disjoint(n,sizeof(*n),pmic,sizeof(*pmic))||!a733_ranges_disjoint(n,sizeof(*n),pck,sizeof(*pck))||
        !a733_ranges_disjoint(n,sizeof(*n),typec,sizeof(*typec))||
        pmic->ops.outside(pmic->cookie,n,sizeof(*n))!=1||
        typec->config.ops.outside(typec->config.context,n,sizeof(*n))!=1)return 0;
    static const uintptr_t pages[]={0x02002000,0x02003000,0x03400000,0x06b00000,0x06c00000,
        0x07010000,0x07067000,0x07068000,0x07090000,0x06a0c000};
    for(unsigned i=0;i<sizeof(pages)/sizeof(pages[0]);++i)if(!mappings(pages[i],4096,0))return 0;
    if(!mappings(0x06a00000,0x8000,0))return 0;
    if(c->superspeed&&(!mappings(0x06c01000,4096,0)||!mappings(0x06c06000,8192,0)||
       !mappings(0x06c80000,0x20000,0)||!mappings(0x06a0d000,4096,0)))return 0;
    *n=(struct a733_usb2_native){.self=n,.locks=locks,.owner_task=task,.parent_pool=pool,
        .admit=admission,.admission_context=context};
    if(!device_memory_acquire(pool,c->owner,A733_XHCI_MEMORY_BYTES,&n->parent,&n->backing))return 0;
    if(!mappings(n->backing.cpu,n->backing.bytes,0x44)||n->backing.cpu!=n->backing.physical||
        !a733_ranges_disjoint(n,sizeof(*n),(void *)n->backing.cpu,n->backing.bytes)||
        !device_memory_pool_init(&n->private_pool,n->backing.cpu,n->backing.physical,n->backing.bytes)||
        !device_memory_acquire(&n->private_pool,c->owner,n->backing.bytes,&n->memory,&n->backing)){
        device_memory_retain(pool,n->parent);return 0;}
    struct device_memory_allocation *a=device_memory_lookup(pool,n->parent);
    if(!a||a->state!=DEVICE_MEMORY_CPU){device_memory_retain(pool,n->parent);return 0;}
    a->state=DEVICE_MEMORY_DEVICE;
    n->platform_ops=(struct a733_usb2_platform_ops){now,current,admit,enter,leave,retain,read,write,platform_outside,read16,write16};
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
    n->core.require_ss=n->platform.config.superspeed&&!n->platform.config.companion_root;
    n->core.companion_root=n->platform.config.companion_root;
    if(!n->stack_started){if(!a733_usb2_host_bind(n)){a733_usb2_native_lost(n);return A733_USB2_RETAINED;}n->stack_started=1;}
    int r=a733_usb2_host_poll(n);if(r<0){a733_usb2_native_lost(n);return A733_USB2_RETAINED;}
    return n->mounted?A733_USB2_READY:A733_USB2_PENDING;
}
