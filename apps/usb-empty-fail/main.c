/* Fail only after both existing empty-port owners reach their checkpoints. */
#include "usb_empty_fail-app.h"
#include "FreeRTOS.h"
#include "task.h"

/* Present only in the diagnostic image. Its acquired atomic flags record
 * the host at ATTACH and the Type-C owner at LOOK, with no attachment. */
extern int os_usb_failure_ready(void);

void usb_empty_fail_main(const struct ss_grant *grant)
{
    if (grant->devices != SS_DEVICE_USB_KEYBOARD || grant->region_count) ss_fail(0x791);
    while (!os_usb_failure_ready()) {
        ss_progress();
        vTaskDelay(1);
    }
    ss_fail_value(0x790, 3);
}
