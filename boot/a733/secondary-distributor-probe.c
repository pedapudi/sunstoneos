/* Distributor preparation and exact restoration for the sequential diagnostic.
 * Mirrors the physically exercised native-startup-probe.S GIC interval. Also
 * holds the local snapshot comparison that both restorations share. */
#include "secondary-local-probe.h"
#define GICD UINT32_C(0x03400000)
static volatile struct secondary_distributor_record *const report=(void *)SECONDARY_DISTRIBUTOR_RECORD;
static void barrier(void) {__asm__ volatile("dsb sy":::"memory");}
static uint64_t ticks(void)
{uint64_t n;__asm__ volatile("isb\n mrs %0,cntpct_el0":"=r"(n)::"memory");return n;}
static uint32_t read32(uintptr_t address)
{barrier();uint32_t n=*(volatile uint32_t *)address;barrier();return n;}
static void write32(uintptr_t address,uint32_t value)
{barrier();*(volatile uint32_t *)address=value;barrier();}
_Static_assert(sizeof(struct secondary_local_state)==80,"Snapshot word order");
int secondary_local_same(const volatile struct secondary_local_state *a,
                         const volatile struct secondary_local_state *b)
{
    static const uint32_t status[10]={GICR_CTLR_STATUS,GICR_WAKER_STATUS,0,GICR_ISPENDR0_STATUS,
                                      0,0,0,0,0,GICR_PWRR_STATUS};
    const volatile uint64_t *x=(const volatile uint64_t *)a,*y=(const volatile uint64_t *)b;
    for(unsigned i=0;i<10;++i)if((x[i]^y[i])&~(uint64_t)status[i])return 0;
    return 1;
}
/* Bounded wait for GICD_CTLR.RWP, as Linux gic_do_wait_for_rwp and TF-A
 * gicd_wait_for_pending_write poll it. A timeout records its own error.
 * Not inlined: the overlay has no room for four copies. */
static __attribute__((noinline)) int wait_rwp(uint64_t deadline)
{
    uint64_t previous=ticks();
    for(;;) {
        uint64_t now=ticks();
        if(now<previous || now>=deadline) {report->error=DISTRIBUTOR_ERROR_RWP_TIMEOUT;return 0;}
        previous=now;if(!(read32(GICD)&GICD_CTLR_RWP))return 1;
    }
}
int secondary_distributor_begin(uint64_t deadline)
{
    const volatile uint64_t *words=(const volatile uint64_t *)report;
    for(unsigned i=0;i<sizeof(*report)/8;++i)if(words[i])return 0;
    if(!wait_rwp(deadline))return 0;
    uint32_t ctlr=read32(GICD),banks=(read32(GICD+4)&31)+1;
    if((ctlr&0x70)!=0x30 || ticks()>=deadline) {report->error=DISTRIBUTOR_ERROR_CONFIGURATION;return 0;}
    report->before=ctlr;report->banks=banks;report->deadline=deadline;
    for(unsigned i=1;i<banks;++i)report->enable_before[i]=read32(GICD+0x100+4*i);
    report->state=1;barrier();
    write32(GICD,ctlr&~7u);if(!wait_rwp(deadline))return 0;
    for(unsigned i=1;i<banks;++i)write32(GICD+0x180+4*i,UINT32_MAX);
    if(!wait_rwp(deadline))return 0;
    for(unsigned i=1;i<banks;++i)if(read32(GICD+0x100+4*i))return 0;
    write32(GICD,(ctlr&~7u)|3u);
    if(!wait_rwp(deadline) || (read32(GICD)&0x73)!=0x33)return 0;
    report->state=2;barrier();return 1;
}
int secondary_distributor_restore(uint64_t deadline)
{
    if(report->state!=2 || !report->banks || report->banks>32 || !wait_rwp(deadline))return 0;
    report->state=3;barrier();
    write32(GICD,(uint32_t)report->before&~7u);if(!wait_rwp(deadline))return 0;
    for(unsigned i=1;i<report->banks;++i)write32(GICD+0x180+4*i,UINT32_MAX);
    if(!wait_rwp(deadline))return 0;
    for(unsigned i=1;i<report->banks;++i)write32(GICD+0x100+4*i,report->enable_before[i]);
    for(unsigned i=1;i<report->banks;++i) {
        report->enable_after[i]=read32(GICD+0x100+4*i);
        if(report->enable_after[i]!=report->enable_before[i])return 0;
    }
    write32(GICD,(uint32_t)report->before);if(!wait_rwp(deadline))return 0;
    report->after=read32(GICD);
    if((report->after^report->before)&~(uint64_t)GICD_CTLR_RWP) {report->error=DISTRIBUTOR_ERROR_CONFIGURATION;return 0;}
    barrier();report->state=4;barrier();return 1;
}
