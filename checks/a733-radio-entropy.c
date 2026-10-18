/* Reuse the actual CE service model helpers, without executing its old cases. */
/* Renaming C main removes its implicit return-zero rule. The archived cases
 * are never called; keep their original source intact for evidence reuse. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#define main prior_ce_cases_not_executed
#include "a733-ce-entropy.c"
#undef main
#pragma clang diagnostic pop
#include "a733_radio_entropy.h"

static int owner_id,ip_id,other_id;
#define OWNER ((TaskHandle_t)&owner_id)
#define IP ((TaskHandle_t)&ip_id)
#define OTHER ((TaskHandle_t)&other_id)
static TaskHandle_t current,mutex_owner;
static BaseType_t scheduler;
static struct a733_radio_entropy *live_adapter;
static struct model *live_model;
static unsigned give_fault,leave_busy,pool_calls;
struct backing {struct model model;StaticSemaphore_t mutex;struct a733_counter_clock clock;};
struct qualification {struct a733_ce_session *engine;unsigned calls,accepted;};
TaskHandle_t xTaskGetCurrentTaskHandle(void) {return current;}
TaskHandle_t FreeRTOS_GetIPTaskHandle(void) {return IP;}
BaseType_t xTaskGetSchedulerState(void) {return scheduler;}
int a733_counter_clock_now(void *cookie,uint64_t *value)
{assert(((struct a733_counter_clock *)cookie)->initialized);*value=live_model->now;return 1;}
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t queue)
{assert(queue);return mutex_owner;}
BaseType_t xQueueSemaphoreTake(QueueHandle_t queue,TickType_t wait)
{assert(queue && wait==0 && current==OWNER);++pool_calls;if(mutex_owner)return pdFALSE;mutex_owner=current;return pdTRUE;}
BaseType_t xQueueGenericSend(QueueHandle_t queue,const void *item,TickType_t wait,BaseType_t position)
{
    (void)position;assert(queue && !item && !wait && mutex_owner==current);++pool_calls;
    mutex_owner=NULL;
    if(give_fault && live_adapter->staged) {
        give_fault=0;assert(a733_clock_retain(&live_model->registry,&live_model->token)==A733_CLOCK_FAULT);
    }
    return pdTRUE;
}
static int qualified(void *cookie,const struct a733_ce_session *e,uint64_t record)
{struct qualification *q=cookie;assert(e==q->engine && record==19);++q->calls;return q->accepted;}
static void fixture_leave(void *cookie)
{
    struct model *m=cookie;leave(cookie);
    if(leave_busy && m->acquired==8) {leave_busy=0;atomic_store(&m->registry.busy,1);}
}
static void init(struct backing *b,struct qualification *q,struct a733_radio_entropy *a)
{
    memset(b,0,sizeof(*b));memset(a,0,sizeof(*a));setup(&b->model,0);
    q->engine=b->model.service.engine;q->calls=0;q->accepted=0;
    scheduler=taskSCHEDULER_NOT_STARTED;current=OWNER;mutex_owner=NULL;pool_calls=0;
    live_adapter=a;live_model=&b->model;give_fault=leave_busy=0;
    b->clock.initialized=1;atomic_init(&b->clock.failed,0);
    b->model.registry.now=a733_counter_clock_now;b->model.registry.context=&b->clock;
    struct a733_radio_entropy_config c={.source=&b->model.service,.owner=OWNER,
        .pool_mutex=(SemaphoreHandle_t)&b->mutex,.qualification_record=19,.request_timeout_us=1000,
        .clock=&b->clock,
        .provider_state=b,.provider_bytes=sizeof(*b),.qualified=qualified,
        .qualification_context=q,.qualification_bytes=sizeof(*q)};
    assert(!a733_radio_entropy_init(a,&c) && !a->self && !b->model.reads && !b->model.writes);
    q->accepted=1;assert(a733_radio_entropy_init(a,&c) && q->calls==2);
    assert(!a733_radio_entropy_init(a,&c));scheduler=taskSCHEDULER_RUNNING;
}
static enum a733_radio_entropy_result fill(struct backing *b,struct a733_radio_entropy *a)
{
    enum a733_radio_entropy_result r=A733_RADIO_ENTROPY_PENDING;
    for(unsigned i=0;i<160 && r==A733_RADIO_ENTROPY_PENDING;++i) {
        if(b->model.pending)complete(&b->model);
        r=a733_radio_entropy_poll(a);
    }
    return r;
}
static void cleared(const uint8_t *p,size_t n)
{for(size_t i=0;i<n;++i)assert(!p[i]);}
int main(void)
{
    struct backing b;struct qualification q;struct a733_radio_entropy a,copy;
    uint8_t out[256];uint32_t word;memset(out,0xa5,sizeof(out));
    init(&b,&q,&a);
    assert(!a733_radio_entropy_bytes(&a,out,32));unchanged(out,sizeof(out));
    current=OTHER;assert(a733_radio_entropy_poll(&a)==A733_RADIO_ENTROPY_INVALID);
    current=IP;assert(a733_radio_entropy_poll(&a)==A733_RADIO_ENTROPY_INVALID);
    current=OWNER;mutex_owner=OTHER;
    assert(a733_radio_entropy_poll(&a)==A733_RADIO_ENTROPY_BUSY && !b.model.starts);mutex_owner=NULL;
    assert(fill(&b,&a)==A733_RADIO_ENTROPY_READY && a.count==256 && b.model.starts==8);
    assert(fill(&b,&a)==A733_RADIO_ENTROPY_READY && a.count==512 && b.model.starts==16);
    unsigned starts=b.model.starts,reads=b.model.reads,writes=b.model.writes,calls=pool_calls;
    assert(a733_radio_entropy_poll(&a)==A733_RADIO_ENTROPY_READY && pool_calls==calls);
    copy=a;assert(!a733_radio_entropy_bytes(&copy,out,1));
    current=OTHER;assert(!a733_radio_entropy_bytes(&a,out,1));current=IP;
    atomic_store(&a.reservoir_busy,1);assert(!a733_radio_entropy_bytes(&a,out,32));atomic_store(&a.reservoir_busy,0);
    assert(!a733_radio_entropy_bytes(&a,(uint8_t *)&b.mutex,4));
    assert(!a733_radio_entropy_bytes(&a,(uint8_t *)&a,4));
    assert(!a733_radio_entropy_bytes(&a,out,257));unchanged(out,sizeof(out));
    assert(a733_radio_entropy_random32(&a,&word));
    uint8_t bytes[4];memcpy(bytes,&word,4);for(unsigned i=0;i<4;++i)assert(bytes[i]==32+i);
    assert(a733_radio_entropy_sequence(&a,1,2,3,4,&word));
    memcpy(bytes,&word,4);for(unsigned i=0;i<4;++i)assert(bytes[i]==36+i);
    assert(a733_radio_entropy_bytes(&a,out,256));for(unsigned i=0;i<256;++i)assert(out[i]==(uint8_t)(40+i));
    assert(a733_radio_entropy_bytes(&a,out,248));assert(!a.count);cleared(a.reservoir,sizeof(a.reservoir));
    memset(out,0xa5,sizeof(out));assert(!a733_radio_entropy_bytes(&a,out,1));unchanged(out,sizeof(out));
    assert(b.model.starts==starts && b.model.reads==reads && b.model.writes==writes && pool_calls==calls);
    current=OWNER;assert(fill(&b,&a)==A733_RADIO_ENTROPY_READY && b.model.starts==24);
    assert(a733_radio_entropy_bytes(&a,out,256));for(unsigned i=0;i<256;++i)assert(out[i]==(uint8_t)(32+i));

    /* A completed refill survives final-token contention without another task. */
    init(&b,&q,&a);b.model.service.engine->binding.ops.leave=fixture_leave;
    b.model.service.engine->controller.ops.leave=fixture_leave;leave_busy=1;
    assert(fill(&b,&a)==A733_RADIO_ENTROPY_BUSY && a.staged && !a.count && b.model.starts==8);
    atomic_store(&b.model.registry.busy,0);
    assert(a733_radio_entropy_poll(&a)==A733_RADIO_ENTROPY_READY && a.count==256 && b.model.starts==8);

    /* Fault publication after the pool unlock cannot expose completed bytes. */
    init(&b,&q,&a);give_fault=1;
    assert(fill(&b,&a)==A733_RADIO_ENTROPY_RETAINED && !a.count);
    cleared(a.reservoir,sizeof(a.reservoir));cleared(a.staging,sizeof(a.staging));
    assert(b.model.pool.allocations[b.model.lease.slot].state==DEVICE_MEMORY_RETAINED);

    /* Active partial CE output and cached bytes are erased on late authority loss. */
    init(&b,&q,&a);assert(fill(&b,&a)==A733_RADIO_ENTROPY_READY);
    for(unsigned i=0;i<30 && b.model.service.filled!=32;++i) {
        if(b.model.pending)complete(&b.model);
        assert(a733_radio_entropy_poll(&a)==A733_RADIO_ENTROPY_PENDING);
    }
    assert(a.active && b.model.service.filled==32);
    assert(a733_clock_retain(&b.model.registry,&b.model.token)==A733_CLOCK_FAULT);
    reads=b.model.reads;writes=b.model.writes;current=IP;memset(out,0xa5,sizeof(out));
    assert(!a733_radio_entropy_bytes(&a,out,32));unchanged(out,sizeof(out));
    current=OWNER;assert(a733_radio_entropy_poll(&a)==A733_RADIO_ENTROPY_RETAINED);
    assert(b.model.reads==reads && b.model.writes==writes);
    cleared(b.model.service.staging,sizeof(b.model.service.staging));cleared(a.reservoir,sizeof(a.reservoir));

    /* Another typed client's fault is visible even before registry retention. */
    init(&b,&q,&a);assert(fill(&b,&a)==A733_RADIO_ENTROPY_READY);
    reads=b.model.reads;writes=b.model.writes;current=OTHER;mutex_owner=OTHER;
    atomic_store(&b.model.registry.busy,1);
    assert(a733_ce_session_retain(b.model.service.engine)==A733_CE_BUSY);
    assert(atomic_load(&b.model.service.engine->fault) && !atomic_load(&b.model.registry.failed));
    current=IP;memset(out,0xa5,sizeof(out));assert(!a733_radio_entropy_bytes(&a,out,32));unchanged(out,sizeof(out));
    current=OWNER;assert(a733_radio_entropy_poll(&a)==A733_RADIO_ENTROPY_BUSY && atomic_load(&a.fault));
    mutex_owner=NULL;atomic_store(&b.model.registry.busy,0);
    assert(a733_radio_entropy_poll(&a)==A733_RADIO_ENTROPY_RETAINED);
    assert(b.model.reads==reads && b.model.writes==writes);

    /* An admitted refill expires even while another pool user holds its mutex. */
    init(&b,&q,&a);assert(fill(&b,&a)==A733_RADIO_ENTROPY_READY);
    for(unsigned i=0;i<30 && b.model.service.filled!=32;++i) {
        if(b.model.pending)complete(&b.model);
        assert(a733_radio_entropy_poll(&a)==A733_RADIO_ENTROPY_PENDING);
    }
    assert(a.active && b.model.service.filled==32);mutex_owner=OTHER;b.model.now=a.deadline;
    reads=b.model.reads;writes=b.model.writes;
    assert(a733_radio_entropy_poll(&a)==A733_RADIO_ENTROPY_BUSY && atomic_load(&a.fault) && !a.count);
    current=IP;memset(out,0xa5,sizeof(out));assert(!a733_radio_entropy_bytes(&a,out,1));unchanged(out,sizeof(out));
    current=OWNER;mutex_owner=NULL;assert(a733_radio_entropy_poll(&a)==A733_RADIO_ENTROPY_RETAINED);
    assert(b.model.reads==reads && b.model.writes==writes);empty_staging(&b.model);

    /* Waiting for the same occupied CE client does not renew the refill bound. */
    init(&b,&q,&a);uint64_t old=0;struct a733_ce_entropy other={0};
    assert(a733_ce_entropy_attach(&other,b.model.service.engine));
    assert(a733_ce_entropy_request(&other,32,10000,&old)==A733_CE_ENTROPY_PENDING);
    assert(a733_radio_entropy_poll(&a)==A733_RADIO_ENTROPY_BUSY);uint64_t deadline=a.deadline;
    b.model.now=deadline;
    assert(a733_radio_entropy_poll(&a)==A733_RADIO_ENTROPY_RETAINED && !b.model.starts);
    puts("PASS radio entropy: actual CE refill, copied consumers, ownership, exhaustion, contention and retained faults; modeled task/mutex dispatch and DMA bytes, no physical entropy qualification");
    return 0;
}
