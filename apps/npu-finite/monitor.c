/* Reuse authenticated retention, secondary-OFF admission, protected page-table
 * construction, GIC/timer initialization and all retained failure paths.
 * Replace only the terminal exception entry for a matched successful receipt. */
#define sd_single_core_exception npu_finite_retained_exception
#include "../storage-read-single-core/monitor.c"
#undef sd_single_core_exception
#include "return.h"
extern _Noreturn void npu_finite_rom_return(void);
static int return_timely(uint64_t deadline)
{return counter()<deadline;}
static int return_wait(uintptr_t address,uint32_t mask,uint64_t deadline)
{
    while(mmio_read(address)&mask)if(!return_timely(deadline))return 0;
    return return_timely(deadline);
}
static int return_secondaries(uint64_t deadline)
{
    for(unsigned c=1;c<8;++c) {
        if(!return_timely(deadline) ||
           (mmio_read(UINT32_C(0x07052008)+(uintptr_t)c*0x1000)&15) ||
           (mmio_read(UINT32_C(0x07050200)+c*4)&1) ||
           (mmio_read(UINT32_C(0x07050220)+c*4)&1))return 0;
    }
    return return_timely(deadline);
}
static int return_gic(uint64_t deadline)
{
    if(!saved.ready || !saved.banks || saved.banks>32)return 0;
    uint32_t live=mmio_read(GICD_BASE);
    mmio_write(GICD_BASE,live&~7u);
    if(!return_wait(GICD_BASE,1u<<31,deadline))return 0;
    mmio_write(GICR_SGI+0x180,UINT32_MAX);
    if(!return_wait(GICR_BASE,8,deadline))return 0;
    /* Device IRQ97 was disabled/clear/inactive before receipt publication.
     * Only the owned timer PPIs are cleared by the monitor. */
    mmio_write(GICR_SGI+0x280,(1u<<TICK_IRQ)|(1u<<RECOVERY_IRQ));
    mmio_write(GICR_SGI+0x380,(1u<<TICK_IRQ)|(1u<<RECOVERY_IRQ));
    mmio_write(GICR_SGI+0x80,saved.group);
    mmio_write(GICR_SGI+0xd00,saved.modifier);
    mmio_write(GICR_SGI+0xc04,saved.configuration);
    mmio_write(GICR_SGI+0x41c,saved.priorities);
    mmio_write(GICR_SGI+0x100,saved.enabled);
    if(!return_wait(GICR_BASE,8,deadline))return 0;
    for(unsigned i=1;i<saved.banks;++i) {
        if(!return_timely(deadline))return 0;
        mmio_write(GICD_BASE+0x100+4*i,saved.spi_enabled[i]);
    }
    mmio_write(GICD_BASE,saved.ctlr|(live&0x30u));
    return return_wait(GICD_BASE,1u<<31,deadline);
}
_Noreturn void sd_single_core_exception(uint64_t operation,uint64_t status,uint64_t kind)
{
    uint64_t esr,control;
    __asm__ volatile("msr daifset,#15\n msr cntps_ctl_el1,xzr\n msr cntp_ctl_el0,xzr\n msr cntv_ctl_el0,xzr\n isb\n mrs %0,esr_el3\n mrs %1,sctlr_el1":"=r"(esr),"=r"(control)::"memory");
    const struct npu_finite_receipt *r=&npu_finite_receipt;
    uint64_t now=counter(),deadline=admission.diagnostic_deadline;
    if(kind!=1 || (esr>>26)!=0x17 || operation!=0x733 || status>1 || (control&0x1005) ||
       now>=deadline || r->magic!=NPU_FINITE_RECEIPT_MAGIC || r->epoch!=admission.epoch ||
       r->generation!=admission.generation || r->lease!=admission.lease || r->status!=status ||
       r->complete!=(status?2u:1u) || r->deadline!=deadline ||
       r->clean_start!=(uintptr_t)__npu_clean_start || r->clean_end!=(uintptr_t)__device_pool_end ||
       r->clean_start!=UINT64_C(0x41200000) || r->clean_end<=r->clean_start ||
       r->clean_end>UINT64_C(0x41f00000) || ((r->clean_start|r->clean_end)&4095) ||
       r->clean_lines!=(r->clean_end-r->clean_start)/64 || r->log_length>RESULT_SIZE-LOG_OFFSET-1)
        a733_smp_stop(6,operation);
    /* No normal return follows a recovery timer, CPU exception, absent receipt,
     * late cleanup or uncertain device state. Resources remain retained. */
    if(!return_secondaries(deadline) || !return_gic(deadline))a733_smp_stop(7,operation);
    REPORT->status=status;REPORT->detail=0;REPORT->end_counter=counter();REPORT->completed_mask=1;
    RESULT->status=status;RESULT->end_counter=REPORT->end_counter;RESULT->stage=5;
    __asm__ volatile("dsb sy":::"memory");
    REPORT->stage=5;
    __asm__ volatile("dsb sy":::"memory");
    npu_finite_rom_return();
}
