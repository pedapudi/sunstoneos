#include "session.h"
#include "cached.h"
#include "a733_storage_update_native.h"
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
static struct a733_storage_native_boot boot;
static struct a733_maintenance_transfer transfer;
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,2);}
void a733_storage_maintenance_stop(void)
{a733_maintenance_revoke(a733_maintenance_native_lease());}
void a733_smp_diagnostic_start(void)
{
    uint8_t raw[A733_STORAGE_SESSION_BYTES];
    volatile const uint8_t *source=(void *)A733_STORAGE_SESSION_ADDRESS;
    if(!a733_smp_device_preparation_admitted())platform_panic("storage bootstrap",1);
    for(unsigned i=0;i<sizeof(raw);++i)raw[i]=source[i];
    __asm__ volatile("dmb sy":::"memory");
    if(!a733_storage_session_decode(&boot,&transfer,raw,a733_smp_counter()) ||
       transfer.epoch!=a733_cached_runtime[0].epoch ||
       boot.grant.deadline_us*24!=a733_cached_runtime[0].diagnostic_deadline ||
       !a733_storage_native_start(&boot,a733_maintenance_native_lease(),&transfer))
        platform_panic("storage session",2);
    vTaskStartScheduler();platform_panic("scheduler returned",3);
}
