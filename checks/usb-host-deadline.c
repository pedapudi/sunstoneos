/* SPDX-License-Identifier: Apache-2.0 */
/* Actual owner waiter and common deadline runtime. GIC, compare and task
 * scheduling are modeled; device completion remains an independent event. */
#define SUNSTONE_BOARD_APPLICATION_PRIVATE_H
#define SUNSTONE_BOARD_HOST_DEADLINE 1
#define SUNSTONE_A733_OS 1
#include "a733_usb2_native.h"
#include "sunstone_deadline.h"
#include "gicv3_spi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef void *TaskHandle_t;
typedef unsigned UBaseType_t;
typedef int BaseType_t;
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY UINT64_MAX
#define taskSCHEDULER_NOT_STARTED 0
#define configMAX_API_CALL_INTERRUPT_PRIORITY 11
#define portPRIORITY_SHIFT 4
enum {BOARD_SERVICE_USB2=4};
static struct {void *handles[8];struct a733_usb2_native host;} board;
static unsigned critical,hints,waits,tick_waits,rearm,gate_held;
static uint64_t counter_us=1000,compare,device_irq_at;
static void enter(void){assert(!critical);critical=1;}
static void leave(void)
{assert(critical);critical=0;if(rearm){rearm=0;sunstone_deadline_rearm();}}
#define taskENTER_CRITICAL() enter()
#define taskEXIT_CRITICAL() leave()
#define taskENTER_CRITICAL_FROM_ISR() (enter(),0)
#define taskEXIT_CRITICAL_FROM_ISR(saved) do{(void)(saved);leave();}while(0)
#define portYIELD_FROM_ISR(woken) ((void)(woken))
static TaskHandle_t xTaskGetCurrentTaskHandle(void){return (void *)1;}
static int xTaskGetSchedulerState(void){return 1;}
static int board_live(void *context){(void)context;return 1;}
static void vTaskNotifyGiveFromISR(void *owner,BaseType_t *woken)
{assert(critical&&owner==(void *)1);++hints;*woken=1;}
static unsigned ulTaskNotifyTake(unsigned,uint64_t);
uint64_t sunstone_deadline_now_us(void){return counter_us;}
void sunstone_deadline_kick(void){assert(critical);rearm=1;}
void sunstone_deadline_program(uint64_t until){compare=until;}
#include "deadline-native.inc"
int board_service_deadline_register(unsigned service)
{assert(service==BOARD_SERVICE_USB2);return sunstone_deadline_register();}
int board_service_blocked_until(unsigned service,uint64_t budget)
{assert(service==BOARD_SERVICE_USB2&&budget==10000000&&budget>counter_us);return 1;}
int gicv3_spi_mask(const struct gicv3_spi *source){assert(critical&&source->intid==187);return 1;}
int gicv3_spi_rearm(const struct gicv3_spi *source){assert(critical&&source->intid==187);return 1;}
int a733_xhci_interrupt_start(struct a733_xhci *core){(void)core;return 1;}
int a733_xhci_interrupt_begin(struct a733_xhci *core){(void)core;return 1;}
int a733_xhci_interrupt_finish(struct a733_xhci *core){(void)core;return 1;}
#include "../apps/board/host_irq.c"
static unsigned ulTaskNotifyTake(unsigned clear,uint64_t ticks)
{
    assert(clear==pdTRUE);
    if(!ticks){assert(critical);unsigned count=hints;hints=0;return count;}
    assert(!critical&&!gate_held);++waits;
    if(ticks!=portMAX_DELAY){++tick_waits;assert(0);}
    if(!hints){
        assert(compare>counter_us);
        if(device_irq_at&&device_irq_at<compare){counter_us=device_irq_at;device_irq_at=0;board_host_irq();}
        else{counter_us=compare;sunstone_deadline_irq();}
    }
    assert(hints);unsigned count=hints;hints=0;return count;
}
static void progress(void)
{sunstone_deadline_owner_progress((void *)1);memset(&irq,0,sizeof(irq));irq.bound=irq.armed=1;}
int main(void)
{
    struct sunstone_deadline_record record;
    board.handles[4]=(void *)1;board.host.platform.config.lifetime_deadline=10000000;
    assert(board_host_irq_deadline_init());progress();
    uint64_t start=counter_us;device_irq_at=start+73;
    board_host_irq_schedule(start+347,0);board_host_irq_wait();
    assert(counter_us==start+73&&waits==1&&!tick_waits);
    assert(sunstone_deadline_owner_record((void *)1,&record)&&!record.armed&&!record.failed&&record.woken_us==counter_us);
    progress();start=counter_us;board_host_irq_schedule(start+347,0);board_host_irq_wait();
    assert(counter_us==start+347&&sunstone_deadline_owner_record((void *)1,&record)&&record.woken_us==counter_us&&!record.armed);
    progress();
    start=counter_us;board_host_irq_schedule(start+17,1);
    unsigned before=waits;for(unsigned i=0;i<31;++i)board_host_irq_wait();assert(waits==before);
    board_host_irq_wait();assert(counter_us==start+17&&waits==before+1);
    progress();start=counter_us;board_host_irq_schedule(start+100000,1);
    for(unsigned i=0;i<32;++i)board_host_irq_wait();assert(counter_us==start+50);
    progress();board_host_irq_schedule(counter_us+7777,0);board_host_irq();
    start=counter_us;board_host_irq_wait();assert(counter_us==start); /* Arrival before arm is retained. */
    progress();board_host_irq_schedule(counter_us,0);before=waits;board_host_irq_wait();assert(waits==before);
    assert(!critical&&!tick_waits);puts("PASS actual precise waiter/runtime: 73us IRQ, 347us expiry, bounded batch, hard deadline, pending-before-arm");
}
