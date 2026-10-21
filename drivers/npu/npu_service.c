#include "npu_service.h"
#include <string.h>

static struct npu_service *instance;
static unsigned creating_worker;
enum { SERVICE_WAITING, SERVICE_CHECKING, SERVICE_ACTIVE, SERVICE_RETAINED };
static void worker(void *);

BaseType_t npu_service_task_privileged(TaskFunction_t entry, void *argument)
{
    return creating_worker && entry == worker && argument == instance ? pdTRUE : pdFALSE;
}
static int duration(uint64_t us) { return us && us <= INT64_MAX; }
static int active(enum a733_npu_state state)
{
    return state == A733_NPU_BOOTING || state == A733_NPU_RUNNING ||
           state == A733_NPU_CANCEL_ASSERTED || state == A733_NPU_CANCEL_WAIT;
}
enum { IRQ_EVENT=1u, IRQ_FAILED=2u, SERVICE_DEFERRED=5, SERVICE_HELD=6 };
static void irq_fault(struct npu_service *s)
{
    s->controller.state=A733_NPU_FAULT;s->controller.fault=A733_NPU_IRQ;
    atomic_fetch_or_explicit(&s->irq_pending,IRQ_FAILED,memory_order_release);
}
static int irq_mask(struct npu_service *s)
{
    if(!s->irq_bound || s->irq_masked)return 1;
    if(s->irq_retained)return -1;
    int r=s->irq_ops.mask(s->irq_cookie);
    if(r==1){s->irq_masked=1;s->irq_rearming=0;}
    else if(r<0){s->irq_retained=1;irq_fault(s);}
    return r;
}
static int irq_progress(struct npu_service *s)
{
    a733_npu_check_timeout(&s->controller);
    unsigned pending=atomic_load_explicit(&s->irq_pending,memory_order_acquire);
    if(pending&IRQ_FAILED)irq_fault(s);
    if(pending || s->controller.state==A733_NPU_FAULT) {
        int masked=irq_mask(s);
        if(masked!=1)return masked<0 || s->controller.state==A733_NPU_FAULT ? 1 : 0;
        if(s->controller.state==A733_NPU_FAULT)return 1;
        pending=atomic_exchange_explicit(&s->irq_pending,0,memory_order_acq_rel);
        if(pending&IRQ_FAILED){irq_fault(s);return 1;}
    }
    enum a733_npu_state before=s->controller.state;
    a733_npu_advance(&s->controller,!!(pending&IRQ_EVENT));
    if((before==A733_NPU_BOOTING && s->controller.state==A733_NPU_READY) ||
       (before==A733_NPU_RUNNING && s->controller.state==A733_NPU_DONE)) {
        unsigned count=atomic_load_explicit(&s->irq_completions,memory_order_relaxed);
        if(count==UINT32_MAX)irq_fault(s);
        else atomic_store_explicit(&s->irq_completions,count+1,memory_order_release);
    }
    if(s->controller.state==A733_NPU_FAULT) {
        (void)irq_mask(s);return 1;
    }
    /* A latched completion keeps the source masked through full-idle/finish.
     * Cancellation also stays masked; it has its own bounded progress path. */
    if((s->controller.state==A733_NPU_BOOTING || s->controller.state==A733_NPU_RUNNING) &&
       !s->controller.irq_seen && (s->irq_masked || s->irq_rearming)) {
        taskENTER_CRITICAL();
        if(!atomic_load_explicit(&s->irq_pending,memory_order_acquire)) {
            int r=s->irq_ops.rearm(s->irq_cookie);
            if(r>=0){s->irq_masked=0;s->irq_rearming=r==0;}
            else {s->irq_retained=1;irq_fault(s);}
        }
        taskEXIT_CRITICAL();
    }
    return 1;
}
static enum npu_service_result execute(struct npu_service *s,
                                        const struct npu_service_message *m,
                                        unsigned *canceled)
{
    const struct npu_service_request *r = &m->request;
    struct a733_npu_controller *c = &s->controller;
    if ((unsigned)r->operation > NPU_SERVICE_WAIT ||
        (r->operation != NPU_SERVICE_SUBMIT && r->command) ||
        ((r->operation == NPU_SERVICE_OPEN || r->operation == NPU_SERVICE_SUBMIT ||
          r->operation == NPU_SERVICE_CANCEL) ? !duration(r->timeout_us) : r->timeout_us != 0) ||
        (r->operation == NPU_SERVICE_CANCEL && r->timeout_us <= 100)) return NPU_SERVICE_INVALID;
    if (r->operation == NPU_SERVICE_STATUS)
        return c->state == A733_NPU_FAULT ? NPU_SERVICE_FAULT : NPU_SERVICE_OK;
    if (r->operation == NPU_SERVICE_OPEN) {
        if(s->irq_bound && (c->state==A733_NPU_CLOSED || c->state==A733_NPU_FAULT))return NPU_SERVICE_FAULT;
        if(s->irq_bound && irq_mask(s)!=1)
            return c->state==A733_NPU_FAULT ? NPU_SERVICE_FAULT : SERVICE_DEFERRED;
        if (s->owner != NPU_SERVICE_CLIENTS) return NPU_SERVICE_BUSY;
        if (c->state != A733_NPU_COLD && c->state != A733_NPU_CLOSED) return NPU_SERVICE_BUSY;
        s->owner = m->client; s->job_ticket = m->ticket;
        if (a733_npu_open(c, &s->ops, s->cookie, &s->image, &s->commands[0], r->timeout_us))
            return NPU_SERVICE_OK;
        if (c->state == A733_NPU_FAULT) return NPU_SERVICE_FAULT;
        /* Controller validation refused before borrowing or hardware access. */
        s->owner = NPU_SERVICE_CLIENTS; s->job_ticket = 0;
        return NPU_SERVICE_INVALID;
    }
    if (s->owner != m->client) return NPU_SERVICE_DENIED;
    if (c->state == A733_NPU_FAULT) return NPU_SERVICE_FAULT;
    switch (r->operation) {
    case NPU_SERVICE_WAIT:
        return active(c->state) ? SERVICE_HELD : NPU_SERVICE_OK;
    case NPU_SERVICE_SUBMIT:
        if (!r->command || r->command >= s->command_count) return NPU_SERVICE_INVALID;
        if (!(s->clients[m->client].commands & (UINT32_C(1) << r->command))) return NPU_SERVICE_DENIED;
        if (c->state != A733_NPU_READY) return NPU_SERVICE_BUSY;
        if(s->irq_bound && irq_mask(s)!=1)
            return c->state==A733_NPU_FAULT ? NPU_SERVICE_FAULT : SERVICE_DEFERRED;
        if (a733_npu_submit(c, &s->commands[r->command], r->timeout_us)) {
            s->job_ticket = m->ticket; return NPU_SERVICE_OK;
        }
        if (c->state == A733_NPU_FAULT) { s->job_ticket = m->ticket; return NPU_SERVICE_FAULT; }
        return NPU_SERVICE_INVALID;
    case NPU_SERVICE_CANCEL:
        if (c->state != A733_NPU_BOOTING && c->state != A733_NPU_RUNNING) return NPU_SERVICE_BUSY;
        if(s->irq_bound && irq_mask(s)!=1)
            return c->state==A733_NPU_FAULT ? NPU_SERVICE_FAULT : SERVICE_DEFERRED;
        if (a733_npu_cancel(c, r->timeout_us)) return NPU_SERVICE_OK;
        break;
    case NPU_SERVICE_REAP: {
        void *token; int stopped;
        if (!a733_npu_reap(c, &token, &stopped)) return NPU_SERVICE_BUSY;
        (void)token; *canceled = (unsigned)stopped;
        return NPU_SERVICE_OK;
    }
    case NPU_SERVICE_CLOSE:
        if (c->state != A733_NPU_READY && c->state != A733_NPU_DRAINED &&
            !(s->irq_bound && c->state==A733_NPU_CLOSED)) return NPU_SERVICE_BUSY;
        if(s->irq_bound && irq_mask(s)!=1)
            return c->state==A733_NPU_FAULT ? NPU_SERVICE_FAULT : SERVICE_DEFERRED;
        if (c->state==A733_NPU_CLOSED || a733_npu_close(c)) {
            if(s->irq_bound) {
                int r=s->irq_ops.stop(s->irq_cookie);
                if(r<0){s->irq_retained=1;irq_fault(s);return NPU_SERVICE_FAULT;}
                if(!r)return SERVICE_DEFERRED;
                atomic_store_explicit(&s->irq_stopped,1,memory_order_release);
            }
            s->owner = NPU_SERVICE_CLIENTS; s->job_ticket = 0; return NPU_SERVICE_OK;
        }
        break;
    default: return NPU_SERVICE_INVALID;
    }
    return c->state == A733_NPU_FAULT ? NPU_SERVICE_FAULT : NPU_SERVICE_INVALID;
}
static void respond(struct npu_service *s, const struct npu_service_message *m,
                    enum npu_service_result result, unsigned canceled)
{
    struct npu_service_reply reply = {.ticket = m->ticket, .result = result, .canceled = canceled};
    reply.state = s->controller.state; reply.fault = s->controller.fault;
    reply.owns_session = s->owner == m->client;
    reply.job_ticket = s->job_ticket;
    if (m->request.operation == NPU_SERVICE_REAP && result == NPU_SERVICE_OK)
        s->job_ticket = 0;
    configASSERT(xQueueSend(s->clients[m->client].replies, &reply, 0) == pdTRUE);
}
/* The owner's held WAIT is answered once the controller leaves an active state.
 * Every active state has a controller deadline, so the hold is bounded. */
