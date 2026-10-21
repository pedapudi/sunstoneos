/* Shared NPU sessions on two QEMU cores: three catalogue models (one single-
 * tensor convolution and two two-input, two-output graphs), one EL0 client
 * task per model, the actual session runtime, shared service, controller,
 * memory policy and native cache operations. A register model stands in for
 * the device. On completion it reads each assembled descriptor through the
 * NPU page tables and writes every output from the input that descriptor
 * names, so a wrong relocation, mapping or session group changes the output.
 * Startup and platform polling are stand-ins; no hardware is involved.
 *
 * The case word at 0x47fff000 selects: 0 polled completion with fixed job
 * credits, 1 completion through the interrupt path, 2 a coefficient change
 * after admission, which must retain every session.
 *
 * Built with NPU_SESSION_RESERVED_CHECK, the check adds the published
 * ShuffleNet graph with its external working pool and one mimus encoder
 * linear layer with INT16 tensors and transpose steps, takes the parent from the
 * reservation constructor and a 48 MiB stand-in for the platform pool, and
 * requires a parent larger than the 4 MiB image-RAM window. Case 3 offers a
 * 4 MiB pool, which must refuse the reservation and leave the pool unused. */
#include "a733_npu_session_reserved.h"
#include "aarch64_device_memory.h"
#include "platform.h"
#include "npu_session_fixtures.h"
#include <string.h>

#define npu_case (*(volatile unsigned *)UINT64_C(0x47fff000))
#define IDLE 0x7fffffffu
#define JOBS 3u
#define REGISTER_CPU 0x09000000u
#define WINDOW_BYTES (4u << 20)

#if defined(NPU_SESSION_RESERVED_CHECK)
_Static_assert(FIXTURES == 5, "five catalogue models");
static struct a733_npu_session_reserved reserved;
#define sessions (reserved.sessions)
#define POOL_BASE 0x44000000u
static struct device_memory_pool pool;
/* Stand-in for the platform pool: 48 MiB of identity-mapped QEMU RAM. */
struct device_memory_pool *a733_memory_reservations_init(void)
{
    if (!pool.initialized)
        configASSERT(device_memory_pool_init(&pool, POOL_BASE, POOL_BASE, npu_case == 3 ? WINDOW_BYTES : 48u << 20));
    return &pool;
}
#define PARENT_CPU (reserved.extent.cpu)
#define PARENT_BYTES (reserved.extent.bytes)
#else
_Static_assert(FIXTURES == 3, "three catalogue models");
static struct npu_sessions sessions, refused;
static _Alignas(4096) uint8_t parent[3u << 20];
static uint8_t mutated[4096];
#define PARENT_CPU ((uintptr_t)parent)
#define PARENT_BYTES sizeof(parent)
#endif
static uint8_t inputs_storage[FIXTURES][2][FIXTURE_MAX_TENSOR], output_storage[FIXTURES][FIXTURE_MAX_TENSOR];
static struct a733_npu_startup startup;
static StaticTask_t tasks[FIXTURES + 1];
static StackType_t stacks[FIXTURES + 1][4096];
static TaskHandle_t clients[FIXTURES], bootstrap;
static uint32_t started, opened[FIXTURES], queued[FIXTURES], done[FIXTURES], closed[FIXTURES], faulted;
static uint32_t stops, irq_masked, irq_delivered;

static struct {
    uint32_t regs[0x800 / 4];
    unsigned running, bootstrap, event_sent;
    uint64_t launched;
    uint32_t credits, triggers, completed, accesses;
    uint32_t order[32];
} model;

