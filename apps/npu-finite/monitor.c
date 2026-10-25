/* Reuse authenticated retention, secondary-OFF admission, protected page-table
 * construction, GIC/timer initialization and all retained failure paths.
 * Replace only the terminal exception entry for a matched successful receipt. */
#define sd_single_core_exception npu_finite_retained_exception
#ifdef SUNSTONE_NPU_FINITE_THERMAL
#define sd_single_core_monitor_init npu_finite_base_monitor_init
#endif
#include "../storage-read-single-core/monitor.c"
#ifdef SUNSTONE_NPU_FINITE_THERMAL
#undef sd_single_core_monitor_init
extern unsigned npu_finite_thermal_irq_admitted;
static int thermal_irq_quiet(void)
{return !(mmio_read(GICD_BASE)&(1u<<31)) &&
    !((mmio_read(GICD_BASE+0x108)|mmio_read(GICD_BASE+0x208)|mmio_read(GICD_BASE+0x308))&128u);}
void sd_single_core_monitor_init(void)
{
    npu_finite_base_monitor_init();
    /* The base monitor already masked SPIs. Authenticate the saved incoming
     * enable state; never clear inherited THS pending/active state. */
    if(!saved.ready || saved.banks<=2 || (saved.spi_enabled[2]&128u) || !thermal_irq_quiet())finish(1);
    npu_finite_thermal_irq_admitted=1;
    __asm__ volatile("dsb sy":::"memory");
}
#endif
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
#ifdef SUNSTONE_NPU_INTERRUPT
    if(!npu_irq_route_restore(&saved.npu_irq,deadline))return 0;
#endif
    for(unsigned i=1;i<saved.banks;++i) {
        if(!return_timely(deadline))return 0;
        mmio_write(GICD_BASE+0x100+4*i,saved.spi_enabled[i]);
    }
    mmio_write(GICD_BASE,saved.ctlr|(live&0x30u));
    return return_wait(GICD_BASE,1u<<31,deadline);
}
/* Route the shared SRAM back to the CPU before the ROM return. The forced-FEL
 * return enters the boot ROM at 0x20, and that path zeroes
 * 0x00044000-0x000f3ffc before it sets SYSCTRL 0x03000008 bit 1 at offset
 * 0x20c (fixs-brom 3a2a7d28); with the SRAM still routed to the NPU those
 * stores would reach the NPU's SRAM. A cold entry at 0x0 sets the bit before
 * it touches the SRAM. The startup's local stop has already set the bit after
 * a normal stop; this step does not depend on that. As at EL1, the bit
 * changes only while CCU +0xb04 holds the NPU's AHB, AXI and core resets
 * asserted with its bus gate closed and the module gate at +0xb00 is closed.
 * A held-reset check or a readback that fails parks with status 7 instead of
 * returning. */
