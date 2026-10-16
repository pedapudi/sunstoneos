#include "result.h"
#include "boot.h"
#include "a733_usb.h"
#include "device/dcd.h"
#define USB UINT32_C(0x04100000)
#define CCU UINT32_C(0x02002000)
static unsigned polling;
static uint64_t last_counter;
extern void a733_cached_smc(unsigned,uint64_t) __attribute__((noreturn));
_Noreturn void a733_observe_failure(uint64_t detail)
{a733_cached_smc(4,UINT64_C(0x55000000)|detail);}
void a733_observe_deadline(void)
{
    uint64_t n=a733_smp_counter();
    if(n<last_counter || n>=a733_observe_identity.deadline)a733_observe_failure(1);
    last_counter=n;
}
uint32_t tusb_time_millis_api(void)
{a733_observe_deadline();return (uint32_t)(last_counter/24000);}
void tusb_time_delay_ms_api(uint32_t ms)
{
    if(ms>150)a733_observe_failure(2);
    a733_observe_deadline();uint64_t end=last_counter+(uint64_t)ms*24000;
    if(end<last_counter || end>=a733_observe_identity.deadline)a733_observe_failure(3);
    do {a733_observe_deadline();}while(last_counter<end);
}
static uint32_t read32(uintptr_t a)
{a733_observe_deadline();__asm__ volatile("dsb sy":::"memory");uint32_t v=*(volatile uint32_t *)a;__asm__ volatile("dsb sy":::"memory");return v;}
static void write32(uintptr_t a,uint32_t v)
{a733_observe_deadline();*(volatile uint32_t *)a=v;__asm__ volatile("dsb sy":::"memory");a733_observe_deadline();}
void a733_usb_prepare(void)
{
    /* Same USB0-only reset/PHY sequence as the exercised PIO adapter. The
     * external admission owns these clocks, pins and controller exclusively. */
    tusb_time_delay_ms_api(150);
    write32(CCU+0x1300,read32(CCU+0x1300)|(1u<<30));
    uint32_t gate=read32(CCU+0x1304)|(1u<<8);
    write32(CCU+0x1304,gate&~(1u<<24));tusb_time_delay_ms_api(1);
    write32(CCU+0x1304,gate|(1u<<24));
    write32(USB+0x420,read32(USB+0x420)|1);
    write32(USB+0x410,(read32(USB+0x410)&~8u)|32u);
}
void a733_usb_poll_enable(_Bool enable){polling=enable;}
void a733_usb_poll(void){a733_observe_deadline();if(polling)dcd_int_handler(0);a733_observe_deadline();}
void a733_usb_trace(uint32_t event,uint32_t first,uint32_t second)
{(void)event;(void)first;(void)second;a733_observe_deadline();}
void a733_usb_assert(void){a733_observe_failure(4);}
