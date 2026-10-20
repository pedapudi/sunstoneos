#include "application.h"
#include "session.h"
#include "boot.h"
#include "cached.h"
#include "platform.h"
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,0);}
void a733_smp_diagnostic_start(void)
{
    configASSERT(a733_smp_device_preparation_admitted() && npu_session_copy() &&
        a733_npu_application_observer_admit() && accelerator_gpu_package());
    TaskHandle_t npu=accelerator_npu_client_create();
    /* The compact NPU constructor requires the first 790 WB pages. GPU firmware,
     * payload and page tables must be allocated after this permanent parent. */
    configASSERT(npu && a733_npu_application_init(npu));
    configASSERT(accelerator_gpu_prepare());
    accelerator_usb_start();
    vTaskStartScheduler();platform_panic("accelerator scheduler returned",0);
}
