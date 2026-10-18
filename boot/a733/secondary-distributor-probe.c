/* Distributor preparation and exact restoration for the sequential diagnostic.
 * Mirrors the physically exercised native-startup-probe.S GIC interval. */
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
static int wait_rwp(uint64_t deadline)
{
    uint64_t previous=ticks();
    for(;;) {
        uint64_t now=ticks();if(now<previous || now>=deadline)return 0;
        previous=now;if(!(read32(GICD)&UINT32_C(0x80000000)))return 1;
    }
}
int secondary_distributor_begin(uint64_t deadline)
{
    const volatile uint64_t *words=(const volatile uint64_t *)report;
    for(unsigned i=0;i<sizeof(*report)/8;++i)if(words[i])return 0;
    uint32_t ctlr=read32(GICD),banks=(read32(GICD+4)&31)+1;
    if((ctlr&0x70)!=0x30 || (ctlr&UINT32_C(0x80000000)) || ticks()>=deadline)return 0;
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
    report->after=read32(GICD);if(report->after!=report->before)return 0;
    barrier();report->state=4;barrier();return 1;
}
