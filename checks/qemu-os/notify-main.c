/* Test application of checks/qemu-os-boot.py for the OS's wake-ups of an application task (docs/notifications.md).
 * The input region holds a mode (u64).
 *   mode 0: a poker task gives the main task the notifications that the OS gives it when the USB transport becomes
 *           ready and when the live state changes (os_application_usb_ready, os_application_usb_changed) while the main
 *           task sleeps in the deadline wait and in ss_host_recv. Each must end at its own condition: the deadline,
 *           and a request or the timeout. Result {tag, deadline status, elapsed us, ss_host_recv result, elapsed us}.
 *   mode 1: the same OS notifications, and a task of the application gives the main task index 0 once while it
 *           sleeps in ss_host_recv. The give must still be there when the call returns: {tag, recv, elapsed us, take}.
 *   mode 2: a task of the application gives the main task index 0 once during the deadline wait. The wait must
 *           expire, and the give must still be there afterwards: {tag, status, elapsed us, take}.
 *   mode 3: a device hint ends the deadline wait early. The next wait of the application on index 0 must then time
 *           out: {tag, status, elapsed us, take, elapsed us of the take}. */
#include "notify_test-app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "application.h"
#include "sunstone_deadline.h"
#include "sunstone_notify.h"

void os_application_usb_changed(void);
#define WAIT_US 30000u
static StaticTask_t poker_state, giver_state;
static StackType_t poker_stack[512], giver_stack[512];
static TaskHandle_t main_handle;
static volatile unsigned giver_index, giver_delay_ms;

static void poker(void *unused)
{
    (void)unused;
    for (;;) {
        os_application_usb_ready();
        os_application_usb_changed();
        vTaskDelay(1);
    }
}

static void giver(void *unused)
{
    (void)unused;
    vTaskDelay(pdMS_TO_TICKS(giver_delay_ms));
    xTaskNotifyGiveIndexed(main_handle, giver_index);
    for (;;) vTaskDelay(1000);
}

static uint64_t now_us(void) { return sunstone_deadline_now_us(); }

void notify_test_main(const struct ss_grant *grant)
{
    const uint64_t mode = ((const uint64_t *)grant->regions[NOTIFY_TEST_REGION_INPUT].base)[0];
    const uint64_t tag = UINT64_C(0x4e4f54494659);
    main_handle = xTaskGetCurrentTaskHandle();
    if (mode <= 1) xTaskCreateStatic(poker, "poker", 512, NULL, 3, poker_stack, &poker_state);
    if (mode >= 1) {
        giver_index = mode == 3 ? SS_NOTIFY_DEADLINE : SS_NOTIFY_OWN;
        giver_delay_ms = mode == 3 ? 5 : 10;
        xTaskCreateStatic(giver, "giver", 512, NULL, 3, giver_stack, &giver_state);
    }
    vTaskDelay(5);
    struct ss_host_request request;
    uint64_t t0 = now_us();
    if (mode == 0) {
        int wait = sunstone_deadline_wait_until(t0 + WAIT_US);
        uint64_t e1 = now_us() - t0;
        t0 = now_us();
        int recv = ss_host_recv(&request, WAIT_US);
        uint64_t e2 = now_us() - t0;
        const uint64_t result[5] = {tag, (uint64_t)wait, e1, (uint64_t)(int64_t)recv, e2};
        ss_finish(result, sizeof result);
    }
    if (mode == 1) {
        int recv = ss_host_recv(&request, WAIT_US);
        uint64_t e = now_us() - t0;
        uint64_t take = ulTaskNotifyTake(pdTRUE, 0);
        const uint64_t result[4] = {tag, (uint64_t)(int64_t)recv, e, take};
        ss_finish(result, sizeof result);
    }
    if (mode == 2) {
        int wait = sunstone_deadline_wait_until(t0 + WAIT_US);
        uint64_t e = now_us() - t0;
        uint64_t take = ulTaskNotifyTake(pdTRUE, 0);
        const uint64_t result[4] = {tag, (uint64_t)wait, e, take};
        ss_finish(result, sizeof result);
    }
    int wait = sunstone_deadline_wait_until(t0 + 20000u);        /* mode 3 */
    uint64_t e = now_us() - t0;
    uint64_t t1 = now_us();
    uint64_t take = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(40));
    uint64_t e3 = now_us() - t1;
    const uint64_t result[5] = {tag, (uint64_t)wait, e, take, e3};
    ss_finish(result, sizeof result);
}
