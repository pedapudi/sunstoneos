/* NPU runtime on two QEMU cores: the five catalogue models (three small
 * convolution graphs, the published ShuffleNetV2 graph with its external
 * working pool and a mimus linear layer) loaded at runtime into four slots,
 * each owned by one EL0 client task, with the actual runtime, service,
 * controller, memory policy, assembler and native cache operations. A
 * register model stands in for the NPU. On completion it reads each assembled
 * descriptor through the NPU page tables and writes every model output from
 * the input that descriptor names, so a wrong relocation, mapping or slot
 * placement changes the output. It also requires the working pool to read as
 * zero through the NPU tables at the start of every job, refuses an event
 * register read while INTID 97 is unmasked and delivers the interrupt only
 * while it is unmasked. Startup and platform polling are stand-ins; no
 * hardware is involved.
 *
 * The case word at 0x47fff000 selects:
 *  0 four slots, three jobs each, started one at a time on credits in
 *    round-robin order. Slot 3 runs the mimus layer, then a LOAD of a changed
 *    catalogue model and three ShuffleNetV2 LOADs whose staged copy changes
 *    during assembly (a load-state record outside the templates, a relocation
 *    outside its region, a coefficient byte that only the staging recheck
 *    sees) are refused, and the shorter conv8 stream loaded afterwards leaves
 *    END pairs from its end to the fetch end.
 *  1 slots 0 to 2 run jobs that complete at once while slot 3 loads
 *    ShuffleNetV2: one LOAD step follows every trigger. ShuffleNetV2 then runs
 *    alone with garbage written over its pool and output before each SUBMIT,
 *    and the per-inference counters, register accesses included, are
 *    printed. Last, slots 0 and 1 submit
 *    while a ShuffleNetV2 job runs: its post-job check finishes before their
 *    replies.
 *  2 every interrupt arrives late, at the next rearm, and one mask is
 *    deferred, while slot 3 loads ShuffleNetV2: the idle-register hint
 *    completes each job within one step and a late notification never
 *    completes the next job. While a job runs with no event pending, the
 *    worker reads one register per step, the idle register.
 *  3 a descriptor byte changes while the slot is idle: A refuses the trigger
 *    and every slot is retained.
 *  4 a coefficient byte changes while the job runs: H fails after the job and
 *    every slot is retained. */
#include "npu_runtime.h"
#include "aarch64_device_memory.h"
#include "platform.h"
#include "npu_session_fixtures.h"
#include <string.h>

#define npu_case (*(volatile unsigned *)UINT64_C(0x47fff000))
#define IDLE 0x7fffffffu
#define JOBS 3u
#define SLOTS 4u
#define REGISTER_CPU 0x09000000u
#define PARENT_BASE 0x44000000u
#define PARENT_LIMIT (48u << 20)
_Static_assert(FIXTURES == 5, "five catalogue models");
enum { CONV8, DUAL_U8, DUAL_I8, SHUFFLENET, MIMUS };

static struct npu_runtime rt;
static uint8_t writable_conv8[4096] __attribute__((aligned(64)));
static struct a733_npu_startup startup;
static StaticTask_t tasks[SLOTS + 1];
static StackType_t stacks[SLOTS + 1][4096];
static TaskHandle_t clients[SLOTS], bootstrap;
static uint32_t started, opened[SLOTS], queued[SLOTS], done[SLOTS], closed[SLOTS], parked[SLOTS];
static uint32_t phase, faulted, stops, released[8], releases;
void decimal(uint64_t);

static struct {
    uint32_t regs[0x800 / 4];
    unsigned running, bootstrap, event_sent, immediate, late, masked, latched, defer_mask, mask_calls;
    uint32_t credits, triggers, completed, accesses, delivered, event_reads, reads;
    /* Register reads, steps and ticks at a job's trigger, and their growth
     * until completion summed over jobs. */
    uint64_t run_reads, run_steps, run_ticks, run_reads_total, run_steps_total, run_ticks_total;
    uint32_t order[256];
    uint64_t load_steps_at_trigger, completed_at_step, finished_at_step;
    unsigned loading_at_trigger;
} model;
static struct { uint64_t translations, digest, requests, replies; } count;
static uint32_t in_job[SLOTS], corrupt;