extern const struct a733_npu_ops __real_a733_npu_native_operations;
static uint32_t get(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void set(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static void credit(void) { __atomic_fetch_add(&model.credits, 1, __ATOMIC_RELEASE); }
static uint64_t task_level(void)
{
    register uint64_t result __asm__("x0");
    __asm__ volatile("svc #120" : "=r"(result) :: "memory");
    return result;
}
static void wait_flag(const uint32_t *p, uint32_t value)
{
    TickType_t start = xTaskGetTickCount();
    while (get(p) < value) { configASSERT(xTaskGetTickCount() - start < 3000); vTaskDelay(1); }
}
static uint32_t word(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Device view: resolve a device address through the published tables. QEMU
 * RAM is identity mapped, so table and payload physical addresses are CPU
 * addresses. */
static uint8_t *walk(uint32_t va, int write)
{
    const uint8_t *arena = (const uint8_t *)sessions.native.memory.arena;
    uint32_t root = word(arena + (va >> 24) * 4);
    configASSERT((root & 15) == 1);
    uint32_t leaf = word((const uint8_t *)(uintptr_t)(root & ~63u) + ((va >> 12) & 4095) * 4);
    configASSERT((leaf & 3) == 3 && (!write || (leaf & 4)));
    return (uint8_t *)(uintptr_t)((leaf & 0xfffff000u) | (va & 4095));
}
static unsigned tensor_index(const struct npu_session *n, uint32_t va, unsigned first, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        if (n->buffers[NBG_ALLOC_INPUT + first + i].device == va) return i;
    return UINT32_MAX;
}
static uint8_t expected(unsigned m, unsigned output, uint8_t input)
{ return (uint8_t)(input + 17 * (m + 1) + 5 * output); }
/* The model input whose descriptor writes output j directly, or UINT32_MAX
 * when the output comes from intermediate memory. Mirrors execute(). */
static unsigned source_of_output(unsigned m, unsigned j)
{
    const struct npu_session *n = &sessions.session[m];
    uint32_t va = n->buffers[NBG_ALLOC_INPUT + n->layout.inputs + j].device;
    for (unsigned k = 0; k < n->layout.nn_operations; ++k) {
        const uint8_t *descriptor = (const uint8_t *)n->buffers[NBG_ALLOC_NN].cpu + k * 192;
        if (word(descriptor + 28) == va)
            return tensor_index(n, word(descriptor + 24), 0, n->layout.inputs);
    }
    platform_panic("output without a producing descriptor", j);
}
/* Execute the triggered command. Each NN descriptor whose output is a model
 * output writes it from the model input it reads, or from zero when it reads
 * intermediate memory. A working pool must arrive cleared through the device
 * view; the device then dirties all of it, as a graph's intermediates would. */
static void execute(uint32_t command)
{
    for (unsigned m = 0; m < sessions.count; ++m) {
        const struct npu_session *n = &sessions.session[m];
        if (n->buffers[NBG_ALLOC_COMMAND].device != command) continue;
        const struct a733_npu_buffer *pool_buffer = &n->buffers[NBG_ALLOC_POOL];
        for (uint32_t at = 0; at < pool_buffer->bytes; at += 4096) {
            uint8_t *page = walk(pool_buffer->device + at, 1);
            for (unsigned x = 0; x < 4096; ++x) configASSERT(!page[x]);
            memset(page, 0xee, 4096);
        }
        for (unsigned k = 0; k < n->layout.nn_operations; ++k) {
            const uint8_t *descriptor = walk(n->buffers[NBG_ALLOC_NN].device + k * 192, 0);
            uint32_t in_va = word(descriptor + 24), out_va = word(descriptor + 28);
            unsigned i = tensor_index(n, in_va, 0, n->layout.inputs);
            unsigned j = tensor_index(n, out_va, n->layout.inputs, n->layout.outputs);
            if (j == UINT32_MAX) continue;
            uint32_t out_bytes = n->layout.buffers[NBG_ALLOC_INPUT + n->layout.inputs + j].payload_bytes;
            uint8_t *out = walk(out_va, 1);
            configASSERT(out_bytes <= 4096 - (out_va & 4095));
            if (i == UINT32_MAX) {
                for (uint32_t x = 0; x < out_bytes; ++x) out[x] = expected(m, j, 0);
                continue;
            }
            uint32_t in_bytes = n->layout.buffers[NBG_ALLOC_INPUT + i].payload_bytes;
            configASSERT(in_bytes <= 4096 - (in_va & 4095));
            const uint8_t *in = walk(in_va, 0);
            for (uint32_t x = 0; x < out_bytes; ++x) out[x] = expected(m, j, in[x % in_bytes]);
        }
        return;
    }
    platform_panic("unknown command", command);
}
static void complete(void)
{
    model.regs[0x10/4] |= 1; model.event_sent = 1; model.regs[1] = IDLE; model.running = 0;
    if (!model.bootstrap) execute(model.regs[0x654/4]);
    __atomic_fetch_add(&model.completed, 1, __ATOMIC_RELEASE);
}
static void advance(void)
{
    if (!model.running || model.event_sent || npu_case == 1) return;
    uint64_t elapsed = __real_a733_npu_native_operations.microseconds(&sessions.native) - model.launched;
    if (elapsed < 200 || !get(&model.credits)) return;
    __atomic_fetch_sub(&model.credits, 1, __ATOMIC_ACQ_REL); /* Sole decrementer. */
    complete();
}
static void privileged(void)
{
    uint64_t level;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(level));
    configASSERT(level == 4 && xTaskGetCurrentTaskHandle() == sessions.service.worker);
}
static int read32(void *cookie, unsigned offset, uint32_t *value)
{
    configASSERT(cookie == &sessions.native && offset < sizeof(model.regs)); privileged();
    __atomic_fetch_add(&model.accesses, 1, __ATOMIC_RELEASE);
    advance(); *value = model.regs[offset / 4];
    if (offset == 0x10) model.regs[offset / 4] = 0;
    return 1;
}
static int write32(void *cookie, unsigned offset, uint32_t value)
{
    configASSERT(cookie == &sessions.native && offset < sizeof(model.regs)); privileged();
    __atomic_fetch_add(&model.accesses, 1, __ATOMIC_RELEASE);
    model.regs[offset / 4] = value;
    if (offset == 0x3a8 && (value & 1)) { model.regs[0x388/4] = 0; model.regs[1] = IDLE; model.running = 0; }
    if (offset == 0x3a4) {
        configASSERT(!model.running && sessions.native.memory.state == A733_NPU_MEMORY_DEVICE);
        uint32_t n = get(&model.triggers);
        configASSERT(n < 32);
        model.order[n] = model.regs[0x654/4]; set(&model.triggers, n + 1);
        model.running = 1; model.event_sent = 0;
        model.bootstrap = model.regs[0x654/4] == sessions.bootstrap.device;
        model.regs[1] = 0;
        model.launched = __real_a733_npu_native_operations.microseconds(&sessions.native);
    }
    return 1;
}
static uint64_t microseconds(void *cookie) { return __real_a733_npu_native_operations.microseconds(cookie); }
static void delay(void *cookie, unsigned us) { __real_a733_npu_native_operations.delay(cookie, us); }
static void barrier(void *cookie) { __real_a733_npu_native_operations.barrier(cookie); }
static int publish(void *cookie, const struct a733_npu_mmu_image *image,
                   const struct a733_npu_command *command, int bootstrap_command)
{ return __real_a733_npu_native_operations.publish(cookie, image, command, bootstrap_command); }
static int finish(void *cookie, const struct a733_npu_command *command, int canceled)
{ return __real_a733_npu_native_operations.finish(cookie, command, canceled); }
const struct a733_npu_ops __wrap_a733_npu_native_operations = {
    read32, write32, microseconds, delay, barrier, publish, finish};
int __real_aarch64_device_translate(uintptr_t, int, struct aarch64_translation *);
int __wrap_aarch64_device_translate(uintptr_t address, int write, struct aarch64_translation *out)
{
    int result = __real_aarch64_device_translate(address, write, out);
    /* Model only the NPU register page's physical address; RAM is unchanged. */
    if (result && address >= REGISTER_CPU && address < REGISTER_CPU + 4096)
        out->physical = address - REGISTER_CPU + 0x03600000;
    return result;
}

/* Startup stand-ins: the platform layer is outside this check. */
int a733_npu_startup_prepared(void *cookie, const struct a733_npu_ops *ops, void *backend,
    const struct a733_npu_mmu_image *image, const struct a733_npu_command *commands, unsigned count)
{
    (void)ops; (void)backend; (void)image; (void)commands;
    configASSERT(cookie == &startup && count == FIXTURES + 1);
    return 1;
}
enum a733_npu_platform_result a733_npu_startup_poll(struct a733_npu_startup *s)
{
    configASSERT(s == &startup);
    return npu_service_activate(&sessions.service) == NPU_SERVICE_OK ?
        A733_NPU_PLATFORM_READY : A733_NPU_PLATFORM_RETAINED;
}
enum a733_npu_platform_result a733_npu_startup_stop_poll(struct a733_npu_startup *s)
{
    configASSERT(s == &startup && sessions.service.controller.state == A733_NPU_CLOSED);
    ++stops;
    return A733_NPU_PLATFORM_READY;
}
BaseType_t npu_session_task_privileged(TaskFunction_t, void *);

/* Interrupt stand-ins for case 1: the rearm window delivers the event. */
static int irq_mask(void *cookie) { configASSERT(cookie == &sessions); irq_masked = 1; return 1; }
static int irq_rearm(void *cookie)
{
    configASSERT(cookie == &sessions && irq_masked);
    irq_masked = 0;
    if (!model.running) return 1;
    complete();
    irq_masked = 1; ++irq_delivered;
    BaseType_t woken = pdFALSE;
    npu_service_irq_notify_from_isr(&sessions.service, 1, &woken);
    return 1;
}
static int irq_stop(void *cookie) { configASSERT(cookie == &sessions && irq_masked); return 1; }
static const struct npu_service_irq_ops irq_ops = {irq_mask, irq_rearm, irq_stop};

static struct npu_service_reply call(unsigned m, enum npu_service_operation op,
                                     const struct nbg_span *inputs, unsigned count, uint64_t timeout)
{
    struct npu_service_reply reply;
    uint64_t ticket;
    configASSERT(npu_session_send(&sessions, m, op, inputs, count, timeout, 100, &ticket));
    configASSERT(npu_session_collect(&sessions, m, ticket, &reply, 3000));
    configASSERT(reply.ticket == ticket);
    return reply;
}
#define expect(reply, want_result, want_state) do { \
        struct npu_service_reply r_ = (reply); \
        configASSERT(r_.result == (want_result) && r_.state == (want_state)); \
    } while (0)

static void client(void *argument)
{
    unsigned m = (unsigned)(uintptr_t)argument;
    const struct fixture *f = &fixtures[m];
    configASSERT(task_level() == 0);
    wait_flag(&started, 1);
    struct npu_service_reply r = call(m, NPU_SERVICE_OPEN, NULL, 0, 1000000);
    configASSERT(r.result == NPU_SERVICE_OK && r.owns_session &&
                 (r.state == A733_NPU_BOOTING || r.state == A733_NPU_READY));
    set(&opened[m], 1);
    expect(call(m, NPU_SERVICE_WAIT, NULL, 0, 0), NPU_SERVICE_OK, A733_NPU_READY);
    /* Decoded descriptors match the catalogue's tensor sizes. */
    for (unsigned i = 0; i < f->inputs + f->outputs; ++i) {
        struct nbg_tensor t;
        configASSERT(npu_session_tensor(&sessions, m, i < f->inputs ? NBG_INPUT : NBG_OUTPUT,
                                        i < f->inputs ? i : i - f->inputs, &t));
        configASSERT(t.logical_bytes == f->tensor_bytes[i] && (t.format == 2 || t.format == 3 || t.format == 5) &&
                     t.quantization == 2);
    }
    struct nbg_tensor unused;
    configASSERT(!npu_session_tensor(&sessions, m, NBG_INPUT, f->inputs, &unused));
    /* Client refusals change nothing. */
    uint8_t (*inputs)[FIXTURE_MAX_TENSOR] = inputs_storage[m], *output = output_storage[m];
    struct nbg_span spans[2];
    uint64_t ticket = 77;
    for (unsigned i = 0; i < f->inputs; ++i) spans[i] = (struct nbg_span){inputs[i], f->tensor_bytes[i]};
    configASSERT(!npu_session_output(&sessions, m, 0, output, f->tensor_bytes[f->inputs]));
    configASSERT(!npu_session_send(&sessions, m, NPU_SERVICE_SUBMIT, spans, f->inputs - 1, 1000000, 100, &ticket));
    spans[0].bytes++;
    configASSERT(!npu_session_send(&sessions, m, NPU_SERVICE_SUBMIT, spans, f->inputs, 1000000, 100, &ticket));
    spans[0].bytes--;
    spans[0].data = (const unsigned char *)PARENT_CPU;
    configASSERT(!npu_session_send(&sessions, m, NPU_SERVICE_SUBMIT, spans, f->inputs, 1000000, 100, &ticket));
    spans[0].data = inputs[0];
    configASSERT(!npu_session_send(&sessions, (m + 1) % FIXTURES, NPU_SERVICE_STATUS, NULL, 0, 0, 100, &ticket));
    configASSERT(!npu_session_send(&sessions, m, NPU_SERVICE_REAP, NULL, 0, 0, 100, &ticket) && ticket == 77);

    if (npu_case == 2) {
        /* After an integrity fault the runtime refuses every client call. */
        if (m == 1) {
            wait_flag(&faulted, 1); /* Coefficients changed after admission. */
            for (unsigned i = 0; i < f->inputs; ++i) memcpy(inputs[i], f->tensor[i], f->tensor_bytes[i]);
            r = call(m, NPU_SERVICE_SUBMIT, spans, f->inputs, 1000000);
            configASSERT(r.result == NPU_SERVICE_OK && r.state == A733_NPU_RUNNING);
            TickType_t start = xTaskGetTickCount();
            while (!atomic_load(&sessions.failed)) { configASSERT(xTaskGetTickCount() - start < 1000); vTaskDelay(1); }
            configASSERT(!npu_session_send(&sessions, m, NPU_SERVICE_WAIT, NULL, 0, 0, 100, &ticket));
            set(&faulted, 2);
        } else {
            wait_flag(&faulted, 2);
            configASSERT(!npu_session_send(&sessions, m, NPU_SERVICE_STATUS, NULL, 0, 0, 100, &ticket));
            set(&closed[m], 1);
        }
        vTaskSuspend(NULL);
    }
    for (unsigned job = 0; job < JOBS; ++job) {
        for (unsigned i = 0; i < f->inputs; ++i)
            for (unsigned x = 0; x < f->tensor_bytes[i]; ++x)
                inputs[i][x] = (uint8_t)(f->tensor[i][x] ^ (job * 0x25));
        struct npu_service_reply submitted = call(m, NPU_SERVICE_SUBMIT, spans, f->inputs, 1000000);
        configASSERT(submitted.result == NPU_SERVICE_OK && submitted.state == A733_NPU_RUNNING &&
                     submitted.job_ticket == submitted.ticket);
        configASSERT(!npu_session_output(&sessions, m, 0, output, f->tensor_bytes[f->inputs]));
        set(&queued[m], job + 1);
        r = call(m, NPU_SERVICE_WAIT, NULL, 0, 0);
        configASSERT(r.result == NPU_SERVICE_OK && r.state == A733_NPU_DONE &&
                     r.job_ticket == submitted.ticket);
        for (unsigned j = 0; j < f->outputs; ++j) {
            size_t bytes = f->tensor_bytes[f->inputs + j];
            unsigned i = source_of_output(m, j);
            configASSERT(!npu_session_output(&sessions, m, j, output, bytes + 1));
            configASSERT(npu_session_output(&sessions, m, j, output, bytes));
            for (size_t x = 0; x < bytes; ++x)
                configASSERT(output[x] == expected(m, j, i == UINT32_MAX ? 0 : inputs[i][x % f->tensor_bytes[i]]));
        }
        configASSERT(!npu_session_output(&sessions, m, f->outputs, output, 1));
        r = call(m, NPU_SERVICE_REAP, NULL, 0, 0);
        configASSERT(r.result == NPU_SERVICE_OK && r.state == A733_NPU_READY && !r.canceled);
        set(&done[m], job + 1);
    }
    r = call(m, NPU_SERVICE_CLOSE, NULL, 0, 0);
    configASSERT(r.result == NPU_SERVICE_OK && r.state == A733_NPU_CLOSED && !r.owns_session);
    configASSERT(!npu_session_send(&sessions, m, NPU_SERVICE_OPEN, NULL, 0, 1000000, 100, &ticket));
    set(&closed[m], 1);
    vTaskSuspend(NULL);
}
static void bootstrap_run(void *unused)
{
    (void)unused;
    configASSERT(npu_session_stop_poll(&sessions) == A733_NPU_PLATFORM_RETAINED && !atomic_load(&sessions.failed));
    configASSERT(npu_session_startup_poll(&sessions) == A733_NPU_PLATFORM_READY);
    set(&started, 1);
    for (unsigned m = 0; m < FIXTURES; ++m) wait_flag(&opened[m], 1);
    if (npu_case != 1) credit(); /* Bootstrap completion. */
    if (npu_case == 2) {
        vTaskDelay(5);
        ((uint8_t *)sessions.session[1].buffers[NBG_ALLOC_COEFF].cpu)[3] ^= 1;
        uint32_t triggers = get(&model.triggers);
        set(&faulted, 1);
        for (unsigned m = 0; m < FIXTURES; ++m) wait_flag(&closed[m], m == 1 ? 0 : 1);
        wait_flag(&faulted, 2);
        uint32_t accesses = get(&model.accesses);
        vTaskDelay(5);
        configASSERT(get(&model.triggers) == triggers && get(&model.accesses) == accesses);
        configASSERT(sessions.service.controller.state == A733_NPU_FAULT &&
                     sessions.service.controller.fault == A733_NPU_MEMORY &&
                     atomic_load(&sessions.failed) && sessions.native.memory.state == A733_NPU_MEMORY_IDLE);
        configASSERT(npu_session_stop_poll(&sessions) == A733_NPU_PLATFORM_RETAINED && !stops);
        platform_puts("PASS NPU sessions: changed coefficients refused before publication; every session retained\n");
        platform_exit(0);
    }
    if (npu_case == 0)
        for (unsigned job = 0; job < FIXTURES * JOBS; ++job) {
            /* One credit at a time, each only once every model with work left
             * has its next job submitted; the next start is then the
             * round-robin choice among all models but the one that finished. */
            for (unsigned m = 0; m < FIXTURES; ++m)
                if (get(&done[m]) < JOBS) wait_flag(&queued[m], get(&done[m]) + 1);
            uint32_t before = 0, after;
            for (unsigned m = 0; m < FIXTURES; ++m) before += get(&done[m]);
            credit();
            TickType_t start = xTaskGetTickCount();
            for (after = before; after == before;) {
                after = 0;
                for (unsigned m = 0; m < FIXTURES; ++m) after += get(&done[m]);
                if (after != before) break;
                configASSERT(xTaskGetTickCount() - start < 3000); vTaskDelay(1);
            }
        }
    for (unsigned m = 0; m < FIXTURES; ++m) wait_flag(&closed[m], 1);
    configASSERT(get(&model.triggers) == 1 + FIXTURES * JOBS && model.order[0] == sessions.bootstrap.device);
    /* Round-robin: whenever all models keep work queued, the start order repeats
     * with period FIXTURES, so no model runs twice before the others run once. */
    if (npu_case == 0)
        for (unsigned k = 1; k + FIXTURES < 1 + FIXTURES * JOBS; ++k)
            configASSERT(model.order[k] == model.order[k + FIXTURES]);
    for (unsigned k = 1; k <= FIXTURES && npu_case == 0; ++k)
        for (unsigned j = 1; j < k; ++j) configASSERT(model.order[k] != model.order[j]);
    if (npu_case == 1)
        configASSERT(npu_service_irq_completions(&sessions.service) == 1 + FIXTURES * JOBS &&
                     irq_delivered == 1 + FIXTURES * JOBS && npu_service_irq_stopped(&sessions.service));
    configASSERT(sessions.service.controller.state == A733_NPU_CLOSED);
    configASSERT(npu_session_stop_poll(&sessions) == A733_NPU_PLATFORM_READY && stops == 1);
#if defined(NPU_SESSION_RESERVED_CHECK)
    platform_puts("PASS NPU sessions: five models, including a mimus linear layer, in one pool reservation above the 4 MiB window, cleared working pool, round-robin jobs, close and stop\n");
#else
    platform_puts(npu_case == 1 ?
        "PASS NPU sessions: three models, interrupt completion, copied multi-tensor outputs, close and stop\n" :
        "PASS NPU sessions: three models, round-robin jobs, copied multi-tensor outputs through NPU tables, close and stop\n");
#endif
    platform_exit(0);
}
BaseType_t npu_session_task_privileged(TaskFunction_t entry, void *argument)
{ return entry == bootstrap_run || npu_service_task_privileged(entry, argument); }
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }

