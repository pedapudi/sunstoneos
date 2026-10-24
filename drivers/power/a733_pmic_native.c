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
/* The provider checks its deadline once per bus phase.
 * a733_counter_clock_now takes a global ticket lock with interrupts masked,
 * so this reads the architectural counter directly: the same microseconds as
 * the counter clock (counter ticks over ticks per microsecond, no origin,
 * divided by a reciprocal that the first read sets),
 * with no lock. The provider faults on a value that runs backwards, and a
 * fault latched in the shared counter clock refuses the read. */
static int now(void *v,uint64_t *out)
{
    struct a733_pmic_native *n=v;const struct a733_counter_clock *c=n->locks->context;
    if(!c->initialized || atomic_load_explicit(&c->failed,memory_order_acquire))return 0;
    if(!n->ticks_per_us.m && (!c->frequency || c->frequency%1000000u ||
       !a733_pmic_divisor_init(c->frequency/1000000u,&n->ticks_per_us)))return 0;
    uint64_t t;__asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(t) :: "memory");
    *out=a733_pmic_divide(t,&n->ticks_per_us);return 1;
}
/* The task and the kernel's mutex holder are checked once, when the bus is
 * entered. A FreeRTOS mutex changes holder only when its holder gives it, and
 * only leave() gives the I2C mutex on this task, so the holding flag stays
 * exact until then. A retain anywhere sets the retained bit, which every
 * access reads with one acquire load. */
static int enter(void *v,uint64_t deadline)
{
    struct a733_pmic_native *n=v;
    if(!current(n) || n->holding || !a733_platform_lock_enter(n->locks,A733_PLATFORM_I2C_LOCK,deadline))return 0;
    configASSERT(xSemaphoreGetMutexHolder(n->locks->mutex[A733_PLATFORM_I2C_LOCK])==(TaskHandle_t)n->task);
    n->holding=1;return 1;
}
static void leave(void *v)
{struct a733_pmic_native *n=v;n->holding=0;a733_platform_lock_leave(n->locks,A733_PLATFORM_I2C_LOCK);}
static void retain(void *v)
{struct a733_pmic_native *n=v;configASSERT(a733_platform_lock_retain(n->locks,A733_PLATFORM_I2C_LOCK)==1);}
/* No task check per access: only a733_pmic_poll() reaches these accessors,
 * and it checks the provider task at entry and holds the provider object
 * (checks/pmic-tree.py keeps both facts). */
static int owned(struct a733_pmic_native *n)
{
    return n->holding && !(atomic_load_explicit(&n->locks->retained,memory_order_acquire)&2u);
}
/* DSB SY before and after every access, as the shared MMIO rules require
 * until a lighter placement is qualified on the board (docs/pmic-runtime.md). */
static int read32(void *v,uintptr_t a,uint32_t *out)
{
    if(!out || !owned(v) || !a733_pmic_native_address(a,0))return 0;
    *out=a733_mmio_read32_dsb_sy_both(a);return 1;
}
static int write32(void *v,uintptr_t a,uint32_t value)
{
    if(!owned(v) || !a733_pmic_native_address(a,1) || value>255 ||
       (a==0x0708300cu && value!=0x48 && value!=0x58 && value!=0x68))return 0;
    a733_mmio_write32_dsb_sy_both(a,value);return 1;
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
    n->locks=locks;n->task=task;n->holding=0;n->ticks_per_us=(struct a733_pmic_divisor){0};
    n->operations=(struct a733_pmic_ops){now,current,enter,leave,retain,read32,write32,outside};
    n->initialized=1;return 1;
}