extern const struct a733_npu_ops __real_a733_npu_native_operations;
static uint32_t get(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void set(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static uint64_t task_level(void)
{
    register uint64_t result __asm__("x0");
    __asm__ volatile("svc #120" : "=r"(result) :: "memory");
    return result;
}
static void wait_flag(const uint32_t *p, uint32_t value)
{
    TickType_t start = xTaskGetTickCount();
    while (get(p) < value) {
        if (xTaskGetTickCount() - start >= 20000) {
            platform_puts("FAIL waiting at "); platform_hex((uintptr_t)p); platform_puts(" for "); platform_hex(value);
            platform_puts(" controller state "); platform_hex(rt.service.controller.state); platform_puts("\n");
        }
        configASSERT(xTaskGetTickCount() - start < 20000); vTaskDelay(1);
    }
}
static uint32_t word(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t steps(void)
{
    uint64_t total = 0;
    for (unsigned k = 0; k < NPU_RUNTIME_STEP_KINDS; ++k) total += rt.counters.steps[k];
    return total;
}

/* Device view: resolve a device address through the published tables. QEMU
 * RAM is identity mapped, so table and payload physical addresses are CPU
 * addresses. Writes need the write permission bit. */
static uint8_t *walk(uint32_t va, int write)
{
    const uint8_t *arena = (const uint8_t *)rt.native.memory.arena;
    uint32_t root = word(arena + (va >> 24) * 4);
    configASSERT((root & 15) == 1);
    uint32_t leaf = word((const uint8_t *)(uintptr_t)(root & ~63u) + ((va >> 12) & 4095) * 4);
    configASSERT((leaf & 3) == 3 && (!write || (leaf & 4)));
    return (uint8_t *)(uintptr_t)((leaf & 0xfffff000u) | (va & 4095));
}
static int writable(uint32_t va)
{
    const uint8_t *arena = (const uint8_t *)rt.native.memory.arena;
    uint32_t root = word(arena + (va >> 24) * 4);
    return (word((const uint8_t *)(uintptr_t)(root & ~63u) + ((va >> 12) & 4095) * 4) & 4) != 0;
}
static unsigned tensor_index(const struct npu_runtime_slot *s, uint32_t va, unsigned first, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        if (s->targets[NBG_ALLOC_INPUT + first + i].device == va) return i;
    return UINT32_MAX;
}
static uint8_t expected(unsigned m, unsigned output, uint8_t input)
{ return (uint8_t)(input + 17 * (m + 1) + 5 * output); }
/* The model input whose descriptor writes output j directly, or UINT32_MAX
 * when the output comes from intermediate memory. Mirrors execute(). */
static unsigned source_of_output(const struct npu_runtime_slot *s, unsigned j)
{
    uint32_t va = s->targets[NBG_ALLOC_INPUT + s->layout.inputs + j].device;
    for (unsigned k = 0; k < s->layout.nn_operations; ++k) {
        const uint8_t *descriptor = (const uint8_t *)s->targets[NBG_ALLOC_NN].cpu + k * 192;
        if (word(descriptor + 28) == va) return tensor_index(s, word(descriptor + 24), 0, s->layout.inputs);
    }
    platform_panic("output without a producing descriptor", j);
}
/* Execute the triggered command: the working pool must read as zero through
 * the device view and is then dirtied; descriptors come from the read-only
 * region, which the tables map without write permission. */
static void execute(uint32_t command)
{
    for (unsigned i = 0; i < rt.slots; ++i) {
        const struct npu_runtime_slot *s = &rt.slot[i];
        if (s->command.device != command) continue;
        configASSERT(!writable(s->command.device) && !writable(s->constant.device) && !writable(s->read.device) &&
                     writable(s->write.device));
        const struct nbg_assembly_buffer *pool = &s->targets[NBG_ALLOC_POOL];
        for (uint32_t at = 0; at < pool->bytes; at += 4096) {
            uint8_t *page = walk(pool->device + at, 1);
            for (unsigned x = 0; x < 4096; ++x) configASSERT(!page[x]);
            memset(page, 0xee, 4096);
        }
        for (unsigned k = 0; k < s->layout.nn_operations; ++k) {
            const uint8_t *descriptor = walk(s->targets[NBG_ALLOC_NN].device + k * 192, 0);
            uint32_t in_va = word(descriptor + 24), out_va = word(descriptor + 28);
            unsigned in = tensor_index(s, in_va, 0, s->layout.inputs);
            unsigned out = tensor_index(s, out_va, s->layout.inputs, s->layout.outputs);
            if (out == UINT32_MAX) continue;
            uint32_t out_bytes = s->layout.buffers[NBG_ALLOC_INPUT + s->layout.inputs + out].payload_bytes;
            uint8_t *o = walk(out_va, 1);
            configASSERT(out_bytes <= 4096 - (out_va & 4095));
            if (in == UINT32_MAX) {
                for (uint32_t x = 0; x < out_bytes; ++x) o[x] = expected(s->model, out, 0);
                continue;
            }
            uint32_t in_bytes = s->layout.buffers[NBG_ALLOC_INPUT + in].payload_bytes;
            configASSERT(in_bytes <= 4096 - (in_va & 4095));
            const uint8_t *p = walk(in_va, 0);
            for (uint32_t x = 0; x < out_bytes; ++x) o[x] = expected(s->model, out, p[x % in_bytes]);
        }
        return;
    }
    platform_panic("unknown command", command);
}
/* The modeled GIC: INTID 97 is delivered only while unmasked, and the ISR
 * entry masks it. A late interrupt stays latched until the next rearm. */
static void deliver(void)
{
    if (model.masked || model.late) { model.latched = 1; return; }
    model.masked = 1; ++model.delivered;
    BaseType_t woken = pdFALSE;
    taskENTER_CRITICAL();
    npu_service_irq_notify_from_isr(&rt.service, 1, &woken);
    taskEXIT_CRITICAL();
}
/* A job that runs across worker steps costs one register read per step,
 * four per one-tick timeout, and the bounded reads of its rearm, of one late
 * notification and of the completing read. */
static void running_reads(void)
{
    uint64_t reads = model.reads - model.run_reads, run = steps() - model.run_steps;
    uint64_t ticks = xTaskGetTickCount() - model.run_ticks;
    if (reads > run + 4 * (ticks + 1) + 8) {
        platform_puts("FAIL register reads while a job ran: "); decimal(reads);
        platform_puts(" in "); decimal(run); platform_puts(" steps and "); decimal(ticks);
        platform_puts(" ticks\n");
        configASSERT(0);
    }
    model.run_reads_total += reads; model.run_steps_total += run; model.run_ticks_total += ticks;
}
static void complete(void)
{
    model.regs[0x10/4] |= 1; model.event_sent = 1; model.regs[1] = IDLE; model.running = 0;
    if (!model.bootstrap && !model.immediate) running_reads();
    if (!model.bootstrap) execute(model.regs[0x654/4]);
    model.completed_at_step = steps();
    __atomic_fetch_add(&model.completed, 1, __ATOMIC_RELEASE);
    deliver();
}
static void advance(void)
{
    if (!model.running || model.event_sent || !get(&model.credits)) return;
    __atomic_fetch_sub(&model.credits, 1, __ATOMIC_ACQ_REL); /* Sole decrementer. */
    complete();
}
static void privileged(void)
{
    uint64_t level;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(level));
    configASSERT(level == 4 && xTaskGetCurrentTaskHandle() == rt.service.worker);
}
static int read32(void *cookie, unsigned offset, uint32_t *value)
{
    configASSERT(cookie == &rt.native && offset < sizeof(model.regs)); privileged();
    __atomic_fetch_add(&model.accesses, 1, __ATOMIC_RELEASE);
    ++model.reads;
    advance();
    if (offset == 0x10) { configASSERT(model.masked); ++model.event_reads; }
    *value = model.regs[offset / 4];
    if (offset == 0x10) model.regs[offset / 4] = 0;
    return 1;
}
static int write32(void *cookie, unsigned offset, uint32_t value)
{
    configASSERT(cookie == &rt.native && offset < sizeof(model.regs)); privileged();
    __atomic_fetch_add(&model.accesses, 1, __ATOMIC_RELEASE);
    model.regs[offset / 4] = value;
    if (offset == 0x3a8 && (value & 1)) { model.regs[0x388/4] = 0; model.regs[1] = IDLE; model.running = 0; }
    if (offset == 0x3a4) {
        /* No trigger while a post-job check is in progress. */
        configASSERT(!model.running && model.masked && rt.checking >= SLOTS);
        uint32_t n = get(&model.triggers);
        uint32_t command = model.regs[0x654/4];
        model.bootstrap = command == rt.bootstrap.device;
        unsigned slot = SLOTS;
        for (unsigned i = 0; i < rt.slots; ++i) if (rt.slot[i].command.device == command) slot = i;
        configASSERT(model.bootstrap || slot < SLOTS);
        if (n < 256) model.order[n] = model.bootstrap ? UINT32_MAX : slot;
        /* One LOAD step follows every trigger while a load is pending. */
        unsigned loading = rt.loading < SLOTS;
        if (!model.bootstrap && loading && model.loading_at_trigger)
            configASSERT(rt.counters.steps[NPU_RUNTIME_STEP_LOAD] > model.load_steps_at_trigger);
        model.loading_at_trigger = loading && !model.bootstrap;
        model.load_steps_at_trigger = rt.counters.steps[NPU_RUNTIME_STEP_LOAD];
        set(&model.triggers, n + 1);
        model.running = 1; model.event_sent = 0; model.regs[1] = 0;
        model.run_reads = model.reads; model.run_steps = steps(); model.run_ticks = xTaskGetTickCount();
        if (model.bootstrap || model.immediate) complete();
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

/* Counters: translations and digest bytes anywhere after admission, the
 * step at which the worker invalidates a job's output (its completion). */
int __real_aarch64_device_translate(uintptr_t, int, struct aarch64_translation *);
int __wrap_aarch64_device_translate(uintptr_t address, int write, struct aarch64_translation *out)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ++count.translations;
    int result = __real_aarch64_device_translate(address, write, out);
    /* Model only the NPU register page's physical address; RAM is unchanged. */
    if (result && address >= REGISTER_CPU && address < REGISTER_CPU + 4096)
        out->physical = address - REGISTER_CPU + 0x03600000;
    return result;
}
int __real_storage_digest_add(struct storage_digest *, const void *, size_t);
int __wrap_storage_digest_add(struct storage_digest *d, const void *data, size_t bytes)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) count.digest += bytes;
    return __real_storage_digest_add(d, data, bytes);
}
static struct { uintptr_t cpu; size_t bytes; enum aarch64_cache_op op; } flushed[64];
static unsigned flushes;
int __real_aarch64_device_cache(enum aarch64_cache_op, uintptr_t, size_t);
int __wrap_aarch64_device_cache(enum aarch64_cache_op op, uintptr_t address, size_t bytes)
{
    if (op == AARCH64_CACHE_INVALIDATE && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        !rt.service.controller.bootstrap) model.finished_at_step = steps();
    if (op == AARCH64_CACHE_CLEAN_INVALIDATE && flushes < 64)
        flushed[flushes++] = (typeof(flushed[0])){address, bytes, op};
    return __real_aarch64_device_cache(op, address, bytes);
}
static int was_flushed(uintptr_t cpu, size_t bytes)
{
    for (unsigned i = 0; i < flushes; ++i)
        if (flushed[i].cpu <= cpu && cpu + bytes <= flushed[i].cpu + flushed[i].bytes) return 1;
    return 0;
}
/* Case 0: change the staged ShuffleNetV2 copy during assembly, once per
 * armed load: a load-state record outside the templates, a relocation source
 * offset outside its region, then a coefficient byte, which the assembler
 * does not read. */
int __real_nbg_assembly_next(const struct nbg_model *, const struct nbg_assembly_buffer *,
                             struct nbg_assembly_layout *, struct nbg_assembly_cursor *, unsigned);
int __wrap_nbg_assembly_next(const struct nbg_model *m, const struct nbg_assembly_buffer *b,
                             struct nbg_assembly_layout *l, struct nbg_assembly_cursor *c, unsigned n)
{
    if (corrupt && c->operation >= 60) {
        if (corrupt == 1) {
            struct nbg_span state;
            configASSERT(nbg_model_table(m, NBG_LOAD_STATES_TABLE, 70, &state) && state.bytes >= 8);
            ((uint8_t *)state.data)[4] ^= 0x40;
        } else if (corrupt == 3) {
            struct nbg_span coeff;
            configASSERT(nbg_model_section(m, NBG_COEFF, &coeff) && coeff.bytes > 64);
            ((uint8_t *)coeff.data)[coeff.bytes / 2] ^= 1;
        } else {
            struct nbg_operation op;
            configASSERT(nbg_model_operation(m, 70, &op) && op.patch_count >= 2);
            unsigned stride = m->header.version == NBG_MODEL_DIRECTORY_VERSION ? 28 : 40;
            uint8_t *patch = (uint8_t *)m->data + m->sections[NBG_PATCH].offset + (op.first_patch + 1) * stride;
            patch[18] ^= 0x7f;
        }
        corrupt = 0;
    }
    return __real_nbg_assembly_next(m, b, l, c, n);
}

/* The order in which held replies are released. */
void __real_npu_service_release(struct npu_service *, unsigned, enum npu_service_result, enum a733_npu_state);
void __wrap_npu_service_release(struct npu_service *s, unsigned client, enum npu_service_result result,
                                enum a733_npu_state state)
{
    if (get(&phase) == 3 && releases < 8) released[releases++] = client;
    __real_npu_service_release(s, client, result, state);
}

/* Startup stand-ins: the platform layer is outside this check. */
int a733_npu_startup_prepared(void *cookie, const struct a733_npu_ops *ops, void *backend,
    const struct a733_npu_mmu_image *image, const struct a733_npu_command *commands, unsigned n)
{
    (void)ops; (void)backend; (void)image; (void)commands;
    configASSERT(cookie == &startup && n == SLOTS + 1);
    return 1;
}
enum a733_npu_platform_result a733_npu_startup_poll(struct a733_npu_startup *s)
{
    configASSERT(s == &startup);
    return npu_service_activate(&rt.service) == NPU_SERVICE_OK ? A733_NPU_PLATFORM_READY :
                                                                 A733_NPU_PLATFORM_RETAINED;
}
enum a733_npu_platform_result a733_npu_startup_stop_poll(struct a733_npu_startup *s)
{
    configASSERT(s == &startup && rt.service.controller.state == A733_NPU_CLOSED);
    ++stops;
    return A733_NPU_PLATFORM_READY;
}
BaseType_t npu_session_task_privileged(TaskFunction_t, void *);

static int irq_mask(void *cookie)
{
    configASSERT(cookie == &rt);
    if (++model.mask_calls == model.defer_mask) return 0;
    model.masked = 1;
    return 1;
}
static int irq_rearm(void *cookie)
{
    configASSERT(cookie == &rt && model.masked);
    model.masked = 0;
    if (model.latched) {
        model.latched = 0; model.masked = 1; ++model.delivered;
        BaseType_t woken = pdFALSE;
        npu_service_irq_notify_from_isr(&rt.service, 1, &woken);
    }
    return 1;
}
static int irq_stop(void *cookie) { configASSERT(cookie == &rt && model.masked); return 1; }
static const struct npu_service_irq_ops irq_ops = {irq_mask, irq_rearm, irq_stop};

/* Client helpers. */
static struct npu_service_reply call(unsigned slot, enum npu_service_operation op, unsigned m, uint64_t timeout)
{
    struct npu_service_reply reply;
    uint64_t ticket;
    configASSERT(npu_runtime_send(&rt, slot, op, m, timeout, 100, &ticket));
    if (in_job[slot]) ++count.requests;
    if (op == NPU_SERVICE_SUBMIT) set(&queued[slot], get(&queued[slot]) + 1);
    configASSERT(npu_runtime_collect(&rt, slot, ticket, &reply, 20000));
    if (in_job[slot]) ++count.replies;
    configASSERT(reply.ticket == ticket);
    return reply;
}
/* The slot's read-only bytes equal a one-call assembly of the same model at
 * the same device addresses into scratch memory: descriptors, coefficients
 * with their padding, and the whole fetch span of the command allocation. */
#define SCRATCH_BASE (PARENT_BASE + (24u << 20))
#define SCRATCH_BYTES (3u << 20)
static void same_as_one_call(unsigned slot)
{
    const struct npu_runtime_slot *s = &rt.slot[slot];
    const struct fixture *f = &fixtures[s->model];
    struct nbg_model view;
    struct nbg_assembly_buffer b[NBG_LAYOUT_MAX_BUFFERS];
    struct nbg_assembly_layout l;
    unsigned count = NBG_ALLOC_INPUT + s->layout.inputs + s->layout.outputs;
    uintptr_t at = SCRATCH_BASE + slot * SCRATCH_BYTES;
    configASSERT(rt.parent_bytes <= SCRATCH_BASE - PARENT_BASE);
    for (unsigned i = 0; i < count; ++i) {
        b[i] = s->targets[i];
        if (!b[i].bytes) continue;
        b[i].cpu = (void *)at; at += b[i].bytes;
    }
    configASSERT(at <= SCRATCH_BASE + (slot + 1) * SCRATCH_BYTES);
    configASSERT(nbg_model_open(f->model, f->bytes, &view) == NBG_DECODE_OK &&
                 nbg_assemble_layout(&view, b, count, &l) && !memcmp(&l, &s->layout, sizeof(l)));
    for (unsigned i = NBG_ALLOC_NN; i <= NBG_ALLOC_COEFF; ++i)
        configASSERT(!memcmp(b[i].cpu, s->targets[i].cpu, b[i].bytes));
    configASSERT(!memcmp(b[NBG_ALLOC_COMMAND].cpu, (const void *)s->command.cpu, A733_NPU_FETCH_BYTES));
}
static void load(unsigned slot, unsigned m)
{
    struct npu_service_reply r = call(slot, NPU_SERVICE_LOAD, m, 0);
    configASSERT(r.result == NPU_SERVICE_OK && r.state == A733_NPU_READY &&
                 atomic_load(&rt.slot[slot].state) == NPU_RUNTIME_READY && rt.slot[slot].model == m);
    const struct fixture *f = &fixtures[m];
    for (unsigned i = 0; i < f->inputs + f->outputs; ++i) {
        struct npu_runtime_tensor t;
        configASSERT(npu_runtime_tensor(&rt, slot, i < f->inputs ? NBG_INPUT : NBG_OUTPUT,
                                        i < f->inputs ? i : i - f->inputs, &t));
        configASSERT(t.bytes == f->tensor_bytes[i] && t.tensor.logical_bytes == t.bytes &&
                     t.tensor.quantization == 2);
    }
    struct npu_runtime_tensor unused;
    configASSERT(!npu_runtime_tensor(&rt, slot, NBG_INPUT, f->inputs, &unused));
    same_as_one_call(slot);
}
static void refused_load(unsigned slot, unsigned m)
{
    struct npu_service_reply r = call(slot, NPU_SERVICE_LOAD, m, 0);
    configASSERT(r.result == NPU_SERVICE_INVALID && r.state == A733_NPU_READY &&
                 atomic_load(&rt.slot[slot].state) == NPU_RUNTIME_EMPTY);
    struct npu_runtime_tensor t;
    uint64_t ticket = 77;
    configASSERT(!npu_runtime_tensor(&rt, slot, NBG_INPUT, 0, &t) &&
                 !npu_runtime_send(&rt, slot, NPU_SERVICE_SUBMIT, 0, 1000000, 100, &ticket) && ticket == 77);
}
static void fill(unsigned slot, unsigned job)
{
    const struct fixture *f = &fixtures[rt.slot[slot].model];
    for (unsigned i = 0; i < f->inputs; ++i) {
        struct npu_runtime_tensor t;
        configASSERT(npu_runtime_tensor(&rt, slot, NBG_INPUT, i, &t));
        for (unsigned x = 0; x < t.bytes; ++x) ((uint8_t *)t.data)[x] = (uint8_t)(f->tensor[i][x] ^ (job * 0x25));
    }
}
static void verify(unsigned slot)
{
    const struct npu_runtime_slot *s = &rt.slot[slot];
    const struct fixture *f = &fixtures[s->model];
    for (unsigned j = 0; j < f->outputs; ++j) {
        struct npu_runtime_tensor out;
        configASSERT(npu_runtime_tensor(&rt, slot, NBG_OUTPUT, j, &out));
        unsigned i = source_of_output(s, j);
        struct npu_runtime_tensor in;
        if (i != UINT32_MAX) configASSERT(npu_runtime_tensor(&rt, slot, NBG_INPUT, i, &in));
        for (size_t x = 0; x < out.bytes; ++x)
            configASSERT(((const uint8_t *)out.data)[x] ==
                         expected(s->model, j, i == UINT32_MAX ? 0 : ((const uint8_t *)in.data)[x % in.bytes]));
    }
}
static struct npu_service_reply job(unsigned slot, unsigned number)
{
    fill(slot, number);
    struct npu_service_reply r = call(slot, NPU_SERVICE_SUBMIT, 0, 1000000);
    if (r.result == NPU_SERVICE_OK) {
        configASSERT(r.state == A733_NPU_DONE && r.job_ticket == r.ticket &&
                     atomic_load(&rt.slot[slot].state) == NPU_RUNTIME_READY);
        verify(slot);
    }
    set(&done[slot], get(&done[slot]) + 1);
    return r;
}
static void close_slot(unsigned slot)
{
    struct npu_service_reply r = call(slot, NPU_SERVICE_CLOSE, 0, 0);
    configASSERT(r.result == NPU_SERVICE_OK && r.state == A733_NPU_CLOSED && !r.owns_session);
    uint64_t ticket;
    configASSERT(!npu_runtime_send(&rt, slot, NPU_SERVICE_LOAD, CONV8, 0, 100, &ticket));
    set(&closed[slot], 1);
}
static void tail_is_end(unsigned slot)
{
    const struct npu_runtime_slot *s = &rt.slot[slot];
    const uint8_t *p = (const uint8_t *)s->command.cpu;
    configASSERT(s->end_mark == s->stream_bytes);
    for (size_t at = s->stream_bytes; at < A733_NPU_FETCH_BYTES; at += 8)
        configASSERT(word(p + at) == 0x10000000u && !word(p + at + 4));
}

static void client(void *argument)
{
    unsigned slot = (unsigned)(uintptr_t)argument;
    configASSERT(task_level() == 0);
    wait_flag(&started, 1);
    struct npu_service_reply r = call(slot, NPU_SERVICE_OPEN, 0, 1000000);
    configASSERT(r.result == NPU_SERVICE_OK && r.owns_session &&
                 (r.state == A733_NPU_BOOTING || r.state == A733_NPU_READY));
    set(&opened[slot], 1);
    uint64_t ticket = 77;
    configASSERT(!npu_runtime_send(&rt, slot, NPU_SERVICE_SUBMIT, 0, 1000000, 100, &ticket) && ticket == 77);
    configASSERT(!npu_runtime_send(&rt, slot, NPU_SERVICE_LOAD, FIXTURES, 0, 100, &ticket));
    configASSERT(!npu_runtime_send(&rt, (slot + 1) % SLOTS, NPU_SERVICE_STATUS, 0, 0, 100, &ticket));
    switch (npu_case) {
    case 0:
        if (slot < 3) {
            load(slot, slot);
            for (unsigned n = 0; n < JOBS; ++n) configASSERT(job(slot, n).result == NPU_SERVICE_OK);
        } else {
            load(slot, MIMUS);
            configASSERT(job(slot, 0).result == NPU_SERVICE_OK);
            uint32_t mimus_stream = rt.slot[slot].stream_bytes;
            writable_conv8[100] ^= 1;   /* A changed catalogue byte is refused at LOAD. */
            refused_load(slot, CONV8);
            writable_conv8[100] ^= 1;
            corrupt = 1; refused_load(slot, SHUFFLENET);
            uint32_t mark = rt.slot[slot].end_mark;
            configASSERT(mark > mimus_stream && !corrupt);
            corrupt = 2; refused_load(slot, SHUFFLENET);
            configASSERT(!corrupt && rt.slot[slot].end_mark >= mark);
            corrupt = 3; refused_load(slot, SHUFFLENET);
            configASSERT(!corrupt && rt.slot[slot].end_mark >= mark);
            load(slot, CONV8);   /* Also compares the whole fetch span with a one-call assembly. */
            configASSERT(rt.slot[slot].stream_bytes < mimus_stream);
            tail_is_end(slot);
            configASSERT(job(slot, 1).result == NPU_SERVICE_OK && job(slot, 2).result == NPU_SERVICE_OK);
        }
        close_slot(slot);
        break;
    case 1:
        if (slot < 3) {
            load(slot, slot);
            for (unsigned n = 0; !get(&phase); ++n) configASSERT(job(slot, n).result == NPU_SERVICE_OK);
            set(&parked[slot], 1);
            wait_flag(&phase, 3);
            if (slot < 2) {
                /* Submitted while the last ShuffleNetV2 job runs. */
                while (atomic_load(&rt.slot[3].state) != NPU_RUNTIME_RUNNING) vTaskDelay(1);
                configASSERT(job(slot, 99).result == NPU_SERVICE_OK);
            }
        } else {
            for (unsigned s = 0; s < 3; ++s) wait_flag(&done[s], 1);
            struct npu_runtime_counters loading = rt.counters;
            load(slot, SHUFFLENET);
            platform_puts("COUNT load=3");
            platform_puts(" load_steps="); decimal(rt.counters.steps[NPU_RUNTIME_STEP_LOAD] - loading.steps[NPU_RUNTIME_STEP_LOAD]);
            platform_puts(" sha256_bytes="); decimal(rt.counters.digest_bytes - loading.digest_bytes);
            platform_puts(" copied_bytes="); decimal(rt.counters.copied_bytes - loading.copied_bytes);
            platform_puts(" staging_check_bytes=");
            decimal(rt.counters.staging_check_bytes - loading.staging_check_bytes);
            platform_puts(" reference_bytes="); decimal(rt.counters.reference_bytes - loading.reference_bytes);
            platform_puts(" jobs_during_load="); decimal(rt.counters.jobs - loading.jobs);
            platform_puts("\n");
            set(&phase, 1);
            for (unsigned s = 0; s < 3; ++s) wait_flag(&parked[s], 1);
            struct npu_runtime_counters before = rt.counters;
            uint64_t translations = count.translations, digest = count.digest;
            in_job[slot] = 1;
            for (unsigned n = 0; n < JOBS; ++n) {
                /* Stray CPU writes to the pool and the output before SUBMIT. */
                memset(rt.slot[slot].targets[NBG_ALLOC_POOL].cpu, 0x5a, 4096);
                struct npu_runtime_tensor out;
                configASSERT(npu_runtime_tensor(&rt, slot, NBG_OUTPUT, 0, &out));
                memset(out.data, 0x5a, out.bytes);
                flushes = 0;
                configASSERT(job(slot, n).result == NPU_SERVICE_OK);
                configASSERT(was_flushed((uintptr_t)out.data, 64) &&
                             was_flushed((uintptr_t)rt.slot[slot].targets[NBG_ALLOC_POOL].cpu, 4096));
            }
            in_job[slot] = 0;
            const struct npu_runtime_counters *after = &rt.counters;
            /* Per inference: one request and reply, no translation or digest. */
            configASSERT(count.requests == JOBS && count.replies == JOBS &&
                         count.translations == translations && count.digest == digest);
            platform_puts("COUNT model=3 jobs=3");
#define FIELD(name, value) do { platform_puts(" " name "="); decimal(value); } while (0)
            FIELD("requests", count.requests); FIELD("replies", count.replies);
            FIELD("translations", count.translations - translations);
            FIELD("sha256_bytes", count.digest - digest);
            FIELD("cache_lines", after->cache_lines - before.cache_lines);
            FIELD("table_bytes", after->table_bytes - before.table_bytes);
            FIELD("address_bytes", after->address_bytes - before.address_bytes);
            FIELD("end_tail_bytes", after->tail_bytes - before.tail_bytes);
            FIELD("readonly_bytes", after->readonly_bytes - before.readonly_bytes);
            FIELD("zeroed_bytes", after->zeroed_bytes - before.zeroed_bytes);
            FIELD("copied_bytes", after->copied_bytes - before.copied_bytes);
            FIELD("load_steps", after->steps[NPU_RUNTIME_STEP_LOAD] - before.steps[NPU_RUNTIME_STEP_LOAD]);
            FIELD("prepare_steps", after->steps[NPU_RUNTIME_STEP_PREPARE] - before.steps[NPU_RUNTIME_STEP_PREPARE]);
            FIELD("pool_steps", after->steps[NPU_RUNTIME_STEP_POOL] - before.steps[NPU_RUNTIME_STEP_POOL]);
            FIELD("check_steps", after->steps[NPU_RUNTIME_STEP_CHECK] - before.steps[NPU_RUNTIME_STEP_CHECK]);
            FIELD("register_reads", after->register_reads - before.register_reads);
            FIELD("register_writes", after->register_writes - before.register_writes);
            platform_puts("\n");
            set(&phase, 2);
            wait_flag(&phase, 3);
            /* The last job runs until the two other slots have queued. */
            configASSERT(job(slot, JOBS).result == NPU_SERVICE_OK);
        }
        close_slot(slot);
        break;
    case 2:
        if (slot == 0) {
            load(slot, CONV8);
            /* The jobs run while slot 3 loads, so bulk work is pending. */
            while (atomic_load(&rt.slot[3].state) != NPU_RUNTIME_LOADING) vTaskDelay(1);
            for (unsigned n = 0; n < 4; ++n) configASSERT(job(slot, n).result == NPU_SERVICE_OK);
            set(&phase, 1);
        } else if (slot == 3) {
            while (!get(&phase)) load(slot, SHUFFLENET);
        }
        close_slot(slot);
        break;
    default:
        if (slot == 0) {
            load(slot, CONV8);
            configASSERT(job(slot, 0).result == NPU_SERVICE_OK);
            set(&faulted, 1);
            wait_flag(&faulted, 2);
            r = job(slot, 1);
            configASSERT(r.result == NPU_SERVICE_FAULT && r.state == A733_NPU_FAULT);
            set(&faulted, 3);
        } else if (slot == 1) {
            load(slot, DUAL_U8);
            wait_flag(&faulted, 3);
            uint64_t t;
            configASSERT(!npu_runtime_send(&rt, slot, NPU_SERVICE_SUBMIT, 0, 1000000, 100, &t));
        }
        set(&closed[slot], 1);
        vTaskSuspend(NULL);
    }
    vTaskSuspend(NULL);
}

void decimal(uint64_t value)
{
    char text[24];
    unsigned at = sizeof(text) - 1;
    text[at] = 0;
    do { text[--at] = (char)('0' + value % 10); value /= 10; } while (value);
    platform_puts(text + at);
}
static void credit(void) { __atomic_fetch_add(&model.credits, 1, __ATOMIC_RELEASE); }
static uint32_t total(const uint32_t *v) { uint32_t t = 0; for (unsigned i = 0; i < SLOTS; ++i) t += get(&v[i]); return t; }
static void finish_case(const char *text)
{
    static const char *const kinds[NPU_RUNTIME_STEP_KINDS] = {"load", "prepare", "pool", "start", "check"};
    platform_puts("COUNT steps");
    for (unsigned k = 0; k < NPU_RUNTIME_STEP_KINDS; ++k) {
        platform_puts(" "); platform_puts(kinds[k]); platform_puts("="); decimal(rt.counters.steps[k]);
        platform_puts(" longest_"); platform_puts(kinds[k]); platform_puts("_us="); decimal(rt.counters.longest[k]);
    }
    platform_puts("\n");
    for (unsigned s = 0; s < SLOTS; ++s) wait_flag(&closed[s], 1);
    configASSERT(rt.service.controller.state == A733_NPU_CLOSED && npu_service_irq_stopped(&rt.service));
    configASSERT(npu_runtime_stop_poll(&rt) == A733_NPU_PLATFORM_READY && stops == 1);
    configASSERT(npu_service_irq_completions(&rt.service) == 1 + rt.counters.jobs);
    platform_puts(text);
    platform_exit(0);
}
static void bootstrap_run(void *unused)
{
    (void)unused;
    configASSERT(npu_runtime_stop_poll(&rt) == A733_NPU_PLATFORM_RETAINED && !atomic_load(&rt.failed));
    configASSERT(npu_runtime_startup_poll(&rt) == A733_NPU_PLATFORM_READY);
    set(&started, 1);
    for (unsigned s = 0; s < SLOTS; ++s) wait_flag(&opened[s], 1);
    if (npu_case == 0) {
        /* One credit at a time, each once every slot with work left has its
         * next job submitted; starts then follow round-robin order. */
        for (unsigned n = 0; n < SLOTS * JOBS; ++n) {
            for (unsigned s = 0; s < SLOTS; ++s)
                if (get(&done[s]) < JOBS) wait_flag(&queued[s], get(&done[s]) + 1);
            uint32_t before = total(done);
            credit();
            TickType_t start = xTaskGetTickCount();
            while (total(done) == before) { configASSERT(xTaskGetTickCount() - start < 20000); vTaskDelay(1); }
        }
        configASSERT(get(&model.triggers) == 1 + SLOTS * JOBS && model.order[0] == UINT32_MAX);
        for (unsigned k = 1; k + SLOTS < 1 + SLOTS * JOBS; ++k) configASSERT(model.order[k] == model.order[k + SLOTS]);
        for (unsigned k = 1; k <= SLOTS; ++k)
            for (unsigned j = 1; j < k; ++j) configASSERT(model.order[k] != model.order[j]);
        configASSERT(rt.counters.refused_loads == 4 && rt.counters.loads == 5);
        finish_case("PASS NPU runtime: four slots, round-robin starts, model switch, refused loads keep the END tail\n");
    }
    if (npu_case == 1) {
        wait_flag(&phase, 2);
        /* Slots 0 and 1 queue behind a running ShuffleNetV2 job and all three
         * complete on credits granted together. ShuffleNetV2's check, the
         * longest, finishes before the next start and before either reply. */
        model.immediate = 0;
        uint32_t base[SLOTS];
        for (unsigned s = 0; s < SLOTS; ++s) base[s] = get(&done[s]);
        set(&phase, 3);
        for (unsigned s = 0; s < 2; ++s) wait_flag(&queued[s], base[s] + 1);
        wait_flag(&queued[3], JOBS + 1);
        TickType_t start = xTaskGetTickCount();
        while (atomic_load(&rt.slot[3].state) != NPU_RUNTIME_RUNNING ||
               atomic_load(&rt.slot[0].state) != NPU_RUNTIME_PREPARED ||
               atomic_load(&rt.slot[1].state) != NPU_RUNTIME_PREPARED) {
            configASSERT(xTaskGetTickCount() - start < 20000); vTaskDelay(1);
        }
        credit(); credit(); credit();
        for (unsigned s = 0; s < SLOTS; ++s) if (s != 2) wait_flag(&done[s], base[s] + 1);
        configASSERT(releases == 3 && released[0] == 3 && released[1] == 0 && released[2] == 1);
        configASSERT(rt.counters.loads == 4 && !rt.counters.refused_loads);
        finish_case("PASS NPU runtime: jobs run while ShuffleNetV2 loads, in-place pool and output preparation, checks before replies\n");
    }
    if (npu_case == 2) {
        for (unsigned n = 0; n < 4; ++n) {
            wait_flag(&queued[0], n + 1);
            vTaskDelay(2);
            uint32_t completed = get(&model.completed);
            configASSERT(get(&done[0]) == n);
            credit();
            wait_flag(&done[0], n + 1);
            configASSERT(get(&model.completed) == completed + 1);
            /* The hint completes the job within one step of full idle. */
            configASSERT(model.finished_at_step <= model.completed_at_step + 1);
        }
        configASSERT(rt.service.runtime->idle_hints >= 4 && model.delivered >= 4 && model.mask_calls > model.defer_mask);
        platform_puts("COUNT running_jobs=4 running_reads="); decimal(model.run_reads_total);
        platform_puts(" running_steps="); decimal(model.run_steps_total);
        platform_puts(" running_ticks="); decimal(model.run_ticks_total); platform_puts("\n");
        finish_case("PASS NPU runtime: late interrupts, idle hint completion within a step, deferred mask retried\n");
    }
    /* Cases 3 and 4: integrity failures retain every slot. */
    wait_flag(&faulted, 1);
    uint32_t triggers = get(&model.triggers);
    if (npu_case == 3) {
        ((uint8_t *)rt.slot[0].targets[NBG_ALLOC_NN].cpu)[40] ^= 1;
        set(&faulted, 2);
    } else {
        model.immediate = 0;
        set(&faulted, 2);
        wait_flag(&queued[0], 2);
        TickType_t start = xTaskGetTickCount();
        while (atomic_load(&rt.slot[0].state) != NPU_RUNTIME_RUNNING) {
            configASSERT(xTaskGetTickCount() - start < 20000); vTaskDelay(1);
        }
        ((uint8_t *)rt.slot[0].targets[NBG_ALLOC_COEFF].cpu)[3] ^= 1;
        credit();
    }
    wait_flag(&faulted, 3);
    for (unsigned s = 0; s < SLOTS; ++s) wait_flag(&closed[s], 1);
    uint32_t accesses = get(&model.accesses);
    vTaskDelay(5);
    configASSERT(get(&model.accesses) == accesses && atomic_load(&rt.failed) &&
                 rt.service.controller.state == A733_NPU_FAULT &&
                 rt.service.controller.fault == A733_NPU_MEMORY &&
                 get(&model.triggers) == triggers + (npu_case == 4));
    configASSERT(npu_runtime_stop_poll(&rt) == A733_NPU_PLATFORM_RETAINED && !stops);
    platform_puts(npu_case == 3 ? "PASS NPU runtime: a changed descriptor is refused before the trigger; every slot retained\n" :
                                  "PASS NPU runtime: a coefficient changed during the job fails the post-job check; every slot retained\n");
    platform_exit(0);
}
BaseType_t npu_session_task_privileged(TaskFunction_t entry, void *argument)
{ return entry == bootstrap_run || npu_service_task_privileged(entry, argument); }
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }

