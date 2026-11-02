/* SPDX-License-Identifier: Apache-2.0 */
/* Actual owner waiter and common deadline runtime. GIC, compare and task
 * scheduling are modeled; device completion remains an independent event. */
#define SUNSTONE_BOARD_APPLICATION_PRIVATE_H
#define SUNSTONE_BOARD_HOST_DEADLINE 1
#define SUNSTONE_A733_OS 1
#ifndef configMAX_API_CALL_INTERRUPT_PRIORITY
#define configMAX_API_CALL_INTERRUPT_PRIORITY 11
#endif
#ifndef portPRIORITY_SHIFT
#define portPRIORITY_SHIFT 4
#endif
#include "a733_usb2_native.h"
#include "sunstone_deadline.h"
#include "gicv3_spi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host_rtos.h"
enum {BOARD_SERVICE_USB2=4};
static struct {void *handles[8];struct a733_usb2_native host;} board;
static unsigned waits,tick_waits;
static int board_live(void *context){(void)context;return 1;}
int board_service_deadline_register(unsigned service)
{assert(service==BOARD_SERVICE_USB2);return sunstone_deadline_register();}
int board_service_blocked_until(unsigned service,uint64_t budget)
{assert(service==BOARD_SERVICE_USB2&&budget==10000000&&budget>host_time_us());return 1;}
/* The GIC masks and rearms under the kernel critical section or in the handler. */
int gicv3_spi_mask(const struct gicv3_spi *source){assert((host_critical_depth()||host_in_isr())&&source->intid==187);return 1;}
int gicv3_spi_rearm(const struct gicv3_spi *source){assert((host_critical_depth()||host_in_isr())&&source->intid==187);return 1;}
int a733_xhci_interrupt_start(struct a733_xhci *core){(void)core;return 1;}
int a733_xhci_interrupt_begin(struct a733_xhci *core){(void)core;return 1;}
int a733_xhci_interrupt_finish(struct a733_xhci *core){(void)core;return 1;}
#ifndef HOST_IRQ_SOURCE
#define HOST_IRQ_SOURCE "../apps/board/host_irq.c"
#endif
#include HOST_IRQ_SOURCE
/* Every blocking notification wait must be the precise, unbounded one, taken
 * outside any critical section. */
uint32_t __real_ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t ticks);
uint32_t __wrap_ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t ticks)
{
    if(ticks&&xTaskGetCurrentTaskHandle()==board.handles[4]){
        assert(!host_critical_depth());++waits;
        if(ticks!=portMAX_DELAY)++tick_waits;
        assert(ticks==portMAX_DELAY);
    }
    return __real_ulTaskGenericNotifyTake(index,clear,ticks);
}
static void device_irq(void *unused){(void)unused;board_host_irq();}
static TaskHandle_t owner;
static void progress(void)
{sunstone_deadline_owner_progress(owner);memset(&irq,0,sizeof(irq));irq.bound=irq.armed=1;}
static void test(void *unused)
{
    (void)unused;
    struct sunstone_deadline_record record;
    assert(owner==xTaskGetCurrentTaskHandle());
    assert(board_host_irq_deadline_init());progress();
    uint64_t start=host_time_us();
    host_irq_at(start+73,device_irq,NULL);
    board_host_irq_schedule(start+347,0);board_host_irq_wait();
    assert(host_time_us()==start+73&&waits==1&&!tick_waits);
    assert(sunstone_deadline_owner_record(owner,&record)&&!record.armed&&!record.failed&&record.woken_us==host_time_us());
    progress();start=host_time_us();board_host_irq_schedule(start+347,0);board_host_irq_wait();
    assert(host_time_us()==start+347&&sunstone_deadline_owner_record(owner,&record)&&record.woken_us==host_time_us()&&!record.armed);
    progress();
    start=host_time_us();board_host_irq_schedule(start+17,1);
    unsigned before=waits;for(unsigned i=0;i<31;++i)board_host_irq_wait();assert(waits==before);
    board_host_irq_wait();assert(host_time_us()==start+17&&waits==before+1);
    progress();start=host_time_us();board_host_irq_schedule(start+100000,1);
    for(unsigned i=0;i<32;++i)board_host_irq_wait();assert(host_time_us()==start+50);
    progress();start=host_time_us();irq.deferred=1;
    board_host_irq_schedule(start+100000,0);board_host_irq_wait();
    assert(host_time_us()==start+50); /* Delayed GIC acknowledgment needs no tick. */
    progress();board_host_irq_schedule(host_time_us()+7777,0);host_isr(device_irq,NULL);
    start=host_time_us();board_host_irq_wait();assert(host_time_us()==start); /* Arrival before arm is retained. */
    progress();board_host_irq_schedule(host_time_us(),0);before=waits;board_host_irq_wait();assert(waits==before);
    board_host_irq_schedule(0,0);board_host_irq_wait();assert(irq.fault&&!host_critical_depth());
    assert(!host_critical_depth()&&!tick_waits);puts("PASS actual precise waiter/runtime: 73us IRQ, 347us expiry, bounded batch, hard deadline, pending-before-arm");
}
int main(void)
{
    host_timer_handler(sunstone_deadline_irq);
    host_set_time_us(1000);
    owner=host_rtos_prepare(test,NULL,2,1000000);
    board.handles[4]=owner;board.host.platform.config.lifetime_deadline=10000000;
    host_rtos_start();
    return 0;
}
