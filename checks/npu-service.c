#include "npu_service.h"
#include "a733_npu_native.h"
#include "platform.h"
#include <stddef.h>
#include <string.h>

#define IDLE 0x7fffffffu
static struct npu_service service;
static struct a733_npu_native native;
static const struct a733_npu_ops *native_ops = &a733_npu_native_operations;
static _Alignas(4096) uint8_t arena[4096 + 16384];
static _Alignas(4096) uint8_t command_memory[3][524288];
static _Alignas(4096) uint8_t data[3 * 4096];
static StaticTask_t tasks[3];
static StackType_t stacks[3][4096];
static uint32_t stage, peer_done, peer_owns, peer_release, peer_waiting;
struct context { uint64_t general[32], vector[64], nzcv, fpcr, fpsr, reserved; };
struct gate { uint32_t ready, release, observed_core, complete; };
static struct gate migration;
extern void check_migrating_context(struct context *, struct context *, struct gate *);
_Static_assert(sizeof(struct context) == 800 && offsetof(struct context, nzcv) == 768, "assembly context");
_Static_assert(offsetof(struct gate, observed_core) == 8, "assembly gate");

static struct {
    uint32_t regs[0x800 / 4];
    unsigned running, bootstrap, event_sent, canceled;
    uint64_t launched;
    uint32_t allow_event, allow_idle, allow_cancel_idle, finished, accesses, validated, clock_samples;
} model;

