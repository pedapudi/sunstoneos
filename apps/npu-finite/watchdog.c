#include "watchdog.h"
#define MODE UINT32_C(0x02050014)
#define KEY UINT32_C(0x16aa0000)
int npu_finite_watchdog_admitted(void)
{
    return (npu_finite_watchdog_read(MODE)&0xffff)==NPU_FINITE_WATCHDOG_MODE &&
        (npu_finite_watchdog_read(UINT32_C(0x02050010))&0xffff)==1 &&
        (npu_finite_watchdog_read(UINT32_C(0x02050018))&0xffff)==2;
}
__attribute__((noinline)) int npu_finite_watchdog_refresh(uint64_t now,uint64_t deadline,unsigned failed)
{
    if(failed || now>=deadline || !npu_finite_watchdog_admitted())return 0;
    npu_finite_watchdog_write(UINT32_C(0x0205000c),UINT32_C(0x14af));return 1;
}
uint32_t npu_finite_watchdog_disarm(void)
{
    npu_finite_watchdog_write(MODE,KEY);
    return npu_finite_watchdog_read(MODE)&0xffff;
}
void npu_finite_watchdog_stop(void)
{
    if(npu_finite_watchdog_read(MODE)&1) {
        npu_finite_watchdog_write(MODE,KEY|1);
        npu_finite_watchdog_write(UINT32_C(0x0205000c),UINT32_C(0x14af));
    }
}
#ifndef SUNSTONE_NPU_WATCHDOG_TEST
#include "a733_npu_application.h"
#include "boot.h"
#include "result.h"
#include "sunstone_crumb.h"
/* Runtime behavior, not reset-route qualification. Active NPU SRAM ownership
 * across a WDT0 reset still needs independent physical evidence. */
__attribute__((used,retain)) const uint64_t os_cpu_return_watchdog[5]={
    UINT64_C(0x31544457534f5243),2,NPU_FINITE_WATCHDOG_MODE,SUNSTONE_CRUMB_RTC,20
};
static StaticTask_t supervisor_state;
static StackType_t supervisor_stack[512] __attribute__((aligned(16)));
static void supervisor(void *unused)
{
    (void)unused;
    TickType_t wake=xTaskGetTickCount();
    for(;;) {
        if(!npu_finite_watchdog_refresh(a733_smp_counter(),a733_observe_identity.deadline,
            atomic_load_explicit(&a733_npu_application_failed,memory_order_acquire))) {
            vTaskSuspend(NULL);
            for(;;)__asm__ volatile("wfe");
        }
        vTaskDelayUntil(&wake,pdMS_TO_TICKS(20));
    }
}
int npu_finite_watchdog_start(void)
{
    configASSERT(pdMS_TO_TICKS(20)>0);
    return xTaskCreateStaticAffinitySet(supervisor,"npu-wdt",512,NULL,configMAX_PRIORITIES-1,
        supervisor_stack,&supervisor_state,1)!=NULL;
}
#endif