static void fatal(void *unused) { (void)unused; platform_panic("session native adapter", 0); }
static void layout_checks(void)
{
    const struct a733_npu_memory *memory = &sessions.native.memory;
    uint64_t end = 0;
    unsigned buffers = 1;
    for (size_t i = 0; i < memory->count; ++i) {
        const struct a733_npu_buffer *b = &memory->buffers[i];
        /* Sorted by device address: one unmapped guard page between allocations. */
        configASSERT(!i || b->device >= end + 4096);
        end = (uint64_t)b->device + b->bytes;
        configASSERT(b->device >= NPU_SESSION_DEVICE_BASE && b->cpu >= PARENT_CPU &&
                     b->cpu + b->bytes <= PARENT_CPU + PARENT_BYTES);
        if (b->role == A733_NPU_BUFFER_BOOTSTRAP) configASSERT(!memory->groups[i]);
    }
    for (unsigned m = 0; m < FIXTURES; ++m) {
        const struct npu_session *n = &sessions.session[m];
        configASSERT(n->layout.inputs == fixtures[m].inputs && n->layout.outputs == fixtures[m].outputs);
        for (unsigned i = 0; i < NBG_ALLOC_INPUT + n->layout.inputs + n->layout.outputs; ++i) {
            if (!n->buffers[i].bytes) { configASSERT(i == NBG_ALLOC_POOL); continue; }
            ++buffers;
            for (size_t k = 0; k < memory->count; ++k)
                if (memory->buffers[k].device == n->buffers[i].device)
                    configASSERT(memory->groups[k] == m + 1);
        }
    }
    configASSERT(buffers == memory->count);
}
void kernel_main(void)
{
    platform_init();
    struct npu_session_model models[NPU_SESSION_MODELS + 1];
    for (unsigned m = 0; m < FIXTURES; ++m) models[m] = (struct npu_session_model){fixtures[m].model, fixtures[m].bytes};
    size_t bytes = npu_session_requirements(models, FIXTURES);
    configASSERT(bytes && !(bytes & 4095));
#if defined(NPU_SESSION_RESERVED_CHECK)
    configASSERT((npu_case == 0 || npu_case == 3) && bytes > WINDOW_BYTES);
    configASSERT(!a733_npu_session_reserve(&reserved, 0, models, FIXTURES, REGISTER_CPU, fatal, NULL));
    configASSERT(!a733_npu_session_reserve(&reserved, 7, models, 0, REGISTER_CPU, fatal, NULL));
    configASSERT(!a733_npu_session_reserve(&reserved, 7, models, FIXTURES, REGISTER_CPU, NULL, NULL));
    struct npu_sessions *prepared = a733_npu_session_reserve(&reserved, 7, models, FIXTURES,
                                                             REGISTER_CPU, fatal, NULL);
    if (npu_case == 3) {
        /* The parent does not fit: nothing is admitted or allocated. */
        configASSERT(!prepared && !reserved.admitted && !sessions.prepared && !atomic_load(&sessions.failed));
        for (unsigned i = 0; i < DEVICE_MEMORY_MAX_ALLOCATIONS; ++i)
            configASSERT(pool.allocations[i].state == DEVICE_MEMORY_FREE);
        platform_puts("PASS NPU sessions: a reservation larger than the pool is refused with the pool unchanged\n");
        platform_exit(0);
    }
    enum device_memory_state state;
    struct device_memory_extent extent;
    configASSERT(prepared == &sessions && reserved.admitted && reserved.extent.bytes == bytes &&
                 reserved.extent.cpu >= POOL_BASE && reserved.extent.cpu + bytes <= POOL_BASE + (48u << 20) &&
                 device_memory_describe(&pool, reserved.lease, &extent, &state) &&
                 state == DEVICE_MEMORY_DEVICE && extent.bytes == bytes);
    configASSERT(!a733_npu_session_reserve(&reserved, 7, models, FIXTURES, REGISTER_CPU, fatal, NULL));
#else
    configASSERT(npu_case <= 2 && bytes <= sizeof(parent));
    /* Absent catalogue identity, count limits and size mismatch are refused. */
    configASSERT(fixtures[0].bytes <= sizeof(mutated));
    memcpy(mutated, fixtures[0].model, fixtures[0].bytes); mutated[fixtures[0].bytes - 1] ^= 1;
    struct npu_session_model wrong[2] = {{mutated, fixtures[0].bytes}, models[1]};
    configASSERT(!npu_session_requirements(wrong, 2) && !npu_session_requirements(models, 0));
    for (unsigned m = FIXTURES; m <= NPU_SESSION_MODELS; ++m) models[m] = models[0];
    configASSERT(!npu_session_requirements(models, NPU_SESSION_MODELS + 1));
    struct npu_session_config config = {.count = FIXTURES, .cpu = (uintptr_t)parent,
        .physical = (uintptr_t)parent, .bytes = bytes, .register_cpu = REGISTER_CPU,
        .register_physical = 0x03600000, .fatal = NULL};
    for (unsigned m = 0; m < FIXTURES; ++m) config.models[m] = models[m];
    struct npu_session_config bad = config;
    bad.register_physical = 0x03700000;
    configASSERT(!npu_session_prepare(&refused, &bad) && !atomic_load(&refused.failed));
    bad = config; bad.bytes -= 4096;
    configASSERT(!npu_session_prepare(&refused, &bad) && atomic_load(&refused.failed));
    bad = config; bad.models[1] = wrong[0];
    memset(&refused, 0, sizeof(refused));
    configASSERT(!npu_session_prepare(&refused, &bad) && atomic_load(&refused.failed));
    configASSERT(!npu_session_prepare(&sessions, &config)); /* A fatal callback is required. */
    memset(&sessions, 0, sizeof(sessions));
    config.fatal = fatal;
    configASSERT(npu_session_prepare(&sessions, &config) && !npu_session_prepare(&sessions, &config));
#endif
    layout_checks();

    model.regs[0x20/4] = 0x9000; model.regs[0x24/4] = 0x9202;
    model.regs[0x30/4] = 0x1000003b; model.regs[0x28/4] = 0x20240101; model.regs[1] = IDLE;
    struct a733_npu_handoff base = {.controller = 0x03600000, .command_admission = 1};
    configASSERT(npu_session_handoff(&sessions, &base, &startup.handoff));
    configASSERT(startup.handoff.command_count == FIXTURES + 1);
    bootstrap = xTaskCreateStaticAffinitySet(bootstrap_run, "bootstrap", 4096, NULL, 3,
                                             stacks[FIXTURES], &tasks[FIXTURES], 1);
    startup.initialized = 1; startup.task = bootstrap;
    for (unsigned m = 0; m < FIXTURES; ++m) {
        clients[m] = xTaskCreateStaticAffinitySet(client, "client", 4096, (void *)(uintptr_t)m, 5,
                                                  stacks[m], &tasks[m], m == 1 ? 2 : 1);
        configASSERT(clients[m]);
    }
    TaskHandle_t duplicate[FIXTURES];
    memcpy(duplicate, clients, sizeof(duplicate));
    duplicate[1] = clients[0];
    configASSERT(!npu_session_bind(&sessions, &startup, duplicate, 4, 1));
    duplicate[1] = bootstrap;
    configASSERT(!npu_session_bind(&sessions, &startup, duplicate, 4, 1));
    configASSERT(!npu_session_bind_irq(&sessions, &irq_ops, &sessions, 1));
    configASSERT(npu_session_bind(&sessions, &startup, clients, 4, 1));
    configASSERT(sessions.service.shared && !npu_session_bind(&sessions, &startup, clients, 4, 1));
    if (npu_case == 1) configASSERT(npu_session_bind_irq(&sessions, &irq_ops, &sessions, 1));
    vTaskStartScheduler(); platform_panic("scheduler returned", 0);
}
