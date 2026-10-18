/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_maintenance_watchdog_internal.h"
#include "a733_counter_clock.h"
#include "a733_platform_locks.h"
#include "a733_storage_update.h"
#include "aarch64_device_memory.h"
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
{
    __asm__ volatile("dsb sy":::"memory");
    uint32_t value=*(volatile const uint32_t *)a;
    __asm__ volatile("dsb sy":::"memory");return value;
}
static int same(const struct a733_maintenance_transfer *t)
{
    return (read32(0x02050014)&0xffff)==0x41 && (read32(0x02050010)&0xffff)==1 &&
        (read32(0x02050018)&0xffff)==2 && !(read32(0x03000024)&0x100) &&
        read32(0x070501c0)==0xfa50392f && read32(0x070501c4)==0x20 &&
        !read32(0x070501d4) && read32(0x07090108)==0x5aa5a55a &&
        read32(0x07090000)==t->rtc_control && read32(0x07090060)==t->rtc_divider &&
        read32(0x07090160)==t->rtc_source;
}
int a733_maintenance_hw_admit(const struct a733_maintenance_transfer *t)
{
    return t && xTaskGetSchedulerState()==taskSCHEDULER_NOT_STARTED && mappings() && same(t);
}
int a733_maintenance_hw_task(void *task)
{
    uint64_t el,flags,nesting,critical;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, daif":"=r"(el),"=r"(flags));
    if(!task || el!=4 || (flags&0x80))return 0;
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
    return !nesting && !critical && xTaskGetSchedulerState()==taskSCHEDULER_RUNNING &&
        xTaskGetCurrentTaskHandle()==(TaskHandle_t)task;
}
int a733_maintenance_hw_reload(const struct a733_maintenance_transfer *t,
    struct a733_counter_clock *clock,struct a733_platform_locks *locks,void *task,
    atomic_uint *state,uint64_t deadline,uint64_t *before,uint64_t *after)
{
    if(!a733_maintenance_hw_task(task) || !mappings() ||
       !a733_platform_lock_enter(locks,A733_PLATFORM_REGISTER_LOCK,deadline))return 0;
    unsigned expected=1;
    uint64_t start,end;
    if(!same(t) || a733_counter_clock_now(clock,&start)!=1 || start>=deadline ||
       start>=t->storage->deadline_us ||
       !atomic_compare_exchange_strong_explicit(state,&expected,3,memory_order_acq_rel,memory_order_acquire)) {
        a733_platform_lock_leave(locks,A733_PLATFORM_REGISTER_LOCK);return 0;
    }
    /* The successful state transition commits this one reload. Concurrent
     * revocation may prevent later reloads but cannot retract a bus transaction.
     * Neither CTRL nor the configuration readback acknowledges counter restart.
     */
    __asm__ volatile("dsb sy":::"memory");
    *(volatile uint32_t *)(uintptr_t)0x0205000c=0x14af;
    __asm__ volatile("dsb sy":::"memory");
    if(!same(t) || a733_counter_clock_now(clock,&end)!=1 || end<start ||
       end-start>t->guard_us || end>=t->storage->deadline_us) {
        atomic_fetch_or_explicit(state,4,memory_order_release);
        (void)a733_platform_lock_retain(locks,A733_PLATFORM_REGISTER_LOCK);return 0;
    }
    unsigned final=atomic_fetch_and_explicit(state,~2u,memory_order_acq_rel);
    int success=(final&4)==0;
    a733_platform_lock_leave(locks,A733_PLATFORM_REGISTER_LOCK);
    if(!success)return 0;
    *before=start;*after=end;return 1;
}