#define SRAM_ROUTING UINT32_C(0x03000008)
#define SRAM_TO_CPU UINT32_C(2)
static void return_sram_to_cpu(void)
{
    uint32_t value=mmio_read(SRAM_ROUTING);
    __asm__ volatile("dsb sy":::"memory");
    if(!(value&SRAM_TO_CPU)) {
        const uint32_t bus=mmio_read(UINT32_C(0x02002b04)),module=mmio_read(UINT32_C(0x02002b00));
        __asm__ volatile("dsb sy":::"memory");
        if((bus&UINT32_C(0x70001)) || (module&UINT32_C(0x80000000)))a733_smp_stop(7,SRAM_ROUTING);
        mmio_write(SRAM_ROUTING,value|SRAM_TO_CPU);
        __asm__ volatile("dsb sy":::"memory");
        value=mmio_read(SRAM_ROUTING);
        __asm__ volatile("dsb sy":::"memory");
    }
    if(!(value&SRAM_TO_CPU))a733_smp_stop(7,SRAM_ROUTING);
}
_Noreturn void sd_single_core_exception(uint64_t operation,uint64_t status,uint64_t kind)
{
    uint64_t esr,control;
    __asm__ volatile("msr daifset,#15\n msr cntps_ctl_el1,xzr\n msr cntp_ctl_el0,xzr\n msr cntv_ctl_el0,xzr\n isb\n mrs %0,esr_el3\n mrs %1,sctlr_el1":"=r"(esr),"=r"(control)::"memory");
    const struct npu_finite_receipt *r=&npu_finite_receipt;
    uint64_t now=counter(),deadline=admission.diagnostic_deadline;
#ifdef SUNSTONE_NPU_ENTRY_CHECKPOINT
    uint64_t caller;__asm__ volatile("mrs %0,spsr_el3":"=r"(caller));
    if(status || (caller&15)!=5 || !(caller&0x80) ||
       !npu_entry_checkpoint_ready(admission.epoch,admission.generation,admission.lease,
           admission.armed_at,admission.diagnostic_deadline))a733_smp_stop(6,operation);
#endif
    if(kind!=1 || (esr>>26)!=0x17 || operation!=0x733 || status>
#ifdef SUNSTONE_NPU_FINITE_THERMAL
       2 || now>=r->thermal_deadline || r->thermal_deadline>deadline || !npu_finite_thermal_irq_admitted || !thermal_irq_quiet() ||
       r->thermal_reason>3 || r->jobs>2 || (status==1 && !r->jobs) || (status==0 && r->jobs!=2) ||
       (status==2 && (!r->thermal_reason || r->jobs==2)) ||
#else
       1 ||
#endif
       (control&0x1005) ||
       now>=deadline || r->magic!=NPU_FINITE_RECEIPT_MAGIC || r->epoch!=admission.epoch ||
       r->generation!=admission.generation || r->lease!=admission.lease || r->status!=status ||
       r->complete!=NPU_FINITE_COMPLETE(status) || r->deadline!=deadline ||
       r->clean_start!=(uintptr_t)__npu_clean_start || r->clean_end!=(uintptr_t)__device_pool_end ||
       r->clean_start!=UINT64_C(0x41200000) || r->clean_end<=r->clean_start ||
       r->clean_end>UINT64_C(0x41f00000) || ((r->clean_start|r->clean_end)&4095) ||
       r->clean_lines!=(r->clean_end-r->clean_start)/64 || r->log_length>RESULT_SIZE-LOG_OFFSET-1)
        a733_smp_stop(6,operation);
    /* No normal return follows a recovery timer, CPU exception, absent receipt,
     * late cleanup or uncertain device state. Resources remain retained. */
    uint64_t cleanup_deadline=
#ifdef SUNSTONE_NPU_FINITE_THERMAL
        r->thermal_deadline;
#else
        deadline;
#endif
    if(!return_secondaries(cleanup_deadline) || !return_gic(cleanup_deadline))a733_smp_stop(7,operation);
#ifdef SUNSTONE_NPU_FINITE_THERMAL
    if(!thermal_irq_quiet())a733_smp_stop(7,operation);
    uint64_t completed_at=counter();
    if(completed_at>=r->thermal_deadline || completed_at>=deadline)a733_smp_stop(7,operation);
#endif
    return_sram_to_cpu();
    REPORT->status=status;
#ifdef SUNSTONE_NPU_ENTRY_CHECKPOINT
    REPORT->detail=NPU_ENTRY_MEMORY_READY;
#else
    REPORT->detail=0;
#endif
#ifdef SUNSTONE_NPU_FINITE_THERMAL
    REPORT->end_counter=completed_at;
#else
    REPORT->end_counter=counter();
#endif
    REPORT->completed_mask=1;
    RESULT->status=status;RESULT->end_counter=REPORT->end_counter;RESULT->stage=5;
    __asm__ volatile("dsb sy":::"memory");
    REPORT->stage=5;
    __asm__ volatile("dsb sy":::"memory");
    npu_finite_rom_return();
}
