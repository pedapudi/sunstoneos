/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_maintenance_watchdog_internal.h"
#include "a733_counter_clock.h"
#include "a733_platform_locks.h"
#include "a733_storage_update.h"
#include "aarch64_device_memory.h"
#include "a733_mmio.h"
#include "a733_task_context.h"
#include "task.h"

static int mappings(void)
{
    static const uintptr_t addresses[]={0x0205000c,0x02050010,0x02050014,0x02050018,
        0x03000024,0x070501c0,0x070501c4,0x070501d4,0x07090108,
        0x07090000,0x07090060,0x07090160};
    uint64_t el;__asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    if(el!=4)return 0;
    for(unsigned i=0;i<sizeof(addresses)/sizeof(addresses[0]);++i) {
        struct aarch64_translation t;
        if(!aarch64_device_translate(addresses[i],i==0,&t) ||
           t.physical!=addresses[i] || t.attributes!=0)return 0;
    }
    return 1;
}
static uint32_t read32(uintptr_t a)
{ return a733_mmio_read32_dsb_sy_both(a); }
static int same(const struct a733_watchdog_window *t)
{
    return (read32(0x02050014)&0xffff)==0x41 && (read32(0x02050010)&0xffff)==1 &&
        (read32(0x02050018)&0xffff)==2 && !(read32(0x03000024)&0x100) &&
        read32(0x070501c0)==0xfa50392f && read32(0x070501c4)==0x20 &&
        !read32(0x070501d4) && read32(0x07090108)==0x5aa5a55a &&
        !((read32(0x07090000)^t->rtc_control)&~A733_WATCHDOG_RTC_BUSY) &&
        read32(0x07090060)==t->rtc_divider &&
        read32(0x07090160)==t->rtc_source;
}
int a733_watchdog_hw_admit(const struct a733_watchdog_window *t)
{
    return t && xTaskGetSchedulerState()==taskSCHEDULER_NOT_STARTED && mappings() && same(t);
}
int a733_maintenance_hw_task(void *task)
{
    return task && a733_task_context_owner_el1((TaskHandle_t)task);
}
int a733_watchdog_hw_reload(const struct a733_watchdog_window *t,
    struct a733_counter_clock *clock,struct a733_platform_locks *locks,void *task,
    atomic_uint *state,uint64_t deadline,uint64_t *before,uint64_t *after)
{
    if(!a733_maintenance_hw_task(task) || !mappings() ||
       !a733_platform_lock_enter(locks,A733_PLATFORM_REGISTER_LOCK,deadline))return 0;
    unsigned expected=1;
    uint64_t start,end;
    if(!same(t) || a733_counter_clock_now(clock,&start)!=1 || start>=deadline ||
       start>=t->deadline_us ||
       !atomic_compare_exchange_strong_explicit(state,&expected,3,memory_order_acq_rel,memory_order_acquire)) {
        a733_platform_lock_leave(locks,A733_PLATFORM_REGISTER_LOCK);return 0;
    }
    /* The successful state transition commits this one reload. Concurrent
     * revocation may prevent later reloads but cannot retract a bus transaction.
     * Neither CTRL nor the configuration readback acknowledges counter restart.
     */
    a733_mmio_write32_dsb_sy_both(0x0205000c,0x14af);
    if(!same(t) || a733_counter_clock_now(clock,&end)!=1 || end<start ||
       end-start>t->guard_us || end>=t->deadline_us) {
        atomic_fetch_or_explicit(state,4,memory_order_release);
        (void)a733_platform_lock_retain(locks,A733_PLATFORM_REGISTER_LOCK);return 0;
    }
    unsigned final=atomic_fetch_and_explicit(state,~2u,memory_order_acq_rel);
    int success=(final&4)==0;
    a733_platform_lock_leave(locks,A733_PLATFORM_REGISTER_LOCK);
    if(!success)return 0;
    *before=start;*after=end;return 1;
}

static struct a733_watchdog_window window(const struct a733_maintenance_transfer *t)
{
    return (struct a733_watchdog_window){t->storage->deadline_us,t->guard_us,
        t->rtc_control,t->rtc_divider,t->rtc_source};
}
int a733_maintenance_hw_admit(const struct a733_maintenance_transfer *t)
{
    if(!t||!t->storage)return 0;
    struct a733_watchdog_window w=window(t);return a733_watchdog_hw_admit(&w);
}
int a733_maintenance_hw_reload(const struct a733_maintenance_transfer *t,
    struct a733_counter_clock *clock,struct a733_platform_locks *locks,void *task,
    atomic_uint *state,uint64_t deadline,uint64_t *before,uint64_t *after)
{
    if(!t||!t->storage)return 0;
    struct a733_watchdog_window w=window(t);
    return a733_watchdog_hw_reload(&w,clock,locks,task,state,deadline,before,after);
}