static uint32_t get(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void set(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static uint64_t task_level(void)
{
    register uint64_t result __asm__("x0");
    __asm__ volatile("svc #120" : "=r"(result) :: "memory");
    return result;
}
static void privileged(void)
{
    uint64_t level, stack;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, spsel" : "=r"(level), "=r"(stack));
    configASSERT(level == 4 && stack == 0 && xTaskGetCurrentTaskHandle() == service.worker);
}
static void fatal(void *unused) { (void)unused; platform_panic("native service backend", 0); }
static uint64_t clock_us(void *unused)
{
    (void)unused; privileged();
    __atomic_fetch_add(&model.clock_samples, 1, __ATOMIC_RELEASE);
    return native_ops->microseconds(&native);
}
static void delay(void *unused, unsigned us)
{ (void)unused; privileged(); native_ops->delay(&native, us); }
static void barrier(void *unused)
{ (void)unused; privileged(); native_ops->barrier(&native); }
static void advance(void)
{
    if (!model.running) return;
    uint64_t elapsed = native_ops->microseconds(&native) - model.launched;
    if (model.canceled) {
        if (get(&model.allow_cancel_idle)) { model.running = 0; model.regs[1] = IDLE; }
        return;
    }
    if (!model.event_sent && elapsed >= 200 && (model.bootstrap || get(&model.allow_event))) {
        model.regs[0x10/4] |= 1; model.event_sent = 1;
    }
    if (model.event_sent && (model.bootstrap || get(&model.allow_idle))) {
        model.regs[1] = IDLE; model.running = 0;
    }
}
static int read32(void *unused, unsigned offset, uint32_t *value)
{
    (void)unused; privileged(); configASSERT(offset < sizeof(model.regs));
    __atomic_fetch_add(&model.accesses, 1, __ATOMIC_RELEASE);
    advance(); *value = model.regs[offset / 4];
    if (offset == 0x10) model.regs[offset / 4] = 0;
    return 1;
}
static int write32(void *unused, unsigned offset, uint32_t value)
{
    (void)unused; privileged(); configASSERT(offset < sizeof(model.regs));
    __atomic_fetch_add(&model.accesses, 1, __ATOMIC_RELEASE);
    uint32_t previous = model.regs[offset / 4];
    model.regs[offset / 4] = value;
    if (offset == 0x3a8) {
        if (value & 1) { model.regs[0x388/4] = 0; model.regs[1] = IDLE; model.running = 0; }
        if ((previous & 8) && !(value & 8)) { model.canceled = 1; model.regs[offset / 4] |= 16; }
    }
    if (offset == 0x3a4) {
        configASSERT(native.memory.state == A733_NPU_MEMORY_DEVICE && value == 0x1ffff);
        model.running = 1; model.event_sent = 0; model.canceled = 0;
        model.bootstrap = model.regs[0x654/4] == 0x10000000;
        model.regs[1] = 0; model.launched = native_ops->microseconds(&native);
    }
    return 1;
}
static int publish(void *unused, const struct a733_npu_mmu_image *image,
                   const struct a733_npu_command *command, int bootstrap)
{
    (void)unused; privileged();
    if (!get(&model.validated)) {
        struct context expected = {0}, actual = {0};
        for (unsigned i = 0; i < 31; ++i) expected.general[i] = UINT64_C(0xb1c2000000000000) | (i * 257u);
        for (unsigned i = 0; i < 64; ++i) expected.vector[i] = UINT64_C(0xd3e4000000000000) | (i * 259u);
        expected.nzcv = UINT64_C(0xa0000000); expected.fpcr = 2u << 22; expected.fpsr = 19;
        check_migrating_context(&expected, &actual, &migration);
        configASSERT(!memcmp(&expected, &actual, sizeof(expected)));
        configASSERT(portGET_CORE_ID() == 1 && task_level() == 4);
        privileged(); set(&model.validated, 1);
    }
    return native_ops->publish(&native, image, command, bootstrap);
}
static int finish(void *unused, const struct a733_npu_command *command, int canceled)
{
    (void)unused; privileged();
    configASSERT(!model.running && model.regs[1] == IDLE);
    configASSERT(canceled ? model.canceled : model.event_sent);
    /* Software device stand-in; QEMU cannot qualify noncoherent visibility. */
    if (!model.bootstrap && !canceled)
        for (unsigned i = 0; i < 4096; ++i) data[4096 + i] = (uint8_t)(command->device >> 19);
    int ok = native_ops->finish(&native, command, canceled);
    if (ok) __atomic_fetch_add(&model.finished, 1, __ATOMIC_RELEASE);
    return ok;
}
static const struct a733_npu_ops model_ops = {read32, write32, clock_us, delay, barrier, publish, finish};

static struct npu_service_reply request(enum npu_service_operation op, unsigned command, uint64_t timeout)
{
    struct npu_service_request r = {op, command, timeout};
    struct npu_service_reply reply;
    uint64_t ticket;
    configASSERT(task_level() == 0);
    configASSERT(npu_service_send(&service, &r, 100, &ticket));
    configASSERT(npu_service_collect(&service, ticket, &reply, 1000));
    configASSERT(reply.ticket == ticket && task_level() == 0);
    return reply;
}
static struct npu_service_reply wait_state(enum a733_npu_state state)
{
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        struct npu_service_reply r = request(NPU_SERVICE_STATUS, 0, 0);
        if (r.state == state) return r;
        configASSERT(xTaskGetTickCount() - start < 500 && r.state != A733_NPU_FAULT);
        vTaskDelay(1);
    }
}
static void wait_flag_budget(const uint32_t *p, uint32_t value, TickType_t budget)
{
    TickType_t start = xTaskGetTickCount();
    while (get(p) != value) { configASSERT(xTaskGetTickCount() - start < budget); vTaskDelay(1); }
}
static void wait_flag(const uint32_t *p, uint32_t value) { wait_flag_budget(p, value, 1000); }
static void supervisor(void *unused)
{
    (void)unused;
    configASSERT(task_level() == 0);
    struct npu_service_request r = {NPU_SERVICE_STATUS, 0, 0}; uint64_t ticket = 7;
    configASSERT(!npu_service_send(&service, &r, 0, &ticket) && ticket == 7);
    wait_flag_budget(&migration.ready, 1, 2000); /* Includes the deliberate one-second idle check. */
    configASSERT(get(&migration.observed_core) == 0);
    vTaskCoreAffinitySet(service.worker, 2);
    wait_flag(&migration.observed_core, 1);
    set(&migration.release, 1);
    wait_flag(&model.validated, 1);
    vTaskSuspend(NULL);
}
static void peer(void *unused)
{
    (void)unused; wait_flag_budget(&stage, 1, 2000);
    configASSERT(request(NPU_SERVICE_SUBMIT, 2, 100000).result == NPU_SERVICE_DENIED);
    configASSERT(request(NPU_SERVICE_CANCEL, 0, 100000).result == NPU_SERVICE_DENIED);
    configASSERT(request(NPU_SERVICE_REAP, 0, 0).result == NPU_SERVICE_DENIED);
    configASSERT(request(NPU_SERVICE_CLOSE, 0, 0).result == NPU_SERVICE_DENIED);
    configASSERT(request(NPU_SERVICE_OPEN, 0, 100000).result == NPU_SERVICE_BUSY);
    configASSERT(request(NPU_SERVICE_WAIT, 0, 0).result == NPU_SERVICE_DENIED);
    set(&peer_done, 1);
    wait_flag(&stage, 2);
    configASSERT(request(NPU_SERVICE_OPEN, 0, 100000).result == NPU_SERVICE_OK);
    /* WAIT replies once bootstrap leaves BOOTING, without STATUS polling. */
    struct npu_service_reply waited = request(NPU_SERVICE_WAIT, 0, 0);
    configASSERT(waited.result == NPU_SERVICE_OK && waited.state == A733_NPU_READY && waited.owns_session);
    configASSERT(request(NPU_SERVICE_WAIT, 0, 5).result == NPU_SERVICE_INVALID);
    set(&peer_owns, 1); wait_flag(&peer_release, 1);
    configASSERT(request(NPU_SERVICE_SUBMIT, 1, 100000).result == NPU_SERVICE_DENIED);
    struct npu_service_reply submitted = request(NPU_SERVICE_SUBMIT, 2, 100000);
    configASSERT(submitted.result == NPU_SERVICE_OK);
    /* The held reply arrives once, only after the job leaves RUNNING; the
     * owner task still gets STATUS replies meanwhile. */
    struct npu_service_request r = {NPU_SERVICE_WAIT, 0, 0}; uint64_t ticket;
    configASSERT(npu_service_send(&service, &r, 100, &ticket));
    configASSERT(!npu_service_collect(&service, ticket, &waited, 5));
    set(&peer_waiting, 1);
    configASSERT(npu_service_collect(&service, ticket, &waited, 1000));
    configASSERT(waited.ticket == ticket && waited.result == NPU_SERVICE_OK &&
        waited.state == A733_NPU_DONE && waited.job_ticket == submitted.ticket);
    vTaskDelay(2); configASSERT(!uxQueueMessagesWaiting(service.clients[1].replies));
    struct npu_service_reply reaped = request(NPU_SERVICE_REAP, 0, 0);
    configASSERT(reaped.result == NPU_SERVICE_OK && reaped.job_ticket == submitted.ticket && !reaped.canceled);
    configASSERT(request(NPU_SERVICE_CLOSE, 0, 0).result == NPU_SERVICE_OK);
    set(&peer_done, 2); vTaskSuspend(NULL);
}
static void owner(void *unused)
{
    (void)unused; configASSERT(task_level() == 0 && portGET_CORE_ID() == 0);
    struct npu_service_request r = {NPU_SERVICE_STATUS, 0, 0};
    struct npu_service_reply reply = {.ticket = 42}; uint64_t ticket, ignored = 99;
    configASSERT(!npu_service_send(&service, &r, portMAX_DELAY, &ignored) && ignored == 99);
    configASSERT(npu_service_send(&service, &r, 0, &ticket));
    /* Worker is lower priority on this same core, so this timeout is deterministic. */
    configASSERT(!npu_service_collect(&service, ticket, &reply, 0) && reply.ticket == 42);
    configASSERT(!npu_service_collect(&service, ticket + 1, &reply, 0));
    configASSERT(!npu_service_send(&service, &r, 0, &ignored) && ignored == 99);
    r.operation = NPU_SERVICE_OPEN; /* The queued copy must remain STATUS. */
    configASSERT(npu_service_collect(&service, ticket, &reply, 1000));
    configASSERT(reply.result == NPU_SERVICE_OK && reply.state == A733_NPU_COLD && !reply.owns_session);
    configASSERT(!npu_service_collect(&service, ticket, &reply, 0));
    vTaskDelay(2);
    uint32_t samples = get(&model.clock_samples);
    vTaskDelay(pdMS_TO_TICKS(1005));
    configASSERT(get(&model.clock_samples) > samples && !get(&model.accesses));
    configASSERT(request((enum npu_service_operation)-1, 0, 0).result == NPU_SERVICE_INVALID);
    configASSERT(request(NPU_SERVICE_OPEN, 0, 0).result == NPU_SERVICE_INVALID);
    configASSERT(request(NPU_SERVICE_OPEN, 0, 1000000).result == NPU_SERVICE_OK);
    wait_state(A733_NPU_READY); configASSERT(get(&model.validated));
    configASSERT(request(NPU_SERVICE_SUBMIT, 2, 100000).result == NPU_SERVICE_DENIED);
    configASSERT(request(NPU_SERVICE_SUBMIT, 0, 100000).result == NPU_SERVICE_INVALID);
    struct npu_service_reply submitted = request(NPU_SERVICE_SUBMIT, 1, 100000);
    configASSERT(submitted.result == NPU_SERVICE_OK && submitted.owns_session);
    set(&stage, 1); wait_flag(&peer_done, 1);
    configASSERT(request(NPU_SERVICE_CLOSE, 0, 0).result == NPU_SERVICE_BUSY);
    configASSERT(request(NPU_SERVICE_REAP, 0, 0).result == NPU_SERVICE_BUSY);
    configASSERT(request(NPU_SERVICE_SUBMIT, 1, 100000).result == NPU_SERVICE_BUSY);
    uint32_t finished = get(&model.finished);
    set(&model.allow_event, 1); vTaskDelay(3);
    configASSERT(get(&model.finished) == finished);
    set(&model.allow_idle, 1); vTaskDelay(3);
    configASSERT(get(&model.finished) == finished + 1); /* No request was needed to poll completion. */
    struct npu_service_reply reaped = request(NPU_SERVICE_REAP, 0, 0);
    configASSERT(reaped.result == NPU_SERVICE_OK && reaped.job_ticket == submitted.ticket && !reaped.canceled);
    configASSERT(request(NPU_SERVICE_REAP, 0, 0).result == NPU_SERVICE_BUSY);
    for (unsigned i = 0; i < 4096; ++i) {
        configASSERT(data[i] == 0x69 && data[8192 + i] == 0x69);
        configASSERT(data[4096 + i] == (uint8_t)(0x10080000u >> 19));
    }
    set(&model.allow_event, 0); set(&model.allow_idle, 0);
    configASSERT(request(NPU_SERVICE_SUBMIT, 1, 100000).result == NPU_SERVICE_OK);
    configASSERT(request(NPU_SERVICE_CANCEL, 0, 100).result == NPU_SERVICE_INVALID);
    configASSERT(request(NPU_SERVICE_CANCEL, 0, 100000).result == NPU_SERVICE_OK);
    finished = get(&model.finished); vTaskDelay(3);
    configASSERT(get(&model.finished) == finished);
    configASSERT(request(NPU_SERVICE_STATUS, 0, 0).state == A733_NPU_CANCEL_WAIT);
    set(&model.allow_cancel_idle, 1); wait_state(A733_NPU_CANCELED);
    reaped = request(NPU_SERVICE_REAP, 0, 0);
    configASSERT(reaped.result == NPU_SERVICE_OK && reaped.canceled && reaped.state == A733_NPU_DRAINED);
    configASSERT(request(NPU_SERVICE_SUBMIT, 1, 100000).result == NPU_SERVICE_BUSY);
    reply = request(NPU_SERVICE_CLOSE, 0, 0);
    configASSERT(reply.result == NPU_SERVICE_OK && !reply.owns_session);
    set(&stage, 2); wait_flag(&peer_owns, 1);
    configASSERT(request(NPU_SERVICE_CANCEL, 0, 100000).result == NPU_SERVICE_DENIED);
    set(&peer_release, 1); wait_flag(&peer_waiting, 1);
    reply = request(NPU_SERVICE_STATUS, 0, 0);
    configASSERT(reply.result == NPU_SERVICE_OK && reply.state == A733_NPU_RUNNING && !reply.owns_session);
    set(&model.allow_event, 1); set(&model.allow_idle, 1); wait_flag(&peer_done, 2);
    configASSERT(request(NPU_SERVICE_OPEN, 0, 100000).result == NPU_SERVICE_OK);
    wait_state(A733_NPU_READY);
    set(&model.allow_event, 0); set(&model.allow_idle, 0);
    configASSERT(request(NPU_SERVICE_SUBMIT, 1, 5000).result == NPU_SERVICE_OK);
    /* A held WAIT ends with the controller deadline. */
    reply = request(NPU_SERVICE_WAIT, 0, 0);
    configASSERT(reply.result == NPU_SERVICE_FAULT && reply.fault == A733_NPU_TIMEOUT && reply.owns_session);
    reply = request(NPU_SERVICE_STATUS, 0, 0);
    configASSERT(reply.result == NPU_SERVICE_FAULT && reply.fault == A733_NPU_TIMEOUT && reply.owns_session);
    uint32_t accesses = get(&model.accesses); samples = get(&model.clock_samples); vTaskDelay(2);
    configASSERT(request(NPU_SERVICE_REAP, 0, 0).result == NPU_SERVICE_FAULT);
    configASSERT(request(NPU_SERVICE_CLOSE, 0, 0).result == NPU_SERVICE_FAULT);
    configASSERT(request(NPU_SERVICE_SUBMIT, 1, 100000).result == NPU_SERVICE_FAULT);
    configASSERT(request(NPU_SERVICE_CANCEL, 0, 100000).result == NPU_SERVICE_FAULT);
    configASSERT(get(&model.accesses) == accesses && native.memory.state == A733_NPU_MEMORY_DEVICE);
    configASSERT(get(&model.clock_samples) == samples);
    platform_puts("PASS NPU service: EL0 clients, preemptible EL1 worker migration/context, task ownership, reply timeouts, autonomous completion/cancel/deadline, held WAIT replies, and retained faults\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }
void kernel_main(void)
{
    platform_init();
    memset(data, 0x69, sizeof(data));
    struct a733_npu_buffer buffers[4];
    for (unsigned i = 0; i < 3; ++i)
        buffers[i] = (struct a733_npu_buffer){(uintptr_t)command_memory[i], (uintptr_t)command_memory[i],
            sizeof(command_memory[i]), 0x10000000 + i * 524288,
            i ? A733_NPU_BUFFER_COMMAND : A733_NPU_BUFFER_BOOTSTRAP};
    buffers[3] = (struct a733_npu_buffer){(uintptr_t)data + 4096, (uintptr_t)data + 4096,
        4096, 0x10180000, A733_NPU_BUFFER_WRITE};
    configASSERT(a733_npu_bootstrap(command_memory[0], sizeof(command_memory[0])));
    configASSERT(a733_npu_native_init(&native, 0x09000000, 0x09000000,
        arena, sizeof(arena), (uintptr_t)arena, buffers, 4, fatal, NULL));
    struct a733_npu_command commands[3];
    for (unsigned i = 0; i < 3; ++i)
        configASSERT(a733_npu_memory_command(&native.memory, buffers[i].device, &commands[i]));
    model.regs[0x20/4] = 0x9000; model.regs[0x24/4] = 0x9202;
    model.regs[0x30/4] = 0x1000003b; model.regs[0x28/4] = 0x20240101; model.regs[1] = IDLE;
    configASSERT(npu_service_init(&service, &model_ops, NULL, &native.memory.image, commands, 3, 4, 1));
    TaskHandle_t owners[2];
    owners[0] = xTaskCreateStaticAffinitySet(owner, "owner", 4096, NULL, 5, stacks[0], &tasks[0], 1);
    owners[1] = xTaskCreateStaticAffinitySet(peer, "peer", 4096, NULL, 5, stacks[1], &tasks[1], 2);
    configASSERT(owners[0] && owners[1]);
    configASSERT(!npu_service_bind(&service, 0, service.worker, 2));
    configASSERT(!npu_service_bind(&service, 0, owners[0], 1));
    configASSERT(npu_service_bind(&service, 0, owners[0], 2));
    configASSERT(!npu_service_bind(&service, 1, owners[0], 4));
    configASSERT(npu_service_bind(&service, 1, owners[1], 4));
    configASSERT(xTaskCreateStaticAffinitySet(supervisor, "supervisor", 4096, NULL, 6, stacks[2], &tasks[2], 2));
    vTaskStartScheduler(); platform_panic("scheduler returned", 0);
}