static void fatal(void *unused) { (void)unused; platform_panic("runtime native adapter", 0); }
static void layout_checks(void)
{
    const struct a733_npu_memory *memory = &rt.native.memory;
    uint64_t end = 0;
    for (size_t i = 0; i < memory->count; ++i) {
        const struct a733_npu_buffer *b = &memory->buffers[i];
        /* Sorted by device address: one unmapped guard page between allocations. */
        configASSERT(!i || b->device >= end + 4096);
        end = (uint64_t)b->device + b->bytes;
        configASSERT(b->device >= NPU_RUNTIME_DEVICE_BASE && b->cpu >= PARENT_BASE &&
                     b->cpu + b->bytes <= PARENT_BASE + rt.parent_bytes);
    }
    configASSERT(memory->count == 1 + 4 * SLOTS && memory->image.bytes == 4096 + 16384);
    /* Read windows and write windows start on 2 MiB boundaries. */
    configASSERT(!((rt.slot[0].read.cpu | rt.slot[0].write.cpu) & (NPU_RUNTIME_GROUP_BYTES - 1)));
    configASSERT(rt.slot[SLOTS - 1].constant.cpu + rt.constant_bytes <= rt.slot[0].read.cpu &&
                 rt.slot[SLOTS - 1].read.cpu + rt.read_bytes <= rt.slot[0].write.cpu);
}
void kernel_main(void)
{
    platform_init();
    configASSERT(fixtures[CONV8].bytes <= sizeof(writable_conv8));
    memcpy(writable_conv8, fixtures[CONV8].model, fixtures[CONV8].bytes);
    struct npu_runtime_config config = {.count = FIXTURES, .slots = SLOTS, .cpu = PARENT_BASE,
        .physical = PARENT_BASE, .register_cpu = REGISTER_CPU, .register_physical = 0x03600000,
        .fatal = fatal};
    for (unsigned m = 0; m < FIXTURES; ++m)
        config.models[m] = (struct npu_runtime_model){m == CONV8 ? writable_conv8 : fixtures[m].model,
                                                       fixtures[m].bytes};
    size_t bytes = npu_runtime_requirements(config.models, FIXTURES, SLOTS);
    configASSERT(bytes && bytes <= PARENT_LIMIT && !(bytes & (NPU_RUNTIME_GROUP_BYTES - 1)));
    configASSERT(!npu_runtime_requirements(config.models, FIXTURES, 0) &&
                 !npu_runtime_requirements(config.models, FIXTURES, NPU_RUNTIME_SLOTS + 1));
    writable_conv8[7] ^= 1;
    configASSERT(!npu_runtime_requirements(config.models, FIXTURES, SLOTS));
    writable_conv8[7] ^= 1;
    config.bytes = bytes - NPU_RUNTIME_GROUP_BYTES;
    struct npu_runtime_config bad = config;
    configASSERT(!npu_runtime_prepare(&rt, &bad) && atomic_load(&rt.failed));
    memset(&rt, 0, sizeof(rt));
    config.bytes = bytes;
    bad = config; bad.cpu += 4096; bad.physical += 4096;
    configASSERT(!npu_runtime_prepare(&rt, &bad) && !atomic_load(&rt.failed));
    configASSERT(npu_runtime_prepare(&rt, &config) && !npu_runtime_prepare(&rt, &config));
    layout_checks();

    model.regs[0x20/4] = 0x9000; model.regs[0x24/4] = 0x9202;
    model.regs[0x30/4] = 0x1000003b; model.regs[0x28/4] = 0x20240101; model.regs[1] = IDLE;
    model.masked = 1;
    model.immediate = npu_case == 1 || npu_case == 3 || npu_case == 4;
    model.late = npu_case == 2;
    model.defer_mask = npu_case == 2 ? 6 : 0;
    struct a733_npu_handoff base = {.controller = 0x03600000, .command_admission = 1};
    configASSERT(npu_runtime_handoff(&rt, &base, &startup.handoff));
    configASSERT(startup.handoff.command_count == SLOTS + 1);
    bootstrap = xTaskCreateStaticAffinitySet(bootstrap_run, "bootstrap", 4096, NULL, 3,
                                             stacks[SLOTS], &tasks[SLOTS], 2);
    startup.initialized = 1; startup.task = bootstrap;
    for (unsigned s = 0; s < SLOTS; ++s) {
        clients[s] = xTaskCreateStaticAffinitySet(client, "client", 4096, (void *)(uintptr_t)s, 5,
                                                  stacks[s], &tasks[s], s & 1 ? 2 : 1);
        configASSERT(clients[s]);
    }
    TaskHandle_t duplicate[SLOTS];
    memcpy(duplicate, clients, sizeof(duplicate));
    duplicate[1] = clients[0];
    configASSERT(!npu_runtime_bind(&rt, &startup, duplicate, 4, 1));
    duplicate[1] = bootstrap;
    configASSERT(!npu_runtime_bind(&rt, &startup, duplicate, 4, 1));
    configASSERT(!npu_runtime_bind_irq(&rt, &irq_ops, &rt, 1, 1));
    configASSERT(npu_runtime_bind(&rt, &startup, clients, 4, 1));
    configASSERT(rt.service.runtime && !npu_runtime_bind(&rt, &startup, clients, 4, 1));
    configASSERT(npu_runtime_bind_irq(&rt, &irq_ops, &rt, 1, 1));
    vTaskStartScheduler(); platform_panic("scheduler returned", 0);
}
