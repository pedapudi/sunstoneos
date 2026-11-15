#include "FreeRTOS.h"
#include "task.h"
#include "sunstone_deadline.h"
/* The precise-deadline backend of the QEMU models, as runtime/deadline-a733.c is
 * the board's: microseconds of the generic counter, and the virtual timer CNTV
 * (PPI 27) on CPU0, which platform.c routes to sunstone_deadline_irq. The
 * counter runs at CNTFRQ_EL0, not the board's 24 MHz, so a microsecond is
 * converted in whole seconds and a remainder, which neither product
 * overflows: now is the whole microseconds counted, and a deadline's compare
 * is the first counter value at which now reaches it. */
static uint64_t frequency(void)
{
    uint64_t f;__asm__ volatile("mrs %0, cntfrq_el0":"=r"(f));return f;
}
uint64_t sunstone_deadline_now_us(void)
{
    uint64_t count;__asm__ volatile("isb\n mrs %0,cntpct_el0":"=r"(count)::"memory");
    const uint64_t f=frequency();
    return count/f*1000000u+count%f*1000000u/f;
}
void sunstone_deadline_kick(void)
{
    vInterruptCore(0,0);
}
void sunstone_deadline_program(uint64_t until)
{
    if(!until){__asm__ volatile("msr cntv_ctl_el0,xzr\n isb":::"memory");return;}
    const uint64_t f=frequency(),compare=until/1000000u*f+(until%1000000u*f+999999u)/1000000u,enable=1;
    __asm__ volatile("msr cntv_cval_el0,%0\n msr cntv_ctl_el0,%1\n isb"
        ::"r"(compare),"r"(enable):"memory");
}
