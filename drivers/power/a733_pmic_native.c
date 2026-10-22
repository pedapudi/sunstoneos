#include "a733_pmic_native.h"
#include "a733_platform_locks.h"
#include "a733_counter_clock.h"
#include "task.h"
#include "a733_range.h"
#include "a733_mmio.h"
#include "a733_task_context.h"
#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1
#error "PMIC provider requires scheduler, task and mutex-owner APIs"
#endif
static int current(void *v)
{
    struct a733_pmic_native *n=v;
    return n && n->initialized && a733_task_context_owner_el1((TaskHandle_t)n->task);
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
/* Device-nGnRnE accesses to one peripheral reach it in program order, so
 * polled reads and the data-byte write need no barrier. A control write
 * starts a timed bus phase, and the provider times that phase from the write
 * reaching the controller: one DSB SY follows it. */
static int read32(void *v,uintptr_t a,uint32_t *out)
{
    if(!out || !owned(v) || !a733_pmic_native_address(a,0))return 0;
    *out=a733_mmio_read32(a);return 1;
}
static int write32(void *v,uintptr_t a,uint32_t value)
{
    if(!owned(v) || !a733_pmic_native_address(a,1) || value>255 ||
       (a==0x0708300cu && value!=0x48 && value!=0x58 && value!=0x68))return 0;
    if(a==0x0708300cu)a733_mmio_write32_dsb_sy_after(a,value);
    else a733_mmio_write32(a,value);
    return 1;
}
static int outside(void *v,const void *p,size_t size)
{
    struct a733_pmic_native *n=v;
    return a733_ranges_disjoint(p,size,n,sizeof(*n)) && a733_ranges_disjoint(p,size,n->locks,sizeof(*n->locks)) &&
        a733_ranges_disjoint(p,size,n->locks->context,sizeof(struct a733_counter_clock));
}
int a733_pmic_native_init(struct a733_pmic_native *n,struct a733_platform_locks *locks,void *task)
{
    if(!n || n->initialized || !locks || !locks->initialized || !task ||
       locks->now!=a733_counter_clock_now || !locks->context ||
       !a733_ranges_disjoint(n,sizeof(*n),locks,sizeof(*locks)) ||
       !a733_ranges_disjoint(n,sizeof(*n),locks->context,sizeof(struct a733_counter_clock)))return 0;
    n->locks=locks;n->task=task;
    n->operations=(struct a733_pmic_ops){now,current,enter,leave,retain,read32,write32,outside};
    n->initialized=1;return 1;
}
