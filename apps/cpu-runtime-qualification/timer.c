#include "FreeRTOS.h"
#include "platform.h"
#include "cached.h"
#include "runtime.h"

/* Owned by one masked initializer and that CPU's IRQ handler. State is
 * published before enabling the producer, even if expiry precedes SVC106. */
static uint32_t timer_state[QUAL_CORES];
extern _Noreturn void a733_smp_exit_privileged(unsigned status,uint64_t detail);

static _Noreturn void failed(unsigned reason)
{a733_smp_exit_privileged(4,0x73600+reason);}

void a733_qualification_timer_arm(unsigned core)
{
    uint64_t el,index,flags,control;
    __asm__ volatile("mrs %0,currentel\n mrs %1,tpidr_el1\n mrs %2,daif\n mrs %3,cntp_ctl_el0"
        :"=r"(el),"=r"(index),"=r"(flags),"=r"(control));
    if(!core || core>=QUAL_CORES || index!=core || el!=4 || !(flags&128) || (control&1))failed(1);
    uint64_t deadline=a733_cached_runtime[core].startup_deadline;
    if(a733_smp_counter()>=deadline || timer_state[core] || a733_smp_core_results[core].ticks)failed(2);
    uintptr_t r=a733_cached_runtime[core].redistributor,g=r+0x10000;
    const uint32_t ppi=UINT32_C(1)<<30;
    if(r!=0x03460000+core*0x20000 ||
       (*(volatile uint32_t *)(g+0x100)&ppi) ||
       (*(volatile uint32_t *)(g+0x200)&ppi) ||
       (*(volatile uint32_t *)(g+0x300)&ppi) ||
       (*(volatile uint32_t *)(g+0xc04)&(3u<<28)))failed(3);
    const uint8_t priority=portLOWEST_USABLE_INTERRUPT_PRIORITY<<portPRIORITY_SHIFT;
    *(volatile uint8_t *)(g+0x400+30)=priority;
    *(volatile uint32_t *)(g+0x100)=ppi;
    __asm__ volatile("dsb sy":::"memory");
    while(*(volatile uint32_t *)r&8)if(a733_smp_counter()>=deadline)failed(4);
    if(!(*(volatile uint32_t *)(g+0x100)&ppi) ||
       *(volatile uint8_t *)(g+0x400+30)!=priority)failed(5);
    uint64_t now=a733_smp_counter();
    if(now>=deadline)failed(5);
    uint64_t compare=now+A733_SMP_COUNTER_HZ/configTICK_RATE_HZ;
    __atomic_store_n(&timer_state[core],1,__ATOMIC_RELEASE);
    __asm__ volatile("msr cntp_cval_el0,%0\n dsb sy\n msr cntp_ctl_el0,%1\n isb"
        ::"r"(compare),"r"(UINT64_C(1)):"memory");
}

void a733_qualification_timer_irq(unsigned core)
{
    uint64_t control;
    /* ISTATUS is meaningful only while ENABLE is set. Remove the level
     * source before returning to the port's combined priority drop/EOI. */
    __asm__ volatile("mrs %0,cntp_ctl_el0\n msr cntp_ctl_el0,xzr\n dsb sy\n isb"
        :"=r"(control)::"memory");
    if(!core || core>=QUAL_CORES || (control&7)!=5 ||
       __atomic_load_n(&timer_state[core],__ATOMIC_ACQUIRE)!=1 ||
       __atomic_load_n(&a733_smp_core_results[core].ticks,__ATOMIC_RELAXED))failed(6);
    __atomic_store_n(&timer_state[core],2,__ATOMIC_RELEASE);
    __atomic_store_n(&a733_smp_core_results[core].ticks,1,__ATOMIC_RELEASE);
}
