/* Test of checks/qemu-os-boot.py: the USB transport becomes ready after the main task has found the application not live
 * and before it has registered its wait. The wake-up of that moment finds no waiter. The main task must still see the
 * transport ready and start; it must not sleep until the session deadline. The wrapper makes the transport ready on the
 * main task's first call of board_service_blocked_until, which application_wait_until makes after ss_live() answered 0
 * and before it registers the wait. */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

extern void os_application_usb_ready(void);
extern int __real_board_service_blocked_until(unsigned service, uint64_t until);
int __wrap_board_service_blocked_until(unsigned service, uint64_t until)
{
    static int done;
    if (!done && xTaskGetApplicationTaskTag(NULL)) {
        done = 1;
        os_application_usb_ready();
    }
    return __real_board_service_blocked_until(service, until);
}
