#include "a733_uart_platform_native.h"
#include "a733_platform_locks.h"
#include "task.h"

#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1
#error "UART native binding requires scheduler-state, task and mutex-owner APIs"
#endif
static int current(void *cookie)
{
    struct a733_uart_platform_native *n=cookie;
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
    struct a733_uart_platform_native *n=cookie;
    return n->locks->now(n->locks->context,v);
}
static int authorize(void *cookie,const struct a733_uart_platform_config *c,
    const struct a733_clock_token *t,enum a733_uart_admission phase)
{
    struct a733_uart_platform_native *n=cookie;
    if (!current(n) || c->format.port!=n->port) return -1;
    return n->admit(n->admission_cookie,c,t,phase);
}
static int enter(void *cookie,uint64_t deadline)
{
    struct a733_uart_platform_native *n=cookie;
    return current(n) && a733_platform_lock_enter(n->locks,A733_PLATFORM_REGISTER_LOCK,deadline);
}
static void leave(void *cookie)
{
    struct a733_uart_platform_native *n=cookie;
    a733_platform_lock_leave(n->locks,A733_PLATFORM_REGISTER_LOCK);
}
static void retain(void *cookie)
{
    struct a733_uart_platform_native *n=cookie;
    configASSERT(a733_platform_lock_retain(n->locks,A733_PLATFORM_REGISTER_LOCK)==1);
}
static int access_allowed(struct a733_uart_platform_native *n,uintptr_t a,unsigned writing)
{
    if (!current(n) || !a733_uart_platform_native_address(n->port,a,writing)) return 0;
    uintptr_t base=n->port<7 ? UINT32_C(0x02500000)+n->port*0x1000u : UINT32_C(0x07080000)+(n->port-7)*0x1000u;
    if (a>=base && a<=base+0xa4) return 1; /* Exclusive controller, owner task. */
    return !(atomic_load_explicit(&n->locks->retained,memory_order_acquire)&1u) &&
        xSemaphoreGetMutexHolder(n->locks->mutex[A733_PLATFORM_REGISTER_LOCK])==(TaskHandle_t)n->owner_task;
}
static int read32(void *cookie,uintptr_t a,uint32_t *v)
{
    struct a733_uart_platform_native *n=cookie;
    if (!v || !access_allowed(n,a,0)) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    *v=*(volatile const uint32_t *)a;
    __asm__ volatile("dsb sy" ::: "memory"); return 1;
}
static int write32(void *cookie,uintptr_t a,uint32_t v)
{
    struct a733_uart_platform_native *n=cookie;
    if (!access_allowed(n,a,1)) return 0;
    uintptr_t base=n->port<7 ? UINT32_C(0x02500000)+n->port*0x1000u : UINT32_C(0x07080000)+(n->port-7)*0x1000u;
    if (a>=base && a<=base+0xa4) {
        if (v>255 || (a==base+0xa4 && (v&~7u)) ||
            (a==base+8 && v!=1 && v!=7) || (a==base+0x10 && v!=0 && v!=0x22)) return 0;
    } else if (a>=UINT32_C(0x03400000) && a<UINT32_C(0x03410000)) {
        unsigned intid=n->port<7 ? 34+n->port : 233+n->port-7;
        if (v!=(UINT32_C(1)<<(intid%32))) return 0;
    } else {
        uint32_t gate=n->port<7 ? 1u : UINT32_C(1)<<(n->port-7);
        uint32_t mask=gate|(gate<<16);
        __asm__ volatile("dsb sy" ::: "memory");
        uint32_t old=*(volatile const uint32_t *)a;
        if ((old^v)&~mask) return 0;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)a=v;
    __asm__ volatile("dsb sy" ::: "memory"); return 1;
}
int a733_uart_platform_native_init(struct a733_uart_platform_native *n,
    struct a733_platform_locks *locks,void *owner_task,unsigned port,
    int (*admit)(void *,const struct a733_uart_platform_config *,
        const struct a733_clock_token *,enum a733_uart_admission),void *admission_cookie)
{
    if (!n || n->initialized || !locks || !locks->initialized || !owner_task || port>8 || !admit) return 0;
    n->locks=locks;n->owner_task=owner_task;n->port=port;n->admit=admit;n->admission_cookie=admission_cookie;
    n->operations=(struct a733_uart_platform_ops){now,current,authorize,enter,leave,retain,read32,write32};
    n->initialized=1;return 1;
}
