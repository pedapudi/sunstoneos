#include "npu_service.h"

static struct npu_service *instance;
static unsigned creating_worker;
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
static enum npu_service_result execute(struct npu_service *s,
                                        const struct npu_service_message *m,
                                        unsigned *canceled)
{
    const struct npu_service_request *r = &m->request;
    struct a733_npu_controller *c = &s->controller;
    if ((unsigned)r->operation > NPU_SERVICE_CLOSE ||
        (r->operation != NPU_SERVICE_SUBMIT && r->command) ||
        ((r->operation == NPU_SERVICE_OPEN || r->operation == NPU_SERVICE_SUBMIT ||
          r->operation == NPU_SERVICE_CANCEL) ? !duration(r->timeout_us) : r->timeout_us != 0) ||
        (r->operation == NPU_SERVICE_CANCEL && r->timeout_us <= 100)) return NPU_SERVICE_INVALID;
    if (r->operation == NPU_SERVICE_STATUS)
        return c->state == A733_NPU_FAULT ? NPU_SERVICE_FAULT : NPU_SERVICE_OK;
    if (r->operation == NPU_SERVICE_OPEN) {
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
    case NPU_SERVICE_SUBMIT:
        if (!r->command || r->command >= s->command_count) return NPU_SERVICE_INVALID;
        if (!(s->clients[m->client].commands & (UINT32_C(1) << r->command))) return NPU_SERVICE_DENIED;
        if (c->state != A733_NPU_READY) return NPU_SERVICE_BUSY;
        if (a733_npu_submit(c, &s->commands[r->command], r->timeout_us)) {
            s->job_ticket = m->ticket; return NPU_SERVICE_OK;
        }
        if (c->state == A733_NPU_FAULT) { s->job_ticket = m->ticket; return NPU_SERVICE_FAULT; }
        return NPU_SERVICE_INVALID;
    case NPU_SERVICE_CANCEL:
        if (c->state != A733_NPU_BOOTING && c->state != A733_NPU_RUNNING) return NPU_SERVICE_BUSY;
        if (a733_npu_cancel(c, r->timeout_us)) return NPU_SERVICE_OK;
        break;
    case NPU_SERVICE_REAP: {
        void *token; int stopped;
        if (!a733_npu_reap(c, &token, &stopped)) return NPU_SERVICE_BUSY;
        (void)token; *canceled = (unsigned)stopped;
        return NPU_SERVICE_OK;
    }
    case NPU_SERVICE_CLOSE:
        if (c->state != A733_NPU_READY && c->state != A733_NPU_DRAINED) return NPU_SERVICE_BUSY;
        if (a733_npu_close(c)) {
            s->owner = NPU_SERVICE_CLIENTS; s->job_ticket = 0; return NPU_SERVICE_OK;
        }
        break;
    default: return NPU_SERVICE_INVALID;
    }
    return c->state == A733_NPU_FAULT ? NPU_SERVICE_FAULT : NPU_SERVICE_INVALID;
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
        /* Keep the native counter extension current even during long idle
         * sessions. A faulted backend must not receive any further callbacks. */
        if (!active(s->controller.state) && s->controller.state != A733_NPU_FAULT)
            (void)s->ops.microseconds(s->cookie);
        /* At most one request between polls: queued STATUS traffic cannot
         * starve device completion or controller deadlines. */
        a733_npu_poll(&s->controller);
        struct npu_service_message message;
        TickType_t wait = active(s->controller.state) ? 1 :
            (s->controller.state == A733_NPU_FAULT ? portMAX_DELAY : pdMS_TO_TICKS(1000));
        if (xQueueReceive(s->requests, &message, wait) != pdTRUE) continue;
        configASSERT(message.client < NPU_SERVICE_CLIENTS && message.ticket);
        struct npu_service_reply reply = {.ticket = message.ticket};
        reply.result = execute(s, &message, &reply.canceled);
        reply.state = s->controller.state; reply.fault = s->controller.fault;
        reply.owns_session = s->owner == message.client;
        reply.job_ticket = s->job_ticket;
        if (message.request.operation == NPU_SERVICE_REAP && reply.result == NPU_SERVICE_OK)
            s->job_ticket = 0;
        configASSERT(xQueueSend(s->clients[message.client].replies, &reply, 0) == pdTRUE);
    }
}

int npu_service_init(struct npu_service *s, const struct a733_npu_ops *ops, void *cookie,
                      const struct a733_npu_mmu_image *image, const struct a733_npu_command *commands,
                      unsigned count, UBaseType_t priority, UBaseType_t affinity)
{
    if (!s || instance || s->initialized || !ops || !image || !commands ||
        !count || count > NPU_SERVICE_COMMANDS || !priority || priority >= configMAX_PRIORITIES ||
        !affinity || (affinity >> configNUMBER_OF_CORES) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED ||
        !ops->read || !ops->write || !ops->microseconds || !ops->delay ||
        !ops->barrier || !ops->publish || !ops->finish) return 0;
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
    s->initialized = 1; instance = s; creating_worker = 1;
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
    if (!c || c->pending || c->next_ticket == UINT64_MAX) return 0;
    struct npu_service_message m = {*request, c->next_ticket + 1, slot};
    c->pending = m.ticket;
    if (xQueueSend(s->requests, &m, wait) != pdTRUE) { c->pending = 0; return 0; }
    c->next_ticket = m.ticket; *ticket = m.ticket;
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
