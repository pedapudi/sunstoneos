#include "deadline.h"
#include "FreeRTOS.h"
#include "a733_mmio.h"
#include "platform.h"
#include <stdint.h>
#define RDIST UINT32_C(0x03460000)
#define SGI (RDIST+UINT32_C(0x10000))
#define PPI (UINT32_C(1)<<27)
extern uint64_t a733_smp_counter(void);
void npu_finite_deadline_setup(void)
{
    uint64_t frequency;
    __asm__ volatile("mrs %0,cntfrq_el0\n msr cntv_ctl_el0,xzr\n isb":"=r"(frequency)::"memory");
    configASSERT(frequency==24000000);
    /* EL3 establishes Group1NS and level sensitivity. Priority is readable
     * here, unlike Nonsecure IGROUPR when the distributor retains security. */
    configASSERT(*(volatile uint8_t *)(uintptr_t)(SGI+0x41b)==0xd8);
    a733_mmio_write32_dsb_sy_after(SGI+0x100,PPI);
    const uint64_t deadline=a733_smp_counter()+2400000;
    /* GICR_CTLR.RWP clears after the PPI enable write has propagated. */
    while(a733_mmio_read32(RDIST)&8)configASSERT(a733_smp_counter()<deadline);
    configASSERT((a733_mmio_read32(SGI+0x100)&PPI)&&
        !(a733_mmio_read32(SGI+0xc04)&(UINT32_C(1)<<23)));
}
