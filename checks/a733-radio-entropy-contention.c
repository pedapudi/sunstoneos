/* Copied-consumer contention against the actual adapter publish path. A
 * completed CE refill is represented by owner-private staging; this fixture
 * does not execute the CE request or the archived CE cases. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#define main prior_ce_cases_not_executed
#include "a733-ce-entropy.c"
#undef main
#pragma clang diagnostic pop
#include "a733_radio_entropy.h"
#include <pthread.h>
#include <sched.h>

static int owner_id,ip_id,other_id;
#define OWNER ((TaskHandle_t)&owner_id)
#define IP ((TaskHandle_t)&ip_id)
#define OTHER ((TaskHandle_t)&other_id)
static _Thread_local TaskHandle_t current;
static TaskHandle_t mutex_owner;
static BaseType_t scheduler;
static struct model *live_model;
struct backing {struct model model;StaticSemaphore_t mutex;struct a733_counter_clock clock;};
struct qualification {struct a733_ce_session *engine;};
TaskHandle_t xTaskGetCurrentTaskHandle(void) {return current;}
TaskHandle_t FreeRTOS_GetIPTaskHandle(void) {return IP;}
BaseType_t xTaskGetSchedulerState(void) {return scheduler;}
int a733_counter_clock_now(void *cookie,uint64_t *value)
{assert(((struct a733_counter_clock *)cookie)->initialized);*value=live_model->now;return 1;}
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t queue)
{assert(queue);return mutex_owner;}
BaseType_t xQueueSemaphoreTake(QueueHandle_t queue,TickType_t wait)
{assert(queue && wait==0 && current==OWNER);if(mutex_owner)return pdFALSE;mutex_owner=current;return pdTRUE;}
BaseType_t xQueueGenericSend(QueueHandle_t queue,const void *item,TickType_t wait,BaseType_t position)
{(void)position;assert(queue && !item && !wait && mutex_owner==current);mutex_owner=NULL;return pdTRUE;}
static int qualified(void *cookie,const struct a733_ce_session *e,uint64_t record)
{struct qualification *q=cookie;return e==q->engine && record==19;}
static struct backing b;
static struct qualification q;
static struct a733_radio_entropy a;
static void init(void)
{
    memset(&b,0,sizeof(b));memset(&a,0,sizeof(a));setup(&b.model,0);
    q.engine=b.model.service.engine;scheduler=taskSCHEDULER_NOT_STARTED;current=OWNER;mutex_owner=NULL;
    live_model=&b.model;b.clock.initialized=1;atomic_init(&b.clock.failed,0);
    b.model.registry.now=a733_counter_clock_now;b.model.registry.context=&b.clock;
    struct a733_radio_entropy_config c={.source=&b.model.service,.owner=OWNER,
        .pool_mutex=(SemaphoreHandle_t)&b.mutex,.qualification_record=19,.request_timeout_us=1000,
        .clock=&b.clock,.provider_state=&b,.provider_bytes=sizeof(b),.qualified=qualified,
        .qualification_context=&q,.qualification_bytes=sizeof(q)};
    assert(a733_radio_entropy_init(&a,&c));scheduler=taskSCHEDULER_RUNNING;
}
/* Owner thread only: the reservoir has room, so publish one completed refill. */
static int publish(uint8_t seed)
{
    for(size_t i=0;i<sizeof(a.staging);++i)a.staging[i]=(uint8_t)(seed+i);
    a.staged=1;a.deadline=live_model->now+1000;
    enum a733_radio_entropy_result r=a733_radio_entropy_poll(&a);
    assert(r==A733_RADIO_ENTROPY_READY || r==A733_RADIO_ENTROPY_BUSY);
    return r==A733_RADIO_ENTROPY_READY && !a.staged;
}
static size_t locked_count(void)
{
    unsigned expected=0;
    while(!atomic_compare_exchange_strong(&a.reservoir_busy,&expected,1))expected=0;
    size_t count=a.count;atomic_store(&a.reservoir_busy,0);return count;
}
enum { REFILLS=4000 };
static atomic_uint owner_done;
static unsigned published;
static void *owner_thread(void *unused)
{
    (void)unused;current=OWNER;
    while(published<REFILLS){
        if(locked_count()>A733_RADIO_ENTROPY_CAPACITY-sizeof(a.staging)){sched_yield();continue;}
        if(publish((uint8_t)published))++published;
    }
    atomic_store(&owner_done,1);return NULL;
}
static unsigned long copied,refused;
static void *ip_thread(void *unused)
{
    (void)unused;current=IP;
    for(;;){
        unsigned done=atomic_load(&owner_done);uint32_t word;
        int r=a733_radio_entropy_random32(&a,&word);
        assert(r==1 || r==0);assert(!atomic_load(&a.fault));
        if(r==1)++copied;else ++refused;
        if(done && !r)break;
    }
    return NULL;
}
int main(void)
{
    uint32_t word=0xa5a5a5a5u;uint8_t bytes[32];
    init();assert(publish(0) && a.count==256);
    current=IP;assert(a733_radio_entropy_random32(&a,&word)==1 && word==0x03020100u);

    /* Contention past the bounded retry is a temporary refusal, not a fault. */
    atomic_store(&a.reservoir_busy,1);word=0xa5a5a5a5u;
    assert(a733_radio_entropy_random32(&a,&word)==0 && word==0xa5a5a5a5u);
    assert(a733_radio_entropy_sequence(&a,1,2,3,4,&word)==0 && !a733_radio_entropy_bytes(&a,bytes,4));
    assert(!atomic_load(&a.fault) && a.count==252);atomic_store(&a.reservoir_busy,0);

    /* Ordinary words leave the reserve for the sequence hook, which cannot fail. */
    while(a.count>A733_RADIO_ENTROPY_SEQUENCE_RESERVE+4)assert(a733_radio_entropy_bytes(&a,bytes,4));
    assert(a733_radio_entropy_random32(&a,&word)==1 && a.count==A733_RADIO_ENTROPY_SEQUENCE_RESERVE);
    assert(a733_radio_entropy_random32(&a,&word)==0 && a.count==A733_RADIO_ENTROPY_SEQUENCE_RESERVE);
    for(unsigned i=0;i<A733_RADIO_ENTROPY_SEQUENCE_RESERVE/4;++i)assert(a733_radio_entropy_sequence(&a,1,2,3,4,&word)==1);
    assert(!a.count && a733_radio_entropy_sequence(&a,1,2,3,4,&word)==0 && !atomic_load(&a.fault));

    /* Invalid callers and actual session faults are permanent refusals. */
    current=OWNER;assert(publish(1));
    current=OTHER;assert(a733_radio_entropy_random32(&a,&word)==-1 && !atomic_load(&a.fault));
    current=IP;atomic_store(&b.model.service.engine->fault,1);
    assert(a733_radio_entropy_random32(&a,&word)==-1 && atomic_load(&a.fault) && !a.count);
    for(size_t i=0;i<sizeof(a.reservoir);++i)assert(!a.reservoir[i]);

    /* Interleave the real owner publish loop with IP-task consumption. */
    init();pthread_t owner,ip;
    assert(!pthread_create(&owner,NULL,owner_thread,NULL) && !pthread_create(&ip,NULL,ip_thread,NULL));
    assert(!pthread_join(owner,NULL) && !pthread_join(ip,NULL));
    assert((unsigned long)published*256==copied*4+a.count && !atomic_load(&a.fault));
    printf("PASS radio entropy contention: %u refills, %lu words copied, %lu temporary refusals, no fault\n",
        published,copied,refused);
    return 0;
}