static void release_held(struct npu_service *s)
{
    if (!s->has_held || active(s->controller.state)) return;
    s->has_held = 0;
    respond(s, &s->held, s->controller.state == A733_NPU_FAULT ? NPU_SERVICE_FAULT : NPU_SERVICE_OK, 0);
}
static void worker(void *argument)
{
    struct npu_service *s = argument;
    uint64_t level, stack;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, spsel" : "=r"(level), "=r"(stack));
    configASSERT(level == 4);
#if configNUMBER_OF_CORES > 1
    configASSERT(stack == 0); /* Preemptible EL1 task uses SP_EL0, not an exception stack. */
#else
    (void)stack;
#endif
    for (;;) {
        if (atomic_load_explicit(&s->activation_state,memory_order_acquire)!=SERVICE_ACTIVE) {
            (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
            continue;
        }
        /* Keep the native counter extension current even during long idle
         * sessions. A faulted backend must not receive any further callbacks. */
        if (!active(s->controller.state) && s->controller.state != A733_NPU_FAULT)
            (void)s->ops.microseconds(s->cookie);
        /* At most one request between polls: queued STATUS traffic cannot
         * starve device completion or controller deadlines. */
        int progressed=1;
        if(s->irq_bound)progressed=irq_progress(s);
        else a733_npu_poll(&s->controller);
        release_held(s);
        struct npu_service_message message;
        TickType_t wait = active(s->controller.state) ? 1 :
            (s->controller.state == A733_NPU_FAULT ? portMAX_DELAY : pdMS_TO_TICKS(1000));
        if(s->irq_bound) {
            if(!s->irq_masked && !s->irq_retained &&
               (s->controller.state==A733_NPU_FAULT || atomic_load_explicit(&s->irq_pending,memory_order_acquire)))wait=1;
            if(!progressed){(void)ulTaskNotifyTake(pdTRUE,1);continue;}
            if(s->has_deferred)message=s->deferred;
            else if(xQueueReceive(s->requests,&message,0)!=pdTRUE) {
                (void)ulTaskNotifyTake(pdTRUE,wait);continue;
            }
        }else if (xQueueReceive(s->requests, &message, wait) != pdTRUE) continue;
        configASSERT(message.client < NPU_SERVICE_CLIENTS && message.ticket);
        unsigned canceled = 0;
        enum npu_service_result result = execute(s, &message, &canceled);
        if((unsigned)result==SERVICE_DEFERRED) {
            s->deferred=message;s->has_deferred=1;
            (void)ulTaskNotifyTake(pdTRUE,1);continue;
        }
        s->has_deferred=0;
        if((unsigned)result==SERVICE_HELD) {
            /* One pending request per client: the owner has no other request. */
            configASSERT(!s->has_held);
            s->held=message;s->has_held=1;continue;
        }
        respond(s, &message, result, canceled);
        release_held(s);
    }
}

static int initialize(struct npu_service *s, const struct a733_npu_ops *ops, void *cookie,
                      const struct a733_npu_mmu_image *image, const struct a733_npu_command *commands,
                      unsigned count, UBaseType_t priority, UBaseType_t affinity,
                      const struct npu_service_activation *activation)
{
    struct npu_service_activation saved_activation={0};
    if(activation) {
        memcpy(&saved_activation,activation,sizeof(saved_activation));
        if(!saved_activation.bootstrap || !saved_activation.prepared)return 0;
        activation=&saved_activation;
    }
    if (!s || instance || s->initialized || !ops || !image || !commands ||
        !count || count > NPU_SERVICE_COMMANDS || !priority || priority >= configMAX_PRIORITIES ||
        !affinity || (affinity >> configNUMBER_OF_CORES) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED ||
        !ops->read || !ops->write || !ops->microseconds || !ops->delay ||
        !ops->barrier || !ops->publish || !ops->finish) return 0;
    const struct a733_npu_ops saved_ops=*ops;
    const struct a733_npu_mmu_image saved_image=*image;
    struct a733_npu_command saved_commands[NPU_SERVICE_COMMANDS];
    memcpy(saved_commands,commands,count*sizeof(*commands));
    ops=&saved_ops;image=&saved_image;commands=saved_commands;
    for (unsigned i = 0; i < count; ++i) {
        const struct a733_npu_command *c = &commands[i];
        if (!c->device || (c->device & 63) || !c->token || c->readable_bytes < A733_NPU_FETCH_BYTES ||
            c->readable_bytes > (UINT64_C(1) << 32) - c->device) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (commands[j].token == c->token || commands[j].device == c->device) return 0;
    }
    s->ops = *ops; s->cookie = cookie; s->image = *image; s->command_count = count;
    for (unsigned i = 0; i < count; ++i) s->commands[i] = commands[i];
    s->owner = NPU_SERVICE_CLIENTS;
    s->requests = xQueueCreateStatic(NPU_SERVICE_CLIENTS, sizeof(struct npu_service_message),
                                     s->request_storage, &s->request_state);
    configASSERT(s->requests);
    for (unsigned i = 0; i < NPU_SERVICE_CLIENTS; ++i) {
        struct npu_service_client *c = &s->clients[i];
        c->replies = xQueueCreateStatic(1, sizeof(struct npu_service_reply),
                                        c->reply_storage, &c->reply_state);
        configASSERT(c->replies);
    }
    s->activation=activation ? *activation : (struct npu_service_activation){0};
    atomic_init(&s->activation_state,activation ? SERVICE_WAITING : SERVICE_ACTIVE);
    atomic_init(&s->irq_pending,0);atomic_init(&s->irq_stopped,0);atomic_init(&s->irq_completions,0);
    s->initialized = 1; s->affinity = affinity; instance = s; creating_worker = 1;
#if configNUMBER_OF_CORES > 1
    s->worker = xTaskCreateStaticAffinitySet(worker, "npu-service", NPU_SERVICE_STACK_WORDS,
        s, priority, s->worker_stack, &s->worker_state, affinity);
#else
    s->worker = xTaskCreateStatic(worker, "npu-service", NPU_SERVICE_STACK_WORDS,
        s, priority, s->worker_stack, &s->worker_state);
#endif
    creating_worker = 0;
    configASSERT(s->worker);
    return 1;
}
int npu_service_init(struct npu_service *s,const struct a733_npu_ops *ops,void *cookie,
    const struct a733_npu_mmu_image *image,const struct a733_npu_command *commands,
    unsigned count,UBaseType_t priority,UBaseType_t affinity)
{return initialize(s,ops,cookie,image,commands,count,priority,affinity,NULL);}
int npu_service_init_deferred(struct npu_service *s,const struct a733_npu_ops *ops,void *cookie,
    const struct a733_npu_mmu_image *image,const struct a733_npu_command *commands,
    unsigned count,UBaseType_t priority,UBaseType_t affinity,const struct npu_service_activation *activation)
{
    if(!activation)return 0;
    return initialize(s,ops,cookie,image,commands,count,priority,affinity,activation);
}
enum npu_service_result npu_service_activate(struct npu_service *s)
{
    if(!s || s!=instance || !s->initialized || !s->activation.bootstrap ||
        xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING ||
        xTaskGetCurrentTaskHandle()!=s->activation.bootstrap)return NPU_SERVICE_DENIED;
    uint64_t level;__asm__ volatile("mrs %0,CurrentEL" : "=r"(level));
    if(level!=4)return NPU_SERVICE_DENIED;
    unsigned expected=SERVICE_WAITING;
    if(!atomic_compare_exchange_strong_explicit(&s->activation_state,&expected,SERVICE_CHECKING,
        memory_order_acq_rel,memory_order_acquire))
        return expected==SERVICE_RETAINED ? NPU_SERVICE_FAULT : NPU_SERVICE_BUSY;
    int ready=s->activation.prepared(s->activation.context,&s->ops,s->cookie,&s->image,s->commands,s->command_count);
    unsigned state=ready==1 ? SERVICE_ACTIVE : ready==0 ? SERVICE_WAITING : SERVICE_RETAINED;
    atomic_store_explicit(&s->activation_state,state,memory_order_release);
    if(state==SERVICE_ACTIVE){xTaskNotifyGive(s->worker);return NPU_SERVICE_OK;}
    return state==SERVICE_WAITING ? NPU_SERVICE_BUSY : NPU_SERVICE_FAULT;
}
int npu_service_bind(struct npu_service *s, unsigned slot, TaskHandle_t task, uint32_t commands)
{
    if (!s || s != instance || !s->initialized || slot >= NPU_SERVICE_CLIENTS || !task ||
        task == s->worker || s->clients[slot].task || (commands & 1u) ||
        (s->command_count < 32 && (commands >> s->command_count)) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    for (unsigned i = 0; i < NPU_SERVICE_CLIENTS; ++i)
        if (s->clients[i].task == task) return 0;
    s->clients[slot].task = task; s->clients[slot].commands = commands;
    return 1;
}
int npu_service_bind_irq(struct npu_service *s,const struct npu_service_irq_ops *ops,void *cookie,
    UBaseType_t route)
{
    if(!s || s!=instance || !s->initialized || s->irq_bound || !ops ||
       !ops->mask || !ops->rearm || !ops->stop || !cookie ||
       !route || (route&(route-1)) || route!=s->affinity ||
       xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    s->irq_ops=*ops;s->irq_cookie=cookie;s->irq_bound=1;return 1;
}
void npu_service_irq_notify_from_isr(struct npu_service *s,int mask_ok,BaseType_t *woken)
{
    configASSERT(s && s==instance && s->initialized && s->irq_bound && woken);
    unsigned stopped=atomic_exchange_explicit(&s->irq_stopped,0,memory_order_acq_rel);
    atomic_fetch_or_explicit(&s->irq_pending,IRQ_EVENT|(mask_ok==1&&!stopped?0:IRQ_FAILED),memory_order_release);
    vTaskNotifyGiveFromISR(s->worker,woken);
}
int npu_service_irq_stopped(const struct npu_service *s)
{return s && s==instance && s->irq_bound && atomic_load_explicit(&s->irq_stopped,memory_order_acquire) &&
    !atomic_load_explicit(&s->irq_pending,memory_order_acquire);}
unsigned npu_service_irq_completions(const struct npu_service *s)
{return s && s==instance && s->irq_bound ? atomic_load_explicit(&s->irq_completions,memory_order_acquire) : 0;}
static struct npu_service_client *caller(struct npu_service *s, unsigned *slot)
{
    if (!s || s != instance || !s->initialized ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return NULL;
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    for (unsigned i = 0; i < NPU_SERVICE_CLIENTS; ++i)
        if (s->clients[i].task == task) { *slot = i; return &s->clients[i]; }
    return NULL;
}
int npu_service_send(struct npu_service *s, const struct npu_service_request *request,
                      TickType_t wait, uint64_t *ticket)
{
    if (!request || !ticket || wait == portMAX_DELAY) return 0;
    unsigned slot;
    struct npu_service_client *c = caller(s, &slot);
    if (!c || c->pending || c->next_ticket == UINT64_MAX ||
        atomic_load_explicit(&s->activation_state,memory_order_acquire)!=SERVICE_ACTIVE) return 0;
    struct npu_service_message m = {*request, c->next_ticket + 1, slot};
    c->pending = m.ticket;
    if (xQueueSend(s->requests, &m, wait) != pdTRUE) { c->pending = 0; return 0; }
    c->next_ticket = m.ticket; *ticket = m.ticket;
    if(s->irq_bound)xTaskNotifyGive(s->worker);
    return 1;
}
int npu_service_collect(struct npu_service *s, uint64_t ticket,
                         struct npu_service_reply *reply, TickType_t wait)
{
    if (!reply || !ticket || wait == portMAX_DELAY) return 0;
    unsigned slot;
    struct npu_service_client *c = caller(s, &slot);
    (void)slot;
    if (!c || c->pending != ticket) return 0;
    struct npu_service_reply value;
    if (xQueueReceive(c->replies, &value, wait) != pdTRUE) return 0;
    configASSERT(value.ticket == ticket);
    c->pending = 0; *reply = value;
    return 1;
}
