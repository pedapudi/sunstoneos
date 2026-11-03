/* Test application of checks/qemu-os-boot.py for the OS's wake-ups that arrive between a test and the registration of a wait.
 * The input region holds a mode (u64).
 *   mode 0: a host request is offered after ss_host_recv found the mailbox empty and before it registered its wait.
 *   mode 1: the USB state changes (os_application_usb_changed) after ss_usb_wait was called and before it registered.
 * The wrapper in wake-race.c makes the event happen at that point. Each call must end at the event, not at its 2 s timeout.
 * Result {tag, call result, elapsed us}. */
#include "wake_race-app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "sunstone_deadline.h"

volatile unsigned wake_race_mode;     /* 0: not armed, else 1 + the mode of the event the wrapper makes */

void wake_race_main(const struct ss_grant *grant)
{
    const uint64_t mode = ((const uint64_t *)grant->regions[WAKE_RACE_REGION_INPUT].base)[0];
    const uint64_t tag = UINT64_C(0x57414b45);
    struct ss_host_request request;
    int result = 0;
    wake_race_mode = (unsigned)mode + 1;
    uint64_t t0 = sunstone_deadline_now_us();
    if (mode == 0) result = ss_host_recv(&request, 2000000u);
    else { ss_usb_wait(2000000u); result = 1; }
    const uint64_t e = sunstone_deadline_now_us() - t0;
    const uint64_t out[3] = {tag, (uint64_t)(int64_t)result, e};
    ss_finish(out, sizeof out);
}
