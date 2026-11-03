/* The event of checks/qemu-os/wake-race-main.c, made at the first call of board_service_blocked_until after the application
 * armed it: that call is application_wait_until's, after the caller's test and before the registration of the wait. */
#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "sunstone_app.h"

extern volatile unsigned wake_race_mode;
extern void os_application_usb_changed(void);
extern int os_host_offer(const struct ss_host_request *);
extern int __real_board_service_blocked_until(unsigned service, uint64_t until);
int __wrap_board_service_blocked_until(unsigned service, uint64_t until)
{
    unsigned mode = wake_race_mode;
    if (mode && xTaskGetApplicationTaskTag(NULL)) {
        wake_race_mode = 0;
        if (mode == 1) { struct ss_host_request r; memset(&r, 0, sizeof r); r.op = 16; os_host_offer(&r); }
        else os_application_usb_changed();
    }
    return __real_board_service_blocked_until(service, until);
}
