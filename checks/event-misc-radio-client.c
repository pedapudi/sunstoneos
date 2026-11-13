#include "host_rtos.h"
#include "sunstone_deadline.h"
#include "sunstone_notify.h"
#include "a733_range.h"
#include "radio.h"
#include <assert.h>
#include <stdio.h>
static struct board_radio radio;
static struct sunstone_net *service;
static struct board_radio_profile profile;
static unsigned waits,observed;
int os_radio_client_blocked_until(uint64_t until) __attribute__((weak));
int os_radio_client_blocked_until(uint64_t until){return until>host_time_us();}
unsigned sunstone_deadline_notify_index(void)
{return xTaskGetApplicationTaskTag(NULL)?SS_NOTIFY_DEADLINE:SS_NOTIFY_OWN;}
int __real_sunstone_deadline_wait_until(uint64_t);
int __wrap_sunstone_deadline_wait_until(uint64_t until)
{if(xTaskGetCurrentTaskHandle()==radio.config.network.clients[0])++waits;return __real_sunstone_deadline_wait_until(until);}
static int request_expired(const struct sunstone_net *s,const struct sunstone_net_message *m)
{(void)s;(void)m;return 0;}
static void dns_cancel(struct sunstone_net *s,unsigned client){(void)s;(void)client;assert(0);}
void aic8800_network_fault(struct aic8800_network *n){(void)n;assert(0);}
int aic8800_network_storage_valid(const void *p,size_t bytes,const struct aic8800_network_config *c)
{(void)p;(void)bytes;(void)c;return 1;}
int aic8800_network_address(const struct aic8800_network *n,uint8_t out[4])
{(void)n;(void)out;return 1;}
#include "radio-client-events.inc"
static int source_bind(void *unused,TaskHandle_t owner){(void)unused;(void)owner;return 1;}
static void hint(void *unused)
{(void)unused;client_ready(&radio.session.sockets,0);}
static void observe(void *unused)
{(void)unused;assert(waits==1&&eTaskGetState(radio.config.network.clients[0])==eBlocked);++observed;}
static void reply_event(void *unused)
{
    (void)unused;
    struct sunstone_net_pending pending={.active=1,.message.client=0};
    struct sunstone_net_reply reply={.result=SUNSTONE_NET_OK};
    publish_reply(&radio.session.sockets,&pending,&reply);assert(!pending.active);
}
static void resume_event(void *unused)
{(void)unused;assert(sunstone_net_resume(&radio.session.sockets));}
static void resume_provider(void *unused)
{
    (void)unused;assert(sunstone_deadline_register());
    while(host_time_us()<3100000)assert(sunstone_deadline_wait_until(3100000)!=SUNSTONE_DEADLINE_ERROR);
    resume_event(NULL);vTaskSuspend(NULL);
}
static void release_gate(void *unused)
{(void)unused;client_gate_leave(&radio.session.sockets);}
static BaseType_t tag(void *unused){(void)unused;return pdPASS;}
static void owner(void *argument)
{
    TaskHandle_t task=xTaskGetCurrentTaskHandle();if(argument)vTaskSetApplicationTaskTag(NULL,tag);
    radio.constructed=1;radio.config.scan_only=1;radio.config.network.clients[0]=task;
    radio.platform.config.lifetime_deadline_us=5000000;
    struct sunstone_net *net=&radio.session.sockets;net->initialized=1;service=net;
    net->clients[0].task=task;net->config.events.bind=source_bind;
    net->owned_poll=1;
    net->requests=xQueueCreateStatic(SUNSTONE_NET_CLIENTS,sizeof(struct sunstone_net_message),net->request_storage,&net->request_queue);
    net->clients[0].reply_queue=xQueueCreateStatic(1,sizeof(struct sunstone_net_reply),net->clients[0].storage,&net->clients[0].queue);
    host_run_at(1000000,observe,NULL);host_run_at(2000000,hint,NULL);
    assert(board_radio_client_wait(&radio)==1&&host_time_us()==2000000&&observed==1);
    radio.config.scan_only=0;radio.startup_profile=&profile;profile.client_deadline_us=4000000;
    atomic_store(&radio.network_started,1);radio.client_pending=1;
    host_run_at(2500000,reply_event,NULL);
    assert(board_radio_client_wait(&radio)==1&&host_time_us()==2500000&&waits==2);
    /* A ready copied reply must not wait for another event. */
    assert(board_radio_client_wait(&radio)==1&&waits==2);
    struct sunstone_net_reply reply;assert(xQueueReceive(net->clients[0].reply_queue,&reply,0)==pdPASS);
    radio.client_pending=0;radio.client_retry=1;
    host_run_at(3000000,hint,NULL); /* Actual client-ready helper used after request dequeue. */
    assert(board_radio_client_wait(&radio)==1&&host_time_us()==3000000&&!radio.client_retry);
    assert(board_radio_client_wait(&radio)==1&&waits==3); /* Ready software admission. */
    struct sunstone_net_request request={.operation=SUNSTONE_NET_EVENT,.timeout=2};uint64_t ticket;
    atomic_store(&net->offline,1);radio.client_retry=1;
    assert(!sunstone_net_submit(net,&request,&ticket));
    net->config.bootstrap=host_task_create("resume",resume_provider,NULL,5);
    assert(board_radio_client_wait(&radio)==1&&host_time_us()==3100000);
    assert(sunstone_net_submit(net,&request,&ticket));
    struct sunstone_net_message message;assert(xQueueReceive(net->requests,&message,0)==pdPASS);atomic_store(&net->clients[0].pending,0);
    atomic_store(&net->client_gate,1);radio.client_retry=1;
    assert(!sunstone_net_submit(net,&request,&ticket));host_run_at(3200000,release_gate,NULL);
    assert(board_radio_client_wait(&radio)==1&&host_time_us()==3200000);
    assert(sunstone_net_submit(net,&request,&ticket));assert(xQueueReceive(net->requests,&message,0)==pdPASS);atomic_store(&net->clients[0].pending,0);
    atomic_store(&radio.client_status,1);host_run_at(4500000,hint,NULL);
    assert(board_radio_client_wait(&radio)==1&&host_time_us()==4500000&&waits==6);
}
int main(int argc,char **argv)
{
    (void)argv;host_timer_handler(sunstone_deadline_irq);
    host_rtos_run(owner,(void *)(uintptr_t)(argc>1),4,6000000);
    puts("PASS radio client sleeps for reply/capacity/quiesce events and preserves ready work");
}
