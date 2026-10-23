/* Shared-mode NPU service on two QEMU cores: three EL0 member tasks, one EL1
 * worker, the actual controller core and native memory policy. A register
 * model stands in for the device; it completes a job only when the check
 * allows it and writes each session's output from that session's input. */
#include "npu_service.h"
#include "a733_npu_native.h"
#include "platform.h"
#include <string.h>

#define IDLE 0x7fffffffu
#define MEMBERS 3u
#define BOOT_VA 0x10000000u
#define COMMAND_VA(i) (0x10100000u + (i) * 0x100000u)
#define DATA_VA(i) (0x11000000u + (i) * 0x4000u)
static struct npu_service service;
static struct a733_npu_native native;
static const struct a733_npu_ops *native_ops = &a733_npu_native_operations;
static _Alignas(4096) uint8_t arena[4096 + 2 * 16384];
static _Alignas(4096) uint8_t command_memory[MEMBERS + 1][524288];
static _Alignas(4096) uint8_t inputs[MEMBERS][4096], outputs[MEMBERS][4096];
static StaticTask_t tasks[MEMBERS + 1];
static StackType_t stacks[MEMBERS + 1][4096];
static TaskHandle_t members[MEMBERS];
static uint32_t stage, arrived[MEMBERS];

static struct {
    uint32_t regs[0x800 / 4];
    unsigned running, bootstrap, event_sent;
    uint64_t launched;
    uint32_t credits, accesses, finished, triggers;
    uint32_t order[32];
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
    uint64_t level;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(level));
    configASSERT(level == 4 && xTaskGetCurrentTaskHandle() == service.worker);
}
static void fatal(void *unused) { (void)unused; platform_panic("native shared backend", 0); }
static uint64_t clock_us(void *unused) { (void)unused; privileged(); return native_ops->microseconds(&native); }
static void delay(void *unused, unsigned us) { (void)unused; privileged(); native_ops->delay(&native, us); }
static void barrier(void *unused) { (void)unused; privileged(); native_ops->barrier(&native); }
static void advance(void)
{
    if (!model.running) return;
    uint64_t elapsed = native_ops->microseconds(&native) - model.launched;
    /* Each credit completes one bootstrap or job, so the check fixes the order. */
    if (model.event_sent || elapsed < 200 || !get(&model.credits)) return;
    __atomic_fetch_sub(&model.credits, 1, __ATOMIC_ACQ_REL); /* Sole decrementer. */
    model.regs[0x10/4] |= 1; model.event_sent = 1; model.regs[1] = IDLE; model.running = 0;
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
    model.regs[offset / 4] = value;
    if (offset == 0x3a8 && (value & 1)) { model.regs[0x388/4] = 0; model.regs[1] = IDLE; model.running = 0; }
    if (offset == 0x3a4) {
        configASSERT(!model.running && native.memory.state == A733_NPU_MEMORY_DEVICE);
        uint32_t n = get(&model.triggers);
        configASSERT(n < 32);
        model.order[n] = model.regs[0x654/4]; set(&model.triggers, n + 1);
        model.running = 1; model.event_sent = 0;
        model.bootstrap = model.regs[0x654/4] == BOOT_VA;
        model.regs[1] = 0; model.launched = native_ops->microseconds(&native);
    }
    return 1;
}
static int publish(void *unused, const struct a733_npu_mmu_image *image,
                   const struct a733_npu_command *command, int bootstrap)
{ (void)unused; privileged(); return native_ops->publish(&native, image, command, bootstrap); }
static int finish(void *unused, const struct a733_npu_command *command, int canceled)
{
    (void)unused; privileged();
    configASSERT(!canceled && !model.running && model.event_sent);
    /* Software device stand-in: each command writes only its own output. */
    for (unsigned i = 0; i < MEMBERS && !model.bootstrap; ++i)
        if (command->device == COMMAND_VA(i))
            for (unsigned j = 0; j < 4096; ++j) outputs[i][j] = inputs[i][j] ^ (uint8_t)(0x40 + i);
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
    configASSERT(reply.ticket == ticket);
    return reply;
}
static uint64_t send(enum npu_service_operation op, unsigned command, uint64_t timeout)
{
    struct npu_service_request r = {op, command, timeout};
    uint64_t ticket;
    configASSERT(npu_service_send(&service, &r, 100, &ticket));
    return ticket;
}
static struct npu_service_reply collect(uint64_t ticket, TickType_t wait, int expect)
{
    struct npu_service_reply reply = {0};
    configASSERT(npu_service_collect(&service, ticket, &reply, wait) == expect);
    configASSERT(!expect || reply.ticket == ticket);
    return reply;
}
static void wait_flag(const uint32_t *p, uint32_t value)
{
    TickType_t start = xTaskGetTickCount();
    while (get(p) < value) { configASSERT(xTaskGetTickCount() - start < 2000); vTaskDelay(1); }
}
static void wait_triggers(uint32_t value) { wait_flag(&model.triggers, value); }
/* A macro, so that a failure reports the caller's line. */
#define expect(reply, want_result, want_state, want_owns) do { \
        struct npu_service_reply r_ = (reply); \
        configASSERT(r_.result == (want_result) && r_.state == (want_state) && \
                     r_.owns_session == (want_owns)); \
    } while (0)
