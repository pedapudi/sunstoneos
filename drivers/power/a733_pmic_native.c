#include "a733_pmic_native.h"
#include "a733_platform_locks.h"
#include "a733_counter_clock.h"
#include "task.h"
#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1
#error "PMIC provider requires scheduler, task and mutex-owner APIs"
#endif
static int current(void *v)
{
    struct a733_pmic_native *n=v;uint64_t el,flags,nesting,critical;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, daif" : "=r"(el),"=r"(flags));
    if(!n || !n->initialized || el!=4 || (flags&0x80))return 0;
    __asm__ volatile("msr daifset, #2" ::: "memory");
#if configNUMBER_OF_CORES > 1
    extern volatile uint64_t ullPortInterruptNestings[],ullCriticalNestings[];
    uint64_t core;__asm__ volatile("mrs %0, tpidr_el1" : "=r"(core));
    nesting=core<configNUMBER_OF_CORES ? ullPortInterruptNestings[core] : 1;
    critical=core<configNUMBER_OF_CORES ? ullCriticalNestings[core] : 1;
#else
    extern uint64_t ullPortInterruptNesting;
    extern volatile uint64_t ullCriticalNesting;
    nesting=ullPortInterruptNesting;critical=ullCriticalNesting;
#endif
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
    return !nesting && !critical && xTaskGetSchedulerState()==taskSCHEDULER_RUNNING &&
        xTaskGetCurrentTaskHandle()==(TaskHandle_t)n->task;
}
static int now(void *v,uint64_t *out)
{struct a733_pmic_native *n=v;return n->locks->now(n->locks->context,out);}
static int enter(void *v,uint64_t deadline)
{struct a733_pmic_native *n=v;return current(n) && a733_platform_lock_enter(n->locks,A733_PLATFORM_I2C_LOCK,deadline);}
static void leave(void *v)
{struct a733_pmic_native *n=v;a733_platform_lock_leave(n->locks,A733_PLATFORM_I2C_LOCK);}
static void retain(void *v)
{struct a733_pmic_native *n=v;configASSERT(a733_platform_lock_retain(n->locks,A733_PLATFORM_I2C_LOCK)==1);}
static int owned(struct a733_pmic_native *n)
{
    return current(n) && !(atomic_load_explicit(&n->locks->retained,memory_order_acquire)&2u) &&
        xSemaphoreGetMutexHolder(n->locks->mutex[A733_PLATFORM_I2C_LOCK])==(TaskHandle_t)n->task;
}
static int read32(void *v,uintptr_t a,uint32_t *out)
{
    if(!out || !owned(v) || !a733_pmic_native_address(a,0))return 0;
    __asm__ volatile("dsb sy" ::: "memory");*out=*(volatile const uint32_t *)a;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
static int write32(void *v,uintptr_t a,uint32_t value)
{
    if(!owned(v) || !a733_pmic_native_address(a,1) || value>255 ||
       (a==0x0708300cu && value!=0x48 && value!=0x58 && value!=0x68))return 0;
    __asm__ volatile("dsb sy" ::: "memory");*(volatile uint32_t *)a=value;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
static int separate(const void *p,size_t size,const void *q,size_t count)
{
    uintptr_t a=(uintptr_t)p,b=(uintptr_t)q;
    return p && q && size<=UINTPTR_MAX-a && count<=UINTPTR_MAX-b && (a+size<=b || b+count<=a);
}
static int outside(void *v,const void *p,size_t size)
{
    struct a733_pmic_native *n=v;
    return separate(p,size,n,sizeof(*n)) && separate(p,size,n->locks,sizeof(*n->locks)) &&
        separate(p,size,n->locks->context,sizeof(struct a733_counter_clock));
}
int a733_pmic_native_init(struct a733_pmic_native *n,struct a733_platform_locks *locks,void *task)
{
    if(!n || n->initialized || !locks || !locks->initialized || !task ||
       locks->now!=a733_counter_clock_now || !locks->context ||
       !separate(n,sizeof(*n),locks,sizeof(*locks)) ||
       !separate(n,sizeof(*n),locks->context,sizeof(struct a733_counter_clock)))return 0;
    n->locks=locks;n->task=task;
    n->operations=(struct a733_pmic_ops){now,current,enter,leave,retain,read32,write32,outside};
    n->initialized=1;return 1;
}
