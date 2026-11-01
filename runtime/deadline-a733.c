#include "FreeRTOS.h"
#include "task.h"
#include "sunstone_deadline.h"
uint64_t sunstone_deadline_now_us(void)
{
    uint64_t count;__asm__ volatile("isb\n mrs %0,cntpct_el0":"=r"(count)::"memory");
    return count/24;
}
void sunstone_deadline_kick(void)
{
#ifdef SUNSTONE_DEADLINE_SINGLE_CORE
    /* The common wait publishes with the timer's API priority masked. This
     * backend is for EL1 tasks; only the SMP backend must cross from EL0. */
    sunstone_deadline_rearm();
#else
    vInterruptCore(0,0);
#endif
}
void sunstone_deadline_program(uint64_t until)
{
    if(!until){__asm__ volatile("msr cntv_ctl_el0,xzr\n isb":::"memory");return;}
    uint64_t ticks=until*24,enable=1;
    __asm__ volatile("msr cntv_cval_el0,%0\n msr cntv_ctl_el0,%1\n isb"
        ::"r"(ticks),"r"(enable):"memory");
}
