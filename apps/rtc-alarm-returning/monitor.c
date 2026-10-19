#include "boot.h"
#include "platform.h"
#include "monitor.h"
#include <string.h>
volatile struct rtc_alarm_route rtc_alarm_route __attribute__((section(".monitor_state")));
extern void __real_monitor_init(void);
extern _Noreturn void __real_monitor_exception(uint64_t,uint64_t,uint64_t);
#define D UINT32_C(0x03400000)
#define R UINT32_C(0x07090000)
#define BIT UINT32_C(16)
static int secure_access(uint64_t *el,uint64_t *sctlr,uint64_t *scr)
{
    __asm__ volatile("mrs %0,CurrentEL":"=r"(*el));
    *sctlr=0;*scr=0;if(*el!=12)return 0;
    __asm__ volatile("mrs %0,sctlr_el3\n mrs %1,scr_el3":"=r"(*sctlr),"=r"(*scr));
    /* TrustZone for Armv8-A3.2/3.4: EL3 is Secure regardless of SCR.NS;
     * with stage1 MMU disabled, accesses use Secure physical addresses.
     * Also require cache-disabled EL3, matching accepted monitor provenance.
     * SCR is observed, never changed by this device wrapper. */
    return *el==12&&!(*sctlr&UINT64_C(0x1005));
}
static int gic_quiet(void)
{return !(mmio_read(D)&(1u<<31))&&!((mmio_read(D+0x11c)|mmio_read(D+0x21c)|mmio_read(D+0x31c))&BIT);}
static int rtc_quiet(void)
{
    if((mmio_read(0x0701020c)&0x10001)!=0x10001)return 0;
    return !((mmio_read(R+0x28)|mmio_read(R+0x2c)|mmio_read(R+0x30)|mmio_read(R+0x50))&1);
}
static void field(uint32_t offset,uint32_t mask,uint32_t value)
{mmio_write(D+offset,(mmio_read(D+offset)&~mask)|(value&mask));__asm__ volatile("dsb sy":::"memory");}
static void route_write(uint64_t value)
{*(volatile uint64_t *)(uintptr_t)(D+0x6720)=value;__asm__ volatile("dsb sy":::"memory");}
static int fields_match(unsigned restored)
{
    return (mmio_read(D+0x9c)&BIT)==(restored?(rtc_alarm_route.group&BIT):BIT)&&
      (mmio_read(D+0xd1c)&BIT)==(restored?(rtc_alarm_route.modifier&BIT):0)&&
      (mmio_read(D+0xc38)&0x300)==(restored?(rtc_alarm_route.configuration&0x300):0)&&
      (mmio_read(D+0x4e4)&255)==(restored?(rtc_alarm_route.priority&255):0xe0)&&
      *(volatile uint64_t *)(uintptr_t)(D+0x6720)==(restored?rtc_alarm_route.affinity:0);
}
void __wrap_monitor_init(void)
{
    memset((void *)&rtc_alarm_route,0,sizeof(rtc_alarm_route));
    uint64_t el,sctlr,scr;unsigned admitted=secure_access(&el,&sctlr,&scr);
    rtc_alarm_route.entry_el=el;rtc_alarm_route.entry_sctlr=sctlr;rtc_alarm_route.entry_scr=scr;
    rtc_alarm_route.reason=1;
    if(admitted){
        rtc_alarm_route.ctlr=mmio_read(D);rtc_alarm_route.typer=mmio_read(D+4);
        admitted=(rtc_alarm_route.ctlr&0x80000070)==0x30&&(rtc_alarm_route.typer&31)>=7;
        rtc_alarm_route.reason=2;
    }
    if(admitted){
        /* Observe original state BEFORE the accepted monitor globally masks
         * SPIs. A post-mask zero cannot admit an originally enabled line. */
        rtc_alarm_route.enabled=mmio_read(D+0x11c);rtc_alarm_route.pending=mmio_read(D+0x21c);rtc_alarm_route.active=mmio_read(D+0x31c);
        admitted=!((rtc_alarm_route.enabled|rtc_alarm_route.pending|rtc_alarm_route.active)&BIT);
        rtc_alarm_route.reason=3;
    }
    if(admitted){
        admitted=rtc_quiet();rtc_alarm_route.reason=4;
        if(admitted){
            rtc_alarm_route.alarm_day=mmio_read(R+0x20);rtc_alarm_route.alarm_hms=mmio_read(R+0x24);
            rtc_alarm_route.group=mmio_read(D+0x9c);rtc_alarm_route.modifier=mmio_read(D+0xd1c);
            rtc_alarm_route.configuration=mmio_read(D+0xc38);rtc_alarm_route.priority=mmio_read(D+0x4e4);
            rtc_alarm_route.affinity=*(volatile uint64_t *)(uintptr_t)(D+0x6720);
            rtc_alarm_route.baseline=1;
        }
    }
    __real_monitor_init(); /* Unchanged2s recovery timer and NS handoff setup. */
    if(!admitted)return;
    /* Secure accesses remain established: original monitor leaves EL3 M/C/I
     * off; SCR.NS only chooses the lower-EL security state. */
    if(!secure_access(&el,&sctlr,&scr)||!gic_quiet()){rtc_alarm_route.reason=5;return;}
    rtc_alarm_route.mutated=1;
    field(0x9c,BIT,BIT);field(0xd1c,BIT,0);field(0xc38,0x300,0);field(0x4e4,255,0xe0);route_write(0);
    if(!gic_quiet()||!fields_match(0)){rtc_alarm_route.reason=6;rtc_alarm_route.retained=1;return;}
    rtc_alarm_route.ready=1;rtc_alarm_route.reason=0;__asm__ volatile("dsb sy":::"memory");
}
_Noreturn void __wrap_monitor_exception(uint64_t operation,uint64_t status,uint64_t kind)
{
    uint64_t el,sctlr,scr,esr;unsigned access=secure_access(&el,&sctlr,&scr);
    rtc_alarm_route.return_el=el;rtc_alarm_route.return_sctlr=sctlr;
    __asm__ volatile("mrs %0,esr_el3":"=r"(esr));
    if(rtc_alarm_route.mutated){
        rtc_alarm_route.retained=1;
        if(access){
            /* Always fence our newly enabled SPI, including timeout/fault.
             * Old monitor finish only sets saved enables and cannot do this. */
            mmio_write(D+0x19c,BIT);__asm__ volatile("dsb sy":::"memory");
            uint64_t end=counter()+COUNTER_HZ/50;
            while((mmio_read(D)&(1u<<31))&&counter()<end){}
            if(kind==1&&(esr>>26)==0x17&&operation==0x733&&!status&&rtc_alarm_route.ready&&
               gic_quiet()&&rtc_quiet()&&mmio_read(R+0x20)==rtc_alarm_route.alarm_day&&
               mmio_read(R+0x24)==rtc_alarm_route.alarm_hms){
                field(0x9c,BIT,rtc_alarm_route.group);field(0xd1c,BIT,rtc_alarm_route.modifier);
                field(0xc38,0x300,rtc_alarm_route.configuration);field(0x4e4,255,rtc_alarm_route.priority);route_write(rtc_alarm_route.affinity);
                if(gic_quiet()&&fields_match(1)){rtc_alarm_route.restored=1;rtc_alarm_route.retained=0;}
            }
        }
        platform_puts("RTC_ALARM_EL3_RESTORED=");platform_hex(rtc_alarm_route.restored);platform_puts("\n");
        platform_puts("RTC_ALARM_EL3_RETAINED=");platform_hex(rtc_alarm_route.retained);platform_puts("\n");
        if(rtc_alarm_route.retained)status=1;
    }
    /* No RTC cleanup or per-SPI restoration on timeout/uncertain return. */
    __real_monitor_exception(operation,status,kind);
}
