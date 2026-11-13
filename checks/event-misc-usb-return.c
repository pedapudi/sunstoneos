/* Production USB return wait on the host kernel and deadline runtime. */
#include "host_rtos.h"
#include "sunstone_deadline.h"
#include <assert.h>
#include <stdio.h>
static struct {struct {uint64_t deadline_us;} session;
    struct {struct {struct {uint64_t lifetime_deadline;} config;} platform;} host;} board;
static TaskHandle_t task;
static unsigned waits;
static void usb_fail(void){assert(0);}
int __real_sunstone_deadline_wait_until(uint64_t);
int __wrap_sunstone_deadline_wait_until(uint64_t until)
{++waits;return __real_sunstone_deadline_wait_until(until);}
#include "usb-return-events.inc"
static void observe(void *unused)
{(void)unused;assert(waits==1&&eTaskGetState(task)==eBlocked);}
static void publish(void *unused)
{(void)unused;xTaskNotifyGive(task);}
static void owner(void *unused)
{
    (void)unused;task=xTaskGetCurrentTaskHandle();board.session.deadline_us=5000000;
    assert(sunstone_deadline_register());
    host_run_at(500000,observe,NULL);host_run_at(1000000,publish,NULL);
    usb_return_wait(0);
    assert(host_time_us()==1000000&&waits==1);
    usb_return_wait(1001000);
    assert(host_time_us()==1000050&&waits==2);
    board.host.platform.config.lifetime_deadline=5000000;
    usb_poll_wait();assert(host_time_us()==1001050&&waits==3);
}
int main(void)
{
    host_timer_handler(sunstone_deadline_irq);host_rtos_run(owner,NULL,4,6000000);
    puts("PASS USB unpublished deadline sleeps for publication and no-IRQ retirement retries in 50 us");
}
