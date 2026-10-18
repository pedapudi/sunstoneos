#include "FreeRTOS.h"
#include "platform.h"
#include "result.h"
#include "boot.h"
#include "a733_usb.h"
#include "device/dcd.h"
static unsigned polling;
static uint64_t last_counter;
_Noreturn void a733_observe_failure(uint64_t detail)
{(void)detail;platform_panic("NPU USB retained",1);}
void a733_observe_deadline(void)
{
    uint64_t n=a733_smp_counter();
    if(n<last_counter||n>=a733_observe_identity.deadline)a733_observe_failure(1);
    last_counter=n;
}
uint32_t tusb_time_millis_api(void)
{a733_observe_deadline();return (uint32_t)(last_counter/24000);}
void tusb_time_delay_ms_api(uint32_t ms)
{
    if(ms>150)a733_observe_failure(2);
    a733_observe_deadline();uint64_t end=last_counter+(uint64_t)ms*24000;
    if(end>=a733_observe_identity.deadline)a733_observe_failure(3);
    do{a733_observe_deadline();}while(last_counter<end);
}
void a733_usb_prepare(void)
{
    extern int npu_single_core_usb_poll(void);
    if(npu_single_core_usb_poll()!=1)a733_observe_failure(4);
}
void a733_usb_poll_enable(_Bool enabled){polling=enabled;}
void a733_usb_poll(void){a733_observe_deadline();if(polling)dcd_int_handler(0);a733_observe_deadline();}
void a733_usb_trace(uint32_t e,uint32_t a,uint32_t b){(void)e;(void)a;(void)b;a733_observe_deadline();}
void a733_usb_assert(void){a733_observe_failure(5);}
