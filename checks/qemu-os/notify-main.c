/* Test application of checks/qemu-os-boot.py for the OS's wake-ups of an application task (docs/notifications.md).
 * A worker of the application gives the main task the notifications that the OS gives it when the USB transport
 * becomes ready and when the live state changes (os_application_usb_ready, os_application_usb_changed). While it does,
 * the main task sleeps in the deadline wait, which the application owns, and in ss_host_recv, an SDK call that
 * sleeps in an OS wait. Each must end at its own condition: the deadline, and a request or the timeout.
 * The result is {tag, deadline wait status, its elapsed microseconds, ss_host_recv result, its elapsed microseconds}. */
#include "notify_test-app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "application.h"
#include "sunstone_deadline.h"
void os_application_usb_changed(void);

#define WAIT_US 30000u
static StaticTask_t poker_state;
static StackType_t poker_stack[512];

static void poker(void *unused)
{
    (void)unused;
    for (;;) {
        os_application_usb_ready();
        os_application_usb_changed();
        vTaskDelay(1);
    }
}

void notify_test_main(const struct ss_grant *grant)
{
    (void)grant;
    xTaskCreateStatic(poker, "poker", 512, NULL, 3, poker_stack, &poker_state);
    vTaskDelay(5);                                        /* the poker is running */
    uint64_t t0 = sunstone_deadline_now_us();
    int wait = sunstone_deadline_wait_until(t0 + WAIT_US);
    uint64_t e1 = sunstone_deadline_now_us() - t0;
    struct ss_host_request request;
    t0 = sunstone_deadline_now_us();
    int recv = ss_host_recv(&request, WAIT_US);
    uint64_t e2 = sunstone_deadline_now_us() - t0;
    const uint64_t result[5] = {UINT64_C(0x4e4f54494659), (uint64_t)wait, e1, (uint64_t)(int64_t)recv, e2};
    ss_finish(result, sizeof result);
}
