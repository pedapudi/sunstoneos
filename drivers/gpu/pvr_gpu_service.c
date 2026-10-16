#include "pvr_gpu_service.h"
#include <string.h>

static struct pvr_gpu_service *instance;
static unsigned creating_worker;
static void worker(void *);
BaseType_t pvr_gpu_service_task_privileged(TaskFunction_t entry, void *argument)
{ return creating_worker && entry == worker && argument == instance ? pdTRUE : pdFALSE; }

static int span(const void *p, size_t n)
{ return p && n && n <= UINTPTR_MAX - (uintptr_t)p; }
static int separate(const void *a, size_t n, const void *b, size_t m)
{ return (uintptr_t)a + n <= (uintptr_t)b || (uintptr_t)b + m <= (uintptr_t)a; }
static const struct pvr_mips_buffer *buffer(const struct pvr_gpu_service_config *c, unsigned i)
{ return i < 3 ? &c->firmware_buffers[i] : &c->resources[i - 3]; }
static int allocations(struct pvr_gpu_service *s, const struct pvr_gpu_service_config *c)
{
    struct pvr_fw_plan plan;
    if (!span(s, sizeof(*s)) || !span(c, sizeof(*c)) ||
        !separate(s, sizeof(*s), c, sizeof(*c)) ||
        !span(c->firmware, c->firmware_bytes) ||
        !separate(s, sizeof(*s), c->firmware, c->firmware_bytes) ||
        !pvr_fw_requirements(c->firmware, c->firmware_bytes, PVR_FW_A733_BVNC, &plan) ||
        c->state.bvnc != PVR_FW_A733_BVNC || !c->state.gpu_clock_hz ||
        c->state.clusters != 1 || c->state.slc_kilobytes != 16) return 0;
    /* Cross-component alias checks precede every write. The individual builders
     * validate their own subsets; neither alone knows all output allocations. */
    for (unsigned i = 0; i < 3 + PVR_FW_RESOURCE_COUNT; ++i) {
        const struct pvr_mips_buffer *b = buffer(c, i);
        size_t expected = i < 2 ? plan.allocation_bytes[i] : i == 2 ? PVR_MIPS_TABLE_BYTES :
            pvr_fw_state_layout((enum pvr_fw_resource)(i - 3))->allocation_bytes;
        if (b->storage.bytes != expected || b->page_count != expected / PVR_MIPS_PAGE_BYTES ||
            !span(b->storage.cpu, expected) ||
            !span(b->pages, b->page_count * sizeof(uint64_t)) ||
            !separate(s, sizeof(*s), b->pages, b->page_count * sizeof(uint64_t)) ||
            !separate(b->storage.cpu, expected, s, sizeof(*s)) ||
            !separate(b->storage.cpu, expected, c, sizeof(*c)) ||
            !separate(b->storage.cpu, expected, c->firmware, c->firmware_bytes)) return 0;
        for (unsigned j = 0; j < 3 + PVR_FW_RESOURCE_COUNT; ++j) {
            const struct pvr_mips_buffer *a = buffer(c, j);
            /* Validate lengths before pointer arithmetic even for later entries. */
            if (a->page_count > PVR_FW_MAX_ALLOCATION / PVR_MIPS_PAGE_BYTES ||
                !span(a->pages, a->page_count * sizeof(uint64_t)) ||
                !separate(b->storage.cpu, expected, a->pages, a->page_count * sizeof(uint64_t))) return 0;
            if (j < i && !separate(b->storage.cpu, expected, a->storage.cpu, a->storage.bytes)) return 0;
        }
        for (unsigned p = 0; p < b->page_count; ++p) {
            uint64_t pa = b->pages[p];
            if (!pa || (pa & 4095) || pa >= (UINT64_C(1) << 36) ||
                (pa >= 0x1fc00000 && pa < 0x1fc04000)) return 0;
            for (unsigned j = 0; j <= i; ++j) {
                const struct pvr_mips_buffer *a = buffer(c, j);
                unsigned count = j == i ? p : a->page_count;
                for (unsigned q = 0; q < count; ++q) if (a->pages[q] == pa) return 0;
            }
        }
    }
    return 1;
}
static void fail(struct pvr_gpu_service *s, enum pvr_gpu_service_error error)
{
    s->phase = PVR_GPU_SERVICE_FAILED; s->error = error;
    (void)pvr_gpu_start_cancel(&s->start);
    (void)pvr_gpu_control_abort(&s->control);
    (void)pvr_ccb_abort(&s->queue);
}
static int deadline(struct pvr_gpu_service *s, uint64_t duration)
{
    uint64_t now;
    if (!s->config.start.now(s->config.context, &now) || now < s->last_now ||
        duration > UINT64_MAX - now) { fail(s, PVR_GPU_SERVICE_CLOCK); return 0; }
    s->last_now = now; s->deadline = now + duration; return 1;
}
static int prepare(struct pvr_gpu_service *s)
{
    const struct pvr_gpu_service_config *c = &s->config;
    memset(&s->queue, 0, sizeof(s->queue)); memset(&s->start, 0, sizeof(s->start));
    memset(&s->control, 0, sizeof(s->control)); memset(&s->stop, 0, sizeof(s->stop));
    if (!pvr_fw_state_prepare(c->resources, &c->state, s->mappings) ||
        !pvr_mips_prepare(c->firmware, c->firmware_bytes, c->firmware_buffers,
            s->mappings, PVR_FW_RESOURCE_COUNT, &s->boot) ||
        !pvr_gpu_start_init(&s->start, c->firmware, c->firmware_bytes, &s->boot,
            &c->start, c->context) ||
        pvr_ccb_init(&s->queue, &c->queue, c->context, s->epoch) != PVR_CCB_OK) {
        fail(s, PVR_GPU_SERVICE_PREPARATION); return 0;
    }
    uint64_t now;
    if (!c->start.now(c->context, &now) || now < s->last_now || now >= s->deadline) {
        fail(s, PVR_GPU_SERVICE_CLOCK); return 0;
    }
    s->last_now = now;
    if (!c->enable(c->context)) { fail(s, PVR_GPU_SERVICE_ENABLE); return 0; }
    if (pvr_gpu_start_begin(&s->start, s->deadline) != PVR_GPU_START_AGAIN ||
        s->start.last_now < s->last_now) { fail(s, PVR_GPU_SERVICE_START); return 0; }
    s->last_now = s->start.last_now; s->phase = PVR_GPU_SERVICE_STARTING; return 1;
}
static void advance(struct pvr_gpu_service *s)
{
    uint64_t now = s->last_now;
    switch (s->phase) {
    case PVR_GPU_SERVICE_STARTING: {
        enum pvr_gpu_start_result r = pvr_gpu_start_poll(&s->start);
        now = s->start.last_now;
        if (r == PVR_GPU_START_OK) {
            if (!pvr_gpu_control_init(&s->control, &s->start, &s->queue,
                    &s->config.control, s->config.context, s->deadline)) {
                fail(s, PVR_GPU_SERVICE_CONTROL); return;
            }
            s->phase = PVR_GPU_SERVICE_CHECKING;
        } else if (r != PVR_GPU_START_AGAIN) { fail(s, PVR_GPU_SERVICE_START); return; }
        break;
    }
    case PVR_GPU_SERVICE_CHECKING:
    case PVR_GPU_SERVICE_READY: {
        enum pvr_gpu_control_result r = pvr_gpu_control_poll(&s->control, 4);
        now = s->control.last_now;
        if (r == PVR_GPU_CONTROL_OK) s->phase = PVR_GPU_SERVICE_READY;
        else if (r != PVR_GPU_CONTROL_AGAIN) { fail(s, PVR_GPU_SERVICE_CONTROL); return; }
        break;
    }
    case PVR_GPU_SERVICE_STOPPING: {
        enum pvr_gpu_stop_result r = pvr_gpu_stop_poll(&s->stop);
        now = s->stop.last_now;
        if (r == PVR_GPU_STOP_OK) {
            s->phase = PVR_GPU_SERVICE_STOPPED;
        } else if (r != PVR_GPU_STOP_AGAIN) { fail(s, PVR_GPU_SERVICE_STOP); return; }
        break;
    }
    default: return;
    }
    if (now < s->last_now) { fail(s, PVR_GPU_SERVICE_CLOCK); return; }
    s->last_now = now;
    if (s->phase == PVR_GPU_SERVICE_STOPPED) s->owner = PVR_GPU_SERVICE_CLIENTS;
}
static enum pvr_gpu_service_result execute(struct pvr_gpu_service *s,
                                           const struct pvr_gpu_service_message *m)
{
    const struct pvr_gpu_service_request *r = &m->request;
    if ((unsigned)r->operation > PVR_GPU_SERVICE_CLOSE ||
        (r->operation == PVR_GPU_SERVICE_STATUS ? r->timeout_us != 0 :
         (!r->timeout_us || r->timeout_us > INT64_MAX))) return PVR_GPU_SERVICE_INVALID;
    if (r->operation == PVR_GPU_SERVICE_STATUS)
        return s->phase == PVR_GPU_SERVICE_FAILED ? PVR_GPU_SERVICE_FAULT : PVR_GPU_SERVICE_OK;
    if (r->operation == PVR_GPU_SERVICE_OPEN) {
        if (s->phase == PVR_GPU_SERVICE_FAILED) return PVR_GPU_SERVICE_FAULT;
        if (s->owner != PVR_GPU_SERVICE_CLIENTS) return PVR_GPU_SERVICE_BUSY;
        if ((s->phase != PVR_GPU_SERVICE_COLD && s->phase != PVR_GPU_SERVICE_STOPPED) ||
            s->epoch == UINT64_MAX) return PVR_GPU_SERVICE_BUSY;
        s->owner = m->client; ++s->epoch;
        if (!deadline(s, r->timeout_us) || !prepare(s)) return PVR_GPU_SERVICE_FAULT;
        return PVR_GPU_SERVICE_OK;
    }
    if (s->owner != m->client) return PVR_GPU_SERVICE_DENIED;
    if (s->phase == PVR_GPU_SERVICE_FAILED) return PVR_GPU_SERVICE_FAULT;
    if (s->phase != PVR_GPU_SERVICE_READY) return PVR_GPU_SERVICE_BUSY;
    if (!deadline(s, r->timeout_us)) return PVR_GPU_SERVICE_FAULT;
    if (!pvr_gpu_stop_init(&s->stop, &s->control, s->config.firmware,
            s->config.firmware_bytes, &s->config.stop, s->config.context, s->deadline)) {
        fail(s, PVR_GPU_SERVICE_STOP); return PVR_GPU_SERVICE_FAULT;
    }
    s->phase = PVR_GPU_SERVICE_STOPPING; return PVR_GPU_SERVICE_OK;
}
static void worker(void *argument)
{
    struct pvr_gpu_service *s = argument;
    uint64_t level, stack;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, spsel" : "=r"(level), "=r"(stack));
    configASSERT(level == 4);
#if configNUMBER_OF_CORES > 1
    configASSERT(stack == 0);
#else
    (void)stack;
#endif
    for (;;) {
        advance(s); /* One bounded step before each request; traffic cannot starve it. */
        int active = s->phase >= PVR_GPU_SERVICE_STARTING && s->phase <= PVR_GPU_SERVICE_STOPPING;
        struct pvr_gpu_service_message message;
        if (xQueueReceive(s->requests, &message, active ? 1 : portMAX_DELAY) != pdTRUE) continue;
        configASSERT(message.client < PVR_GPU_SERVICE_CLIENTS && message.ticket);
        struct pvr_gpu_service_reply reply = {.ticket = message.ticket};
        reply.result = execute(s, &message);
        reply.phase = s->phase; reply.error = s->error; reply.epoch = s->epoch;
        reply.owns_session = s->owner == message.client;
        reply.start_error = s->start.error; reply.control_error = s->control.error;
        reply.stop_error = s->stop.error;
        configASSERT(xQueueSend(s->clients[message.client].replies, &reply, 0) == pdTRUE);
    }
}
int pvr_gpu_service_init(struct pvr_gpu_service *s, const struct pvr_gpu_service_config *c,
                         UBaseType_t priority, UBaseType_t affinity)
{
    if (!s || instance || s->initialized || !c || !priority || priority >= configMAX_PRIORITIES ||
        !affinity || (affinity >> configNUMBER_OF_CORES) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED || !c->enable ||
        !c->queue.read || !c->queue.write || !c->queue.barrier || !c->queue.kick ||
        !c->start.now || !c->start.read || !c->start.write || !c->start.started || !c->start.publish ||
        c->control.now != c->start.now || !c->control.read32 || !c->control.write32 || !c->control.compat ||
        c->stop.now != c->start.now || !c->stop.read || !c->stop.write ||
        !c->stop.sync_write || !c->stop.sync_read || !c->stop.isolate || !allocations(s, c)) return 0;
    s->config = *c; s->owner = PVR_GPU_SERVICE_CLIENTS;
    s->requests = xQueueCreateStatic(PVR_GPU_SERVICE_CLIENTS, sizeof(struct pvr_gpu_service_message),
                                     s->request_storage, &s->request_state);
    configASSERT(s->requests);
    for (unsigned i = 0; i < PVR_GPU_SERVICE_CLIENTS; ++i) {
        struct pvr_gpu_service_client *client = &s->clients[i];
        client->replies = xQueueCreateStatic(1, sizeof(struct pvr_gpu_service_reply),
                                            client->reply_storage, &client->reply_state);
        configASSERT(client->replies);
    }
    instance = s; s->initialized = 1; creating_worker = 1;
#if configNUMBER_OF_CORES > 1
    s->worker = xTaskCreateStaticAffinitySet(worker, "gpu-service", PVR_GPU_SERVICE_STACK_WORDS,
        s, priority, s->worker_stack, &s->worker_state, affinity);
#else
    s->worker = xTaskCreateStatic(worker, "gpu-service", PVR_GPU_SERVICE_STACK_WORDS,
        s, priority, s->worker_stack, &s->worker_state);
#endif
    creating_worker = 0; configASSERT(s->worker); return 1;
}
int pvr_gpu_service_bind(struct pvr_gpu_service *s, unsigned slot, TaskHandle_t task)
{
    if (!s || s != instance || !s->initialized || slot >= PVR_GPU_SERVICE_CLIENTS || !task ||
        task == s->worker || s->clients[slot].task ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    for (unsigned i = 0; i < PVR_GPU_SERVICE_CLIENTS; ++i)
        if (s->clients[i].task == task) return 0;
    s->clients[slot].task = task; return 1;
}
static struct pvr_gpu_service_client *caller(struct pvr_gpu_service *s, unsigned *slot)
{
    if (!s || s != instance || !s->initialized ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return NULL;
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    for (unsigned i = 0; i < PVR_GPU_SERVICE_CLIENTS; ++i)
        if (s->clients[i].task == task) { *slot = i; return &s->clients[i]; }
    return NULL;
}
int pvr_gpu_service_send(struct pvr_gpu_service *s, const struct pvr_gpu_service_request *r,
                         TickType_t wait, uint64_t *ticket)
{
    if (!r || !ticket || wait == portMAX_DELAY) return 0;
    unsigned slot; struct pvr_gpu_service_client *c = caller(s, &slot);
    if (!c || c->pending || c->next_ticket == UINT64_MAX) return 0;
    struct pvr_gpu_service_message message = {*r, c->next_ticket + 1, slot};
    c->pending = message.ticket;
    if (xQueueSend(s->requests, &message, wait) != pdTRUE) { c->pending = 0; return 0; }
    c->next_ticket = message.ticket; *ticket = message.ticket; return 1;
}
int pvr_gpu_service_collect(struct pvr_gpu_service *s, uint64_t ticket,
                            struct pvr_gpu_service_reply *reply, TickType_t wait)
{
    if (!reply || !ticket || wait == portMAX_DELAY) return 0;
    unsigned slot; struct pvr_gpu_service_client *c = caller(s, &slot); (void)slot;
    if (!c || c->pending != ticket) return 0;
    struct pvr_gpu_service_reply value;
    if (xQueueReceive(c->replies, &value, wait) != pdTRUE) return 0;
    configASSERT(value.ticket == ticket); c->pending = 0; *reply = value; return 1;
}
