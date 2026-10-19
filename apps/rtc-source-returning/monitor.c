#include "boot.h"
#include "monitor.h"
#include <string.h>
volatile struct rtc_source_admission rtc_source_admission __attribute__((section(".monitor_state")));
extern void __real_monitor_init(void);
void __wrap_monitor_init(void)
{
    memset((void *)&rtc_source_admission,0,sizeof(rtc_source_admission));
    uint64_t el,sctlr=0,scr=0;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    rtc_source_admission.el=el;rtc_source_admission.reason=1;
    if(el==12){
        __asm__ volatile("mrs %0,sctlr_el3\n mrs %1,scr_el3":"=r"(sctlr),"=r"(scr));
        rtc_source_admission.sctlr=sctlr;rtc_source_admission.scr=scr;
        /* EL3 is Secure regardless of SCR.NS; stage1-disabled accesses target
         * Secure physical space. No NS RAZ register value supplies authority. */
        if(!(sctlr&UINT64_C(0x1005))){
            rtc_source_admission.ctlr=mmio_read(0x03400000);rtc_source_admission.typer=mmio_read(0x03400004);
            rtc_source_admission.reason=2;
            if((rtc_source_admission.ctlr&0x80000070)==0x30&&(rtc_source_admission.typer&31)>=7){
                rtc_source_admission.enabled=mmio_read(0x0340011c);rtc_source_admission.pending=mmio_read(0x0340021c);
                rtc_source_admission.active=mmio_read(0x0340031c);rtc_source_admission.reason=3;
                if(!((rtc_source_admission.enabled|rtc_source_admission.pending|rtc_source_admission.active)&16)){
                    rtc_source_admission.ready=1;rtc_source_admission.reason=0;
                }
            }
        }
    }
    /* No GIC configuration or source mutation in this hook. Original state
     * is captured before the accepted monitor's global SPI mask. Retained
     * .monitor_state is outside EL1 BSS and never written by the task. */
    __asm__ volatile("dsb sy":::"memory");__real_monitor_init();
}
