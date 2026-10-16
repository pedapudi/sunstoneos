#include "a733_mmc_platform_native.h"
#include "a733_platform_locks.h"
#include "task.h"
#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1
#error "MMC native binding requires scheduler-state, task and mutex-owner APIs"
#endif
static int current(void *cookie)
{
    struct a733_mmc_platform_native *n=cookie;
    uint64_t el,flags,nesting,critical;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, daif" : "=r"(el),"=r"(flags));
    if (!n || !n->initialized || el!=4 || (flags&0x80)) return 0;
    /* Read the port's task/interrupt state with the same short IRQ exclusion
     * used by the shared lock implementation. Never admit ISR or critical code. */
    __asm__ volatile("msr daifset, #2" ::: "memory");
#if configNUMBER_OF_CORES > 1
    extern volatile uint64_t ullPortInterruptNestings[],ullCriticalNestings[];
    uint64_t core;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(core));
    nesting=core<configNUMBER_OF_CORES ? ullPortInterruptNestings[core] : 1;
    critical=core<configNUMBER_OF_CORES ? ullCriticalNestings[core] : 1;
#else
    extern uint64_t ullPortInterruptNesting;
    extern volatile uint64_t ullCriticalNesting;
    nesting=ullPortInterruptNesting; critical=ullCriticalNesting;
#endif
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
    return !nesting && !critical && xTaskGetSchedulerState()==taskSCHEDULER_RUNNING &&
        xTaskGetCurrentTaskHandle()==(TaskHandle_t)n->owner_task;
}

static int now(void *cookie,uint64_t *v)
{
    struct a733_mmc_platform_native *n=cookie;
    return n->locks->now(n->locks->context,v);
}
static int admit(void *cookie,const struct a733_mmc_platform_config *c,const struct a733_clock_token *t)
{
    struct a733_mmc_platform_native *n=cookie;
    return c->kind==n->kind&&current(n) ? n->admit(n->admission_cookie,c,t) : -1;
}
static int enter(void *cookie,uint64_t deadline)
{
    struct a733_mmc_platform_native *n=cookie;
    return current(n)&&a733_platform_lock_enter(n->locks,A733_PLATFORM_REGISTER_LOCK,deadline);
}
static void leave(void *cookie)
{
    struct a733_mmc_platform_native *n=cookie;
    a733_platform_lock_leave(n->locks,A733_PLATFORM_REGISTER_LOCK);
}
static void retain(void *cookie)
{
    struct a733_mmc_platform_native *n=cookie;
    configASSERT(a733_platform_lock_retain(n->locks,A733_PLATFORM_REGISTER_LOCK)==1);
}
static int allowed(struct a733_mmc_platform_native *n,uintptr_t a,unsigned writing)
{
    if(!current(n)||!a733_mmc_platform_native_address(n->kind,a,writing))return 0;
    return !(atomic_load_explicit(&n->locks->retained,memory_order_acquire)&1u)&&
        xSemaphoreGetMutexHolder(n->locks->mutex[A733_PLATFORM_REGISTER_LOCK])==(TaskHandle_t)n->owner_task;
}
static int read32(void *cookie,uintptr_t a,uint32_t *v)
{
    struct a733_mmc_platform_native *n=cookie;
    if(!v||!allowed(n,a,0))return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    *v=*(volatile const uint32_t *)a;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
static int write32(void *cookie,uintptr_t a,uint32_t v)
{
    struct a733_mmc_platform_native *n=cookie;
    if(!allowed(n,a,1))return 0;
    uintptr_t base=n->kind==SUNXI_MMC_SD?0x04020000:0x04022000;
    uintptr_t module=n->kind==SUNXI_MMC_SD?0x02002d00:0x02002d20;
    if(a==0x03400198) {
        if(v!=(n->kind==SUNXI_MMC_SD?2u:8u))return 0;
    } else if(a==module||a==module+0xc) {
        uint32_t mask=a==module?UINT32_C(0x87001f1f):UINT32_C(0x10001);
        if(a==module&&(v&(mask&~UINT32_C(0x80000000)))!=29)return 0;
        __asm__ volatile("dsb sy" ::: "memory");
        uint32_t old=*(volatile const uint32_t *)a;
        if((old^v)&~mask)return 0;
    } else if(a==base+0x18) {
        if(v&(1u<<10))return 0; /* Never enable data writes. */
        if(!(v&(1u<<21)))switch(v&63) {
        case 0:case 1:case 2:case 3:case 7:case 8:case 9:case 13:
        case 16:case 17:case 18:case 41:case 55:break;
        default:return 0;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)a=v;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
static int barrier(void *cookie)
{
    if(!current(cookie))return 0;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);
}
static int outside(void *cookie,const void *v,size_t bytes)
{
    struct a733_mmc_platform_native *n=cookie;
    return n&&separate(n,sizeof(*n),v,bytes)&&separate(n->locks,sizeof(*n->locks),v,bytes);
}
int a733_mmc_platform_native_init(struct a733_mmc_platform_native *n,
    struct a733_platform_locks *locks,void *task,enum sunxi_mmc_kind kind,
    int (*admission)(void *,const struct a733_mmc_platform_config *,const struct a733_clock_token *),void *cookie)
{
    if(!n||n->initialized||!locks||!locks->initialized||!task||!admission||
       (kind!=SUNXI_MMC_SD&&kind!=SUNXI_MMC_EMMC))return 0;
    n->kind=kind;n->locks=locks;n->owner_task=task;n->admit=admission;n->admission_cookie=cookie;
    n->operations=(struct a733_mmc_platform_ops){now,current,admit,enter,leave,retain,read32,write32,barrier,outside};
    n->initialized=1;return 1;
}
