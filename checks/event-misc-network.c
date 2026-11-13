/* The production request publications and wait selection run on the real
 * FreeRTOS host kernel. The transport's admitted event source is modeled. */
#include "host_rtos.h"
#include "sunstone_net.h"
#include "FreeRTOS_IP_Private.h"
#include "sunstone_deadline.h"
#include "sunstone_notify.h"
#include "a733_range.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct sunstone_net net,*service;
static unsigned waits,observed,isr_events;
static uint64_t ticket;
QueueHandle_t xNetworkEventQueue;
static int ready_work;
static unsigned capacity_race;
static void ip_capacity_event(struct sunstone_net *);
BaseType_t xSendEventStructToIPTask(const IPStackEvent_t *event,TickType_t wait)
{
    assert(!wait);
    BaseType_t result=xQueueSend(xNetworkEventQueue,event,0);
    if(result!=pdPASS&&capacity_race){
        IPStackEvent_t retired;assert(xQueueReceive(xNetworkEventQueue,&retired,0)==pdPASS);
        ip_capacity_event(&net);capacity_race=0;
    }
    return result;
}
int aic8800_network_storage_valid(const void *pointer,size_t bytes,const struct aic8800_network_config *config)
{(void)pointer;(void)bytes;(void)config;return 1;}
int __real_sunstone_deadline_wait_until(uint64_t);
int __wrap_sunstone_deadline_wait_until(uint64_t until)
{++waits;return __real_sunstone_deadline_wait_until(until);}
#include "network-events.inc"
static uint64_t no_timer(void *unused){(void)unused;return 0;}
static int work_pending(void *unused){(void)unused;return ready_work<0?-1:ready_work!=0;}
static uint64_t crossed;
static uint64_t crossing_timer(void *unused)
{
    (void)unused;
    if(!crossed){crossed=host_time_us()+5;host_advance_us(5);}
    return crossed;
}
static int bind_source(void *unused,TaskHandle_t owner)
{(void)unused;return owner==xTaskGetCurrentTaskHandle();}
static void observe(void *unused)
{(void)unused;assert(!waits&&eTaskGetState(net.worker)==eBlocked);++observed;}
static void submit(void *unused)
{
    (void)unused;net.clients[0].task=xTaskGetCurrentTaskHandle();
    struct sunstone_net_request request={.operation=SUNSTONE_NET_EVENT,.timeout=2};
    assert(sunstone_net_submit(&net,&request,&ticket));
}
static void cancel(void *unused)
{(void)unused;net.clients[0].task=xTaskGetCurrentTaskHandle();assert(sunstone_net_cancel(&net,ticket));}
static void device_irq(void *unused)
{
    (void)unused;BaseType_t woken=pdFALSE;++isr_events;
    sunstone_net_event_from_isr(&woken);portYIELD_FROM_ISR(woken);
}
static void bound(void *unused)
{
    (void)unused;
    static FreeRTOS_Socket_t socket;
    socket.xEventBits=eSOCKET_BOUND;net.sockets[0].socket=&socket;
    socket_event(&socket);
}
static void dequeue(void *unused)
{
    (void)unused;IPStackEvent_t event;
    assert(xQueueReceive(xNetworkEventQueue,&event,0)==pdPASS);ip_capacity_event(&net);
}
static void owner(void *unused)
{
    (void)unused;service=&net;net.initialized=1;net.worker=xTaskGetCurrentTaskHandle();
    net.config.events=(struct sunstone_net_event_source){.bind=bind_source,.deadline_us=no_timer,.work_pending=work_pending};
    net.requests=xQueueCreateStatic(SUNSTONE_NET_CLIENTS,sizeof(struct sunstone_net_message),net.request_storage,&net.request_queue);
    assert(net.requests&&sunstone_deadline_register()&&bind_source(NULL,net.worker));
    host_run_at(1000000,observe,NULL);host_run_at(2000123,submit,NULL);
    assert(worker_wait(&net));
    assert(host_time_us()==2000123&&observed==1&&!waits);
    struct sunstone_net_message message;
    assert(xQueueReceive(net.requests,&message,0)==pdPASS&&message.ticket==ticket&&message.deadline_us==2002123);
    net.pending[0]=(struct sunstone_net_pending){.message=message,.active=1};
    host_run_at(2001100,cancel,NULL);
    assert(worker_wait(&net)&&host_time_us()==2001100&&waits==1&&atomic_load(&net.cancel[0])==ticket);
    assert(!request_expired(&net,&message));
    assert(worker_wait(&net)&&host_time_us()==2002123&&waits==2&&request_expired(&net,&message));
    /* Observe an expired IP fence once, then remain asleep for its event. */
    assert(worker_wait(&net)&&net.pending[0].deadline_seen&&waits==2);
    host_irq_at(2003000,device_irq,NULL);
    assert(worker_wait(&net)&&host_time_us()==2003000&&waits==2&&isr_events==1);
    net.pending[0].active=0;
    host_run_at(2004000,bound,NULL);
    assert(worker_wait(&net)&&host_time_us()==2004000&&atomic_load(&net.sockets[0].bound_event));
    net.config.events.deadline_us=crossing_timer;
    assert(worker_wait(&net)&&host_time_us()==2004005&&net.transport_due_us==crossed);
    assert(!worker_wait(&net));
    net.config.events.deadline_us=no_timer;net.clients[0].task=xTaskGetCurrentTaskHandle();
    atomic_store(&net.clients[0].pending,0);
    struct sunstone_net_request oversized={.operation=SUNSTONE_NET_EVENT,.timeout=portMAX_DELAY/2};
    uint64_t refused=0;
    assert(!sunstone_net_submit(&net,&oversized,&refused)&&!refused&&!uxQueueMessagesWaiting(net.requests));
    /* A bounded pass leaves ready work visible even when hints coalesce. */
    ready_work=2;
    while(ready_work){assert(worker_wait(&net)&&host_time_us()==2004005);--ready_work;}
    ready_work=-1;assert(!worker_wait(&net));ready_work=0;
    StaticQueue_t ip_queue;unsigned char ip_storage[sizeof(IPStackEvent_t)];
    xNetworkEventQueue=xQueueCreateStatic(1,sizeof(IPStackEvent_t),ip_storage,&ip_queue);
    IPStackEvent_t occupied={eNoEvent,NULL},event;
    assert(xQueueSend(xNetworkEventQueue,&occupied,0)==pdPASS);
    static FreeRTOS_Socket_t binding;
    struct sunstone_net_socket record={.socket=&binding};
    const uint8_t address[4]={10,0,0,1};
    assert(!bind_start(&net,0,&record,address,9000)&&atomic_load(&net.ip_waiters)==1);
    host_run_at(2005000,dequeue,NULL);
    assert(worker_wait(&net)&&host_time_us()==2005000&&!atomic_load(&net.ip_waiters));
    assert(bind_start(&net,0,&record,address,9000));
    assert(xQueueReceive(xNetworkEventQueue,&event,0)==pdPASS&&event.eEventType==eSocketBindEvent&&event.pvData==&binding);
    assert(xQueueSend(xNetworkEventQueue,&occupied,0)==pdPASS);
    event=(IPStackEvent_t){eSocketCloseEvent,&binding};
    assert(!ip_publish(&net,1,&event)&&atomic_load(&net.ip_waiters)==2);
    host_run_at(2006000,dequeue,NULL);
    assert(worker_wait(&net)&&host_time_us()==2006000&&!atomic_load(&net.ip_waiters));
    assert(ip_publish(&net,1,&event)&&xQueueReceive(xNetworkEventQueue,&event,0)==pdPASS&&event.eEventType==eSocketCloseEvent);
    assert(xQueueSend(xNetworkEventQueue,&occupied,0)==pdPASS);capacity_race=1;
    assert(!bind_start(&net,0,&record,address,9000)&&!atomic_load(&net.ip_waiters));
    assert(worker_wait(&net)&&host_time_us()==2006000);
    assert(bind_start(&net,0,&record,address,9000));
}
int main(void)
{
    host_timer_handler(sunstone_deadline_irq);host_rtos_run(owner,NULL,4,6000000);
    puts("PASS network idle, request/cancel/IRQ publication, exact request expiry and pending-fence sleep");
}
