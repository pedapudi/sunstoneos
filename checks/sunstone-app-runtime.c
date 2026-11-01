#include "os.h"
#include "application.h"
#include "../apps/board/application.h"
#include "session-fixture.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
void __assert_func(const char *file,int line,const char *function,const char *expression)
{(void)file;(void)line;(void)function;(void)expression;__builtin_trap();}
static int live=1;
static unsigned notified,waits,offer_on_handle,offer_on_wait;
static struct ss_host_request injected={.op=1,.count=1};
static const struct ss_manifest manifest={.host_op_first=0,.host_op_count=5,.priority=2,.stack_bytes=4096,
    .return_reserve_us=5000000};
static StackType_t main_stack[4096/sizeof(StackType_t)];static StaticTask_t main_state;
static void app_main(const struct ss_grant *g){(void)g;__builtin_trap();}
const struct ss_application ss_application={.manifest=&manifest,.main=app_main,.stack=main_stack,.task=&main_state};
struct board_application board;
const struct os_session *os_session_admitted(void){return (const void *)service_fixture;}
uint32_t os_session_devices(void){return 0;}
const struct ss_input_fact *os_session_inputs(unsigned *n){*n=0;return NULL;}
uint64_t a733_smp_counter(void){return 24000000;}
void board_service_progress(unsigned n){(void)n;}
void board_fail(void *p){(void)p;__builtin_trap();}
void vTaskSuspend(TaskHandle_t t){(void)t;__builtin_trap();}
void runtime_smp_init(void){}
void runtime_init(void){}
TaskHandle_t xTaskCreateStaticAffinitySet(TaskFunction_t entry,const char *name,configSTACK_DEPTH_TYPE depth,
    void *context,UBaseType_t priority,StackType_t *stack,StaticTask_t *state,UBaseType_t affinity)
{
    (void)entry;(void)name;assert(!context&&depth==4096/sizeof(StackType_t)&&priority==2&&stack==main_stack&&
        state==&main_state&&affinity==(1u<<2));return (TaskHandle_t)1;
}
int board_live(void *p){(void)p;return live;}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{if(offer_on_handle){offer_on_handle=0;assert(os_host_offer(&injected)==1);}return (TaskHandle_t)1;}
BaseType_t xTaskGenericNotify(TaskHandle_t task,UBaseType_t index,uint32_t value,eNotifyAction action,uint32_t *previous)
{(void)index;(void)value;(void)action;(void)previous;assert(task==(TaskHandle_t)1);++notified;return pdPASS;}
uint32_t ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t ticks)
{(void)index;(void)clear;(void)ticks;++waits;
 if(offer_on_wait){offer_on_wait=0;assert(os_host_offer(&injected)==1);return 1;}return 0;}
int main(void)
{
    struct ss_host_request q={.op=1,.count=7,.offset=3,.argument=9},got;
    struct ss_host_reply reply={.version=3,.state=1,.total=27,.bytes=7,.data="payload"},copied;
    uint8_t payload[SS_HOST_PAYLOAD_BYTES];
    assert(ss_host_recv(&got,0)==0);
    assert(os_host_offer(&q)==1&&notified==1);
    assert(os_host_offer(&q)==0);
    assert(ss_host_recv(&got,1000)==1&&!memcmp(&q,&got,sizeof(q))&&!waits);
    assert(ss_host_recv(&got,0)==0);
    assert(ss_host_send(&reply)==1);
    assert(ss_host_send(&reply)==-1);
    assert(os_host_reply(&copied,payload)==1&&copied.bytes==7&&!memcmp(payload,"payload",7));
    assert(os_host_offer(&q)==0);
    os_host_delivered();
    q.op=5;assert(os_host_offer(&q)==0);
    q.op=1;assert(os_host_offer(&q)==1);
    assert(ss_host_recv(&got,0)==1);
    reply.bytes=SS_HOST_PAYLOAD_BYTES+1;assert(ss_host_send(&reply)==-1);
    reply.bytes=1;reply.data=NULL;assert(ss_host_send(&reply)==-1);
    reply.data="x";assert(ss_host_send(&reply)==1);os_host_delivered();
    /* Arrival before waiter publication must be observed without sleeping. */
    offer_on_handle=1;unsigned before=waits;
    assert(ss_host_recv(&got,1000)==1&&waits==before);
    assert(ss_host_send(&reply)==1);os_host_delivered();
    /* Arrival after the empty check and at the wait boundary wakes the task. */
    offer_on_wait=1;before=notified;
    assert(ss_host_recv(&got,1000)==1&&notified==before+1&&waits==1);
    assert(ss_host_send(&reply)==1);os_host_delivered();
    live=0;assert(ss_host_recv(&got,1)==-1);
    assert(os_session_admitted()->board.deadline_us==0&&!ss_grant());
    board.session.deadline_us=manifest.return_reserve_us;assert(!os_application_prepare());
    board.session.deadline_us=123456789;assert(os_application_prepare());
    assert(ss_grant()->finish_by_us==123456789-manifest.return_reserve_us);
    assert(os_session_admitted()->board.deadline_us==0);
    puts("mailbox serialization, delivery acknowledgment, bounds and closed session passed");return 0;
}
