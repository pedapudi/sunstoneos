/* Private guard around the byte-identical qualified monitor. Only a normal,
 * restored high-speed read lifetime may reach its ROM return. */
#include "boot.h"
#include "platform.h"
#include "return.h"
extern void __real_monitor_init(void);
extern _Noreturn void __real_monitor_exception(uint64_t,uint64_t,uint64_t);
static const uintptr_t baseline_address[]={
    0x02002d00,0x02002d0c, /* SDC0 module and bus/reset */
    0x02000300,0x02000320,0x02000330, /* PF0..6 mux, drive, pull */
    0x02000040,0x02000048,0x02000070, /* PF supply selection */
    0x0701019c,0x07025000,0x07025014,0x07025024, /* S_TWI0 and PL0/1 */
    0x0701000c,0x07010010,0x07083014 /* Inherited APBS0/APBS1 and TWI divisor */
};
static const uint32_t baseline_mask[]={
    0x87001f1f,0x10001,0x0fffffff,0x0fffffff,0x3fff,
    3u<<10,3u<<10,1,0x10001,0xff,0xff,0xf,0x0700001f,0x0700001f,0x1ff
};
static volatile struct {
    uint64_t magic,start;
    uint32_t fields[15],entered;
} saved __attribute__((section(".monitor_state")));
static int secure(void)
{
    uint64_t el,sctlr;
    __asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(el!=12)return 0;
    __asm__ volatile("mrs %0,sctlr_el3":"=r"(sctlr));
    return !(sctlr&UINT64_C(0x1005));
}
static int irq_idle(void)
{
    return !(mmio_read(0x03400000)&(1u<<31))&&
        !((mmio_read(0x03400118)|mmio_read(0x03400218)|mmio_read(0x03400318))&2);
}
static int pmic_idle(void)
{
    return (mmio_read(0x0701019c)&0x10001)==0x10001&&
        (mmio_read(0x0708300c)&0xff)==0x40&&(mmio_read(0x07083010)&0xff)==0xf8&&
        (mmio_read(0x07083020)&0x35)==0x30;
}
void __wrap_monitor_init(void)
{
    saved.magic=0;saved.entered=0;saved.start=counter();
    SDRT_RETURN->magic=0;SDRT_RETURN->state=SDRT_RETURN_REFUSED;
    unsigned ok=secure()&&irq_idle()&&pmic_idle();
    if(ok){
        for(unsigned i=0;i<15;++i)saved.fields[i]=mmio_read(baseline_address[i])&baseline_mask[i];
        ok=!(saved.fields[0]&(1u<<31))&&!saved.fields[1]&&saved.fields[6]==(2u<<10)&&saved.fields[7]==1;
    }
    saved.magic=SDRT_RETURN_MAGIC;
    SDRT_RETURN->state=ok?SDRT_RETURN_UNARMED:SDRT_RETURN_REFUSED;
    SDRT_RETURN->magic=SDRT_RETURN_MAGIC;
    __asm__ volatile("dsb sy":::"memory");
    __real_monitor_init();
}
__attribute__((used,retain,noinline,noreturn)) void sdrt_return_park(void)
{
    __asm__ volatile("msr daifset,#15\n msr cntps_ctl_el1,xzr\n msr cntp_ctl_el0,xzr\n msr cntv_ctl_el0,xzr\n isb":::"memory");
    SDRT_RETURN->state=SDRT_RETURN_RETAINED;
    RESULT->status=7;RESULT->stage=5;
    __asm__ volatile("dsb sy":::"memory");
    for(;;)__asm__ volatile("wfe");
}
_Noreturn void __wrap_monitor_exception(uint64_t operation,uint64_t status,uint64_t kind)
{
    uint64_t esr;
    if(!secure()||saved.entered)sdrt_return_park();
    saved.entered=1; /* A synchronous fault in a readback cannot reach ROM. */
    __asm__ volatile("mrs %0,esr_el3":"=r"(esr));
    if(saved.magic!=SDRT_RETURN_MAGIC||SDRT_RETURN->magic!=SDRT_RETURN_MAGIC||
       kind!=1||(esr>>26)!=0x17||operation!=0x733)sdrt_return_park();
    uint64_t state=SDRT_RETURN->state;
    if(status==1&&(state==SDRT_RETURN_UNARMED||state==SDRT_RETURN_REFUSED))
        __real_monitor_exception(operation,status,kind);
    if(status||state!=SDRT_RETURN_READY)sdrt_return_park();
    uint64_t now=counter();
    if(now<=saved.start||now-saved.start>=2*COUNTER_HZ||!irq_idle()||!pmic_idle())sdrt_return_park();
    for(unsigned i=0;i<15;++i)
        if((mmio_read(baseline_address[i])&baseline_mask[i])!=saved.fields[i])sdrt_return_park();
    SDRT_RETURN->state=SDRT_RETURN_CONSUMED;
    __asm__ volatile("dsb sy":::"memory");
    platform_puts("SD_RESTORED=0x0000000000000001\n");
    __real_monitor_exception(operation,status,kind);
}