static void check_output(unsigned member)
{
    for (unsigned j = 0; j < 4096; ++j)
        configASSERT(outputs[member][j] == (uint8_t)(inputs[member][j] ^ (0x40 + member)));
}
static unsigned self(void)
{
    for (unsigned i = 0; i < MEMBERS; ++i) if (members[i] == xTaskGetCurrentTaskHandle()) return i;
    platform_panic("unknown member", 0);
}

static void credit(void) { __atomic_fetch_add(&model.credits, 1, __ATOMIC_RELEASE); }

/* Members B and C follow the stages that member A (the coordinator) sets. */
static void follower(void *unused)
{
    (void)unused;
    unsigned me = self(), command = me + 1;
    wait_flag(&stage, 1);
    /* Join while the bootstrap is still running; WAIT is held until READY. */
    expect(request(NPU_SERVICE_OPEN, 0, 100000), NPU_SERVICE_OK, A733_NPU_BOOTING, 1);
    configASSERT(request(NPU_SERVICE_SUBMIT, command, 100000).result == NPU_SERVICE_BUSY);
    set(&arrived[me], 1);
    expect(request(NPU_SERVICE_WAIT, 0, 0), NPU_SERVICE_OK, A733_NPU_READY, 1);
    wait_flag(&stage, 2 + me);
    /* Queue behind A's running job (B first, then C). The reply comes at once. */
    memset(inputs[me], 0x30 + me, sizeof(inputs[me]));
    struct npu_service_reply queued = request(NPU_SERVICE_SUBMIT, command, 1000000);
    expect(queued, NPU_SERVICE_OK, A733_NPU_RUNNING, 1);
    configASSERT(queued.job_ticket == queued.ticket);
    configASSERT(request(NPU_SERVICE_SUBMIT, command, 1000000).result == NPU_SERVICE_BUSY);
    set(&arrived[me], 2);
    struct npu_service_reply waited = request(NPU_SERVICE_WAIT, 0, 0);
    expect(waited, NPU_SERVICE_OK, A733_NPU_DONE, 1);
    configASSERT(waited.job_ticket == queued.ticket);
    check_output(me);
    /* C keeps its result unreaped while A runs again: no blocking. */
    set(&arrived[me], 3);
    wait_flag(&stage, 5);
    struct npu_service_reply reaped = request(NPU_SERVICE_REAP, 0, 0);
    expect(reaped, NPU_SERVICE_OK, A733_NPU_READY, 1);
    configASSERT(reaped.job_ticket == queued.ticket && !reaped.canceled);
    configASSERT(request(NPU_SERVICE_REAP, 0, 0).result == NPU_SERVICE_BUSY);
    if (me == 1) {
        /* A queued job can be canceled; it never reaches the device. */
        uint32_t triggers = get(&model.triggers);
        configASSERT(request(NPU_SERVICE_SUBMIT, command, 1000000).result == NPU_SERVICE_OK);
        expect(request(NPU_SERVICE_CANCEL, 0, 1000), NPU_SERVICE_OK, A733_NPU_CANCELED, 1);
        expect(request(NPU_SERVICE_WAIT, 0, 0), NPU_SERVICE_OK, A733_NPU_CANCELED, 1);
        configASSERT(request(NPU_SERVICE_CLOSE, 0, 0).result == NPU_SERVICE_BUSY);
        reaped = request(NPU_SERVICE_REAP, 0, 0);
        configASSERT(reaped.result == NPU_SERVICE_OK && reaped.canceled && reaped.state == A733_NPU_READY);
        configASSERT(get(&model.triggers) == triggers);
    }
    /* Leaving the session keeps it open for the remaining members. */
    expect(request(NPU_SERVICE_CLOSE, 0, 0), NPU_SERVICE_OK, A733_NPU_CLOSED, 0);
    configASSERT(request(NPU_SERVICE_SUBMIT, command, 1000000).result == NPU_SERVICE_DENIED);
    configASSERT(request(NPU_SERVICE_WAIT, 0, 0).result == NPU_SERVICE_DENIED);
    set(&arrived[me], 4);
    if (me == 1) {
        /* Rejoin, then hold a WAIT that ends with A's deadline fault. */
        wait_flag(&stage, 6);
        expect(request(NPU_SERVICE_OPEN, 0, 100000), NPU_SERVICE_OK, A733_NPU_READY, 1);
        configASSERT(request(NPU_SERVICE_SUBMIT, command, 1000000).result == NPU_SERVICE_OK);
        set(&arrived[me], 5);
        struct npu_service_reply failed = request(NPU_SERVICE_WAIT, 0, 0);
        configASSERT(failed.result == NPU_SERVICE_FAULT && failed.state == A733_NPU_FAULT &&
                     failed.fault == A733_NPU_TIMEOUT && failed.owns_session);
        configASSERT(request(NPU_SERVICE_REAP, 0, 0).result == NPU_SERVICE_FAULT);
        configASSERT(request(NPU_SERVICE_CLOSE, 0, 0).result == NPU_SERVICE_FAULT);
        set(&arrived[me], 6);
    }
    vTaskSuspend(NULL);
}
static void coordinator(void *unused)
{
    (void)unused;
    configASSERT(!npu_service_share(&service)); /* Only before scheduling. */
    for (unsigned op = NPU_SERVICE_SUBMIT; op <= NPU_SERVICE_WAIT; ++op)
        if (op != NPU_SERVICE_OPEN)
            configASSERT(request((enum npu_service_operation)op, op == NPU_SERVICE_SUBMIT,
                op == NPU_SERVICE_SUBMIT || op == NPU_SERVICE_CANCEL ? 100000 : 0).result ==
                NPU_SERVICE_DENIED);
    configASSERT(request(NPU_SERVICE_OPEN, 0, 0).result == NPU_SERVICE_INVALID);
    configASSERT(request(NPU_SERVICE_STATUS, 0, 5).result == NPU_SERVICE_INVALID);
    expect(request(NPU_SERVICE_STATUS, 0, 0), NPU_SERVICE_OK, A733_NPU_COLD, 0);
    expect(request(NPU_SERVICE_OPEN, 0, 1000000), NPU_SERVICE_OK, A733_NPU_BOOTING, 1);
    configASSERT(request(NPU_SERVICE_OPEN, 0, 1000000).result == NPU_SERVICE_BUSY);
    set(&stage, 1);
    wait_flag(&arrived[1], 1); wait_flag(&arrived[2], 1);
    credit();
    expect(request(NPU_SERVICE_WAIT, 0, 0), NPU_SERVICE_OK, A733_NPU_READY, 1);
    configASSERT(get(&model.triggers) == 1 && model.order[0] == BOOT_VA); /* One bootstrap. */
    configASSERT(request(NPU_SERVICE_SUBMIT, 2, 1000000).result == NPU_SERVICE_DENIED);
    configASSERT(request(NPU_SERVICE_SUBMIT, 9, 1000000).result == NPU_SERVICE_INVALID);

    /* A starts at once; B and then C queue behind it. */
    memset(inputs[0], 0x5a, sizeof(inputs[0]));
    struct npu_service_reply first = request(NPU_SERVICE_SUBMIT, 1, 1000000);
    expect(first, NPU_SERVICE_OK, A733_NPU_RUNNING, 1);
    wait_triggers(2);
    configASSERT(request(NPU_SERVICE_CANCEL, 0, 1000).result == NPU_SERVICE_BUSY);
    set(&stage, 3); wait_flag(&arrived[1], 2);
    set(&stage, 4); wait_flag(&arrived[2], 2);
    /* A's held WAIT and both followers' held WAITs coexist. */
    uint64_t held = send(NPU_SERVICE_WAIT, 0, 0);
    collect(held, 5, 0);
    credit();
    struct npu_service_reply waited = collect(held, 1000, 1);
    expect(waited, NPU_SERVICE_OK, A733_NPU_DONE, 1);
    configASSERT(waited.job_ticket == first.ticket);
    check_output(0);
    /* B now runs. A reaps and queues again behind C: when B completes, C is
     * next even though A has the lower client index. */
    wait_triggers(3);
    configASSERT(request(NPU_SERVICE_REAP, 0, 0).result == NPU_SERVICE_OK);
    memset(inputs[0], 0x6b, sizeof(inputs[0]));
    struct npu_service_reply second = request(NPU_SERVICE_SUBMIT, 1, 1000000);
    expect(second, NPU_SERVICE_OK, A733_NPU_RUNNING, 1);
    credit(); wait_flag(&arrived[1], 3);
    credit(); wait_flag(&arrived[2], 3);
    credit();
    /* C has not reaped; A's second job still completes. */
    expect(request(NPU_SERVICE_WAIT, 0, 0), NPU_SERVICE_OK, A733_NPU_DONE, 1);
    check_output(0);
    configASSERT(get(&model.triggers) == 5);
    configASSERT(model.order[1] == COMMAND_VA(0) && model.order[2] == COMMAND_VA(1) &&
                 model.order[3] == COMMAND_VA(2) && model.order[4] == COMMAND_VA(0));
    configASSERT(request(NPU_SERVICE_CLOSE, 0, 0).result == NPU_SERVICE_BUSY);
    configASSERT(request(NPU_SERVICE_REAP, 0, 0).result == NPU_SERVICE_OK);

    /* A occupies the device; B queues, cancels, and both followers leave. */
    configASSERT(request(NPU_SERVICE_SUBMIT, 1, 1000000).result == NPU_SERVICE_OK);
    wait_triggers(6);
    set(&stage, 5);
    wait_flag(&arrived[1], 4); wait_flag(&arrived[2], 4);
    credit();
    expect(request(NPU_SERVICE_WAIT, 0, 0), NPU_SERVICE_OK, A733_NPU_DONE, 1);
    configASSERT(request(NPU_SERVICE_REAP, 0, 0).result == NPU_SERVICE_OK);
    configASSERT(get(&model.triggers) == 6 && get(&model.finished) == 6);

    /* The last member's CLOSE closes the controller; a later OPEN boots again. */
    expect(request(NPU_SERVICE_CLOSE, 0, 0), NPU_SERVICE_OK, A733_NPU_CLOSED, 0);
    configASSERT(model.regs[0x14/4] == 0 && model.regs[0x388/4] == 0);
    expect(request(NPU_SERVICE_STATUS, 0, 0), NPU_SERVICE_OK, A733_NPU_CLOSED, 0);
    expect(request(NPU_SERVICE_OPEN, 0, 1000000), NPU_SERVICE_OK, A733_NPU_BOOTING, 1);
    credit();
    expect(request(NPU_SERVICE_WAIT, 0, 0), NPU_SERVICE_OK, A733_NPU_READY, 1);
    configASSERT(get(&model.triggers) == 7 && model.order[6] == BOOT_VA);

    /* A deadline fault is shared: every member's session is retained. */
    configASSERT(request(NPU_SERVICE_SUBMIT, 1, 100000).result == NPU_SERVICE_OK);
    wait_triggers(8);
    set(&stage, 6); wait_flag(&arrived[1], 5);
    struct npu_service_reply failed = request(NPU_SERVICE_WAIT, 0, 0);
    configASSERT(failed.result == NPU_SERVICE_FAULT && failed.state == A733_NPU_FAULT &&
                 failed.fault == A733_NPU_TIMEOUT && failed.owns_session);
    wait_flag(&arrived[1], 6);
    uint32_t accesses = get(&model.accesses);
    vTaskDelay(5);
    configASSERT(request(NPU_SERVICE_REAP, 0, 0).result == NPU_SERVICE_FAULT);
    configASSERT(request(NPU_SERVICE_SUBMIT, 1, 100000).result == NPU_SERVICE_FAULT);
    configASSERT(request(NPU_SERVICE_CLOSE, 0, 0).result == NPU_SERVICE_FAULT);
    configASSERT(get(&model.accesses) == accesses && get(&model.triggers) == 8);
    configASSERT(model.order[7] == COMMAND_VA(0));
    configASSERT(native.memory.state == A733_NPU_MEMORY_DEVICE);
    platform_puts("PASS NPU shared service: per-member sessions, round-robin dispatch, unreaped results, queued cancel, per-member WAIT, last-member close, and shared fault retention\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }
void kernel_main(void)
{
    platform_init();
    struct a733_npu_buffer buffers[1 + 3 * MEMBERS];
    buffers[0] = (struct a733_npu_buffer){(uintptr_t)command_memory[0], (uintptr_t)command_memory[0],
        sizeof(command_memory[0]), BOOT_VA, A733_NPU_BUFFER_BOOTSTRAP};
    for (unsigned i = 0; i < MEMBERS; ++i) {
        buffers[1 + 3 * i] = (struct a733_npu_buffer){(uintptr_t)command_memory[i + 1],
            (uintptr_t)command_memory[i + 1], sizeof(command_memory[i + 1]), COMMAND_VA(i),
            A733_NPU_BUFFER_COMMAND};
        buffers[2 + 3 * i] = (struct a733_npu_buffer){(uintptr_t)inputs[i], (uintptr_t)inputs[i],
            4096, DATA_VA(i), A733_NPU_BUFFER_READ};
        buffers[3 + 3 * i] = (struct a733_npu_buffer){(uintptr_t)outputs[i], (uintptr_t)outputs[i],
            4096, DATA_VA(i) + 0x2000, A733_NPU_BUFFER_WRITE};
    }
    configASSERT(a733_npu_bootstrap(command_memory[0], sizeof(command_memory[0])));
    configASSERT(a733_npu_native_init(&native, 0x09000000, 0x09000000,
        arena, sizeof(arena), (uintptr_t)arena, buffers, 1 + 3 * MEMBERS, fatal, NULL));
    for (unsigned i = 0; i < MEMBERS; ++i) {
        configASSERT(a733_npu_memory_group(&native.memory, COMMAND_VA(i), i + 1));
        configASSERT(a733_npu_memory_group(&native.memory, DATA_VA(i), i + 1));
        configASSERT(a733_npu_memory_group(&native.memory, DATA_VA(i) + 0x2000, i + 1));
    }
    struct a733_npu_command commands[MEMBERS + 1];
    configASSERT(a733_npu_memory_command(&native.memory, BOOT_VA, &commands[0]));
    for (unsigned i = 0; i < MEMBERS; ++i)
        configASSERT(a733_npu_memory_command(&native.memory, COMMAND_VA(i), &commands[i + 1]));
    model.regs[0x20/4] = 0x9000; model.regs[0x24/4] = 0x9202;
    model.regs[0x30/4] = 0x1000003b; model.regs[0x28/4] = 0x20240101; model.regs[1] = IDLE;
    configASSERT(!npu_service_share(&service));
    configASSERT(npu_service_init(&service, &model_ops, NULL, &native.memory.image, commands,
        MEMBERS + 1, 4, 1));
    /* A command inside internal SRAM would be refused at dispatch; sharing
     * refuses it up front. */
    service.commands[2].device = 0x400000;
    configASSERT(!npu_service_share(&service) && !service.shared);
    service.commands[2] = commands[2];
    configASSERT(npu_service_share(&service) && !npu_service_share(&service));
    members[0] = xTaskCreateStaticAffinitySet(coordinator, "member-a", 4096, NULL, 5, stacks[0], &tasks[0], 1);
    members[1] = xTaskCreateStaticAffinitySet(follower, "member-b", 4096, NULL, 5, stacks[1], &tasks[1], 2);
    members[2] = xTaskCreateStaticAffinitySet(follower, "member-c", 4096, NULL, 5, stacks[2], &tasks[2], 2);
    for (unsigned i = 0; i < MEMBERS; ++i) {
        configASSERT(members[i]);
        configASSERT(npu_service_bind(&service, i, members[i], UINT32_C(1) << (i + 1)));
    }
    vTaskStartScheduler(); platform_panic("scheduler returned", 0);
}
