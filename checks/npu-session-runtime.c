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
 *    every slot is retained.
 *  5 (built with NPU_BOARD_CLIENT_CHECK) slot 3 runs the board image's client
 *    loop (apps/npu-board/client.c) and the bootstrap task runs the board
 *    image's USB0 protocol (apps/npu-board/usb.c) over a modeled TinyUSB
 *    vendor class and plays the host. STATUS and LOAD sent before the client
 *    has opened are answered not-ready. After the bootstrap starts the
 *    client, which sends OPEN and publishes ready, the host loads
 *    ShuffleNetV2 and three times uploads the input in 4 KiB requests,
 *    submits, reads the output and returns the window, then closes. A SUBMIT
 *    posted while the read window is lent is refused. The requests, replies
 *    and client tick waits per inference and the copies per tensor byte in
 *    each direction are printed. */
#include "npu_runtime.h"
#include "aarch64_device_memory.h"
#include "platform.h"
#include "npu_session_fixtures.h"
#ifdef NPU_BOARD_CLIENT_CHECK
#include "../apps/npu-board/client.h"
#include "npu-board-usb/npu-board-usb-model.h"
static unsigned thermal_on_trigger,thermal_window_checks;
static int board_time(void *unused,uint64_t *out)
{(void)unused;*out=(uint64_t)xTaskGetTickCount()*1000;return 1;}
#endif
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
#ifdef NPU_BOARD_CLIENT_CHECK
        if(!model.bootstrap&&thermal_on_trigger) {
            uint64_t now;configASSERT(board_time(NULL,&now));thermal_on_trigger=0;
            npu_board_client_thermal(&board_client,0,1,0,now);
        }
#endif
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
int a733_npu_startup_prepared(struct a733_npu_startup *s)
{
    configASSERT(s == &startup);
    return 1;
}
enum a733_npu_platform_result a733_npu_startup_step(struct a733_npu_startup *s)
{
    configASSERT(s == &startup);
    return npu_service_activate(&rt.service) == NPU_SERVICE_OK ? A733_NPU_PLATFORM_READY :
                                                                 A733_NPU_PLATFORM_RETAINED;
}
enum a733_npu_platform_result a733_npu_startup_stop(struct a733_npu_startup *s)
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

#ifdef NPU_BOARD_CLIENT_CHECK
struct npu_board_client board_client;
static unsigned client_tick_waits;
void __real_vTaskDelay(TickType_t);
void __wrap_vTaskDelay(TickType_t ticks)
{
    if (xTaskGetCurrentTaskHandle() == clients[3]) ++client_tick_waits;
    __real_vTaskDelay(ticks);
}
#endif
static void client(void *argument)
{
    unsigned slot = (unsigned)(uintptr_t)argument;
    configASSERT(task_level() == 0);
#ifdef NPU_BOARD_CLIENT_CHECK
    if ((npu_case == 5||npu_case == 6) && slot == 3) { npu_board_client_run(&board_client); configASSERT(0); }
#endif
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
                /* Clean and invalidate of every output byte before the job:
                 * the OS client's fill pattern depends on it. */
                configASSERT(was_flushed((uintptr_t)out.data, out.bytes) &&
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
    case 5:case 6:
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
            set(&parked[slot], 1);
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
#ifdef NPU_BOARD_CLIENT_CHECK
/* The bootstrap runs the board USB protocol over modeled class FIFOs.
 * PIO moves bytes between the host and each class FIFO directly. One OUT
 * and one IN packet move per step; native stack checks cover aggregated
 * transfer completion, short packets and ring wrap. */
#define USB_PACKET 512u
/* The FIFO sizes of apps/npu-board/tusb_config.h, which the image resolves. */
#define RX_FIFO CFG_TUD_VENDOR_RX_BUFSIZE
#define TX_FIFO CFG_TUD_VENDOR_TX_BUFSIZE
_Static_assert(CFG_TUD_EDPT_DEDICATED_HWFIFO == 1 && RX_FIFO == 2048 && TX_FIFO == 4096, "apps/npu-board/tusb_config.h FIFO sizes");
#define HOST_CHUNK 4096u
enum { STATUS = 1, LOAD, UPLOAD, SUBMIT, READ, RETURN, CLOSE };
const uint64_t npu_board_usb_epoch = UINT64_C(0x0123456789abcdef), npu_board_usb_generation = 3;
_Noreturn void npu_board_usb_stop(void) { platform_puts("FAIL usb.c stopped USB0\n"); platform_exit(1); }
static struct {
    uint8_t out[64 + HOST_CHUNK], in[64 + FIXTURE_MAX_TENSOR], chunk[HOST_CHUNK];
    size_t out_len, out_at, in_len;
    uint8_t rx_ff[RX_FIFO], tx_ff[TX_FIFO];
    size_t rx_head, rx_count, tx_head, tx_count;
    int zlp;
    /* Bytes moved per stage; headers counts request and response headers. */
    uint64_t pio_in, fifo_to_caller, into_window, headers_in;
    uint64_t caller_to_fifo, from_window, pio_out, headers_out;
} usb;
static unsigned host_sequence;
static void ring_put(uint8_t *ring, size_t size, size_t head, size_t *count, const uint8_t *from, size_t n)
{
    size_t at = (head + *count) % size, first = n < size - at ? n : size - at;
    memcpy(ring + at, from, first); memcpy(ring, from + first, n - first); *count += n;
}
static void ring_get(const uint8_t *ring, size_t size, size_t *head, size_t *count, uint8_t *to, size_t n)
{
    size_t first = n < size - *head ? n : size - *head;
    memcpy(to, ring + *head, first); memcpy(to + first, ring, n - first);
    *head = (*head + n) % size; *count -= n;
}
static int in_tensor(const struct npu_board_tensor *t, unsigned n, uintptr_t p)
{
    for (unsigned i = 0; i < n; ++i) if (p >= t[i].cpu && p < t[i].cpu + t[i].bytes) return 1;
    return 0;
}
int tud_speed_get(void) { return TUSB_SPEED_HIGH; }
uint32_t tud_vendor_available(void) { return (uint32_t)usb.rx_count; }
uint32_t tud_vendor_read(void *buffer, uint32_t bytes)
{
    size_t n = bytes < usb.rx_count ? bytes : usb.rx_count;
    ring_get(usb.rx_ff, RX_FIFO, &usb.rx_head, &usb.rx_count, buffer, n);
    usb.fifo_to_caller += n;
    if (in_tensor(board_client.input, board_client.inputs, (uintptr_t)buffer)) usb.into_window += n;
    return (uint32_t)n;
}
uint32_t tud_vendor_write(const void *buffer, uint32_t bytes)
{
    size_t room = TX_FIFO - usb.tx_count, n = bytes < room ? bytes : room;
    ring_put(usb.tx_ff, TX_FIFO, usb.tx_head, &usb.tx_count, buffer, n);
    usb.caller_to_fifo += n;
    if (in_tensor(board_client.output, board_client.outputs, (uintptr_t)buffer)) usb.from_window += n;
    return (uint32_t)n;
}
uint32_t tud_vendor_flush(void) { return (uint32_t)usb.tx_count; }
/* The controller and the class's transfer-complete handling: an OUT packet
 * only when the receive FIFO has room for it, and after the transmit FIFO
 * empties on a whole packet, the zero-length packet. */
static int usb_controller(void)
{
    int moved = 0;
    if (usb.out_at < usb.out_len && RX_FIFO - usb.rx_count >= USB_PACKET) {
        size_t n = usb.out_len - usb.out_at < USB_PACKET ? usb.out_len - usb.out_at : USB_PACKET;
        ring_put(usb.rx_ff, RX_FIFO, usb.rx_head, &usb.rx_count, usb.out + usb.out_at, n);
        usb.out_at += n; usb.pio_in += n;
        tud_vendor_rx_cb(0, NULL, 0); moved = 1;
    }
    if (usb.tx_count) {
        if(npu_case==6&&npu_board_client_stopping(&board_client)&&!atomic_load(&board_client.closed)) {
            configASSERT(atomic_load(&board_client.window)==NPU_BOARD_WRITE_LENT);
            ++thermal_window_checks;
        }
        size_t n = usb.tx_count < USB_PACKET ? usb.tx_count : USB_PACKET;
        configASSERT(usb.in_len + n <= sizeof(usb.in));
        ring_get(usb.tx_ff, TX_FIFO, &usb.tx_head, &usb.tx_count, usb.in + usb.in_len, n);
        usb.in_len += n; usb.pio_out += n;
        usb.zlp = !usb.tx_count && n == USB_PACKET;
        tud_vendor_tx_cb(0, (uint32_t)n); moved = 1;
    } else if (usb.zlp) {
        usb.zlp = 0; tud_vendor_tx_cb(0, 0); moved = 1;
    }
    return moved;
}
static void put32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint64_t word64(const uint8_t *p) { return word(p) | (uint64_t)word(p + 4) << 32; }
/* One request and its whole response. Returns the result; usb.in holds the
 * response. While the client works, USB0 makes no progress and the host
 * waits a tick. */
static unsigned host(unsigned op, unsigned argument, unsigned offset, const uint8_t *payload, uint32_t bytes)
{
    configASSERT(usb.out_at == usb.out_len && 64 + bytes <= sizeof(usb.out));
    memset(usb.out, 0, 64);
    put32(usb.out, 0x3151424eu); put32(usb.out + 4, op); put32(usb.out + 8, ++host_sequence);
    put32(usb.out + 12, bytes); put64(usb.out + 16, npu_board_usb_epoch); put64(usb.out + 24, npu_board_usb_generation);
    put32(usb.out + 32, argument); put32(usb.out + 36, offset);
    if (bytes) memcpy(usb.out + 64, payload, bytes);
    usb.out_len = 64 + bytes; usb.out_at = 0; usb.in_len = 0; usb.headers_in += 64;
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        const size_t in = usb.in_len, out = usb.out_at, rx = usb.rx_count, tx = usb.tx_count;
        int moved = usb_controller();
        npu_board_usb_exchange();
        if (usb.in_len >= 64 && usb.in_len == 64 + word(usb.in + 28) && usb.out_at == usb.out_len &&
            !usb.tx_count && !usb.zlp) break;
        if (!moved && in == usb.in_len && out == usb.out_at && rx == usb.rx_count && tx == usb.tx_count) {
            configASSERT(xTaskGetTickCount() - start < 20000); vTaskDelay(1);
        }
    }
    configASSERT(word(usb.in) == 0x3152424eu && word(usb.in + 4) == op && word(usb.in + 8) == host_sequence &&
                 word64(usb.in + 32) == npu_board_usb_epoch && word64(usb.in + 40) == npu_board_usb_generation);
    usb.headers_out += 64;
    return word(usb.in + 12);
}
static void board_usb(void)
{
    struct npu_board_client *c = &board_client;
    /* Before the client has sent OPEN, STATUS and LOAD are answered not-ready
     * with zero client fields, and nothing is posted. */
    configASSERT(host(STATUS, 0, 0, NULL, 0) == NPU_BOARD_RESULT_NOT_READY && !word(usb.in + 16));
    configASSERT(host(LOAD, SHUFFLENET, 0, NULL, 0) == NPU_BOARD_RESULT_NOT_READY && !word(usb.in + 56));
    configASSERT(!c->requests && !atomic_load(&c->request) && atomic_load(&c->window) == NPU_BOARD_HELD);
    npu_board_client_start(c);
    TickType_t start = xTaskGetTickCount();
    while (!npu_board_client_ready(c)) { configASSERT(xTaskGetTickCount() - start < 20000); vTaskDelay(1); }
    set(&opened[3], 1);
    configASSERT(host(STATUS, 0, 0, NULL, 0) == NPU_BOARD_RESULT_OK && c->requests == 1 && c->replies == 1);
    if(npu_case==6) {
        const struct fixture *f=&fixtures[CONV8];
        configASSERT(host(LOAD,CONV8,0,NULL,0)==NPU_BOARD_RESULT_OK);
        configASSERT(host(UPLOAD,0,0,f->tensor[0],f->tensor_bytes[0])==NPU_BOARD_RESULT_OK);
        uint64_t now;configASSERT(board_time(NULL,&now));
        npu_board_client_thermal(c,0,0,now+1000000,now);
        const unsigned before=c->requests;
        configASSERT(host(SUBMIT,0,0,NULL,0)==NPU_BOARD_RESULT_REFUSED&&c->requests==before);
        npu_board_client_thermal(c,1,0,now+1000000,now);thermal_on_trigger=1;
        configASSERT(host(SUBMIT,0,0,NULL,0)==NPU_BOARD_RESULT_OK);
        configASSERT(!thermal_on_trigger&&npu_board_client_stopping(c)&&!(atomic_load(&c->admission)&NPU_BOARD_ACCEPTED));
        configASSERT(thermal_window_checks);
        start=xTaskGetTickCount();
        while(!atomic_load(&c->closed)) {
            npu_board_usb_exchange();configASSERT(xTaskGetTickCount()-start<5000);vTaskDelay(1);
        }
        configASSERT(atomic_load(&c->window)==NPU_BOARD_HELD&&c->requests==before+2&&c->replies==c->requests);
        configASSERT(host(STATUS,0,0,NULL,0)==NPU_BOARD_RESULT_OK&&word(usb.in+16)==A733_NPU_CLOSED);
        configASSERT(host(SUBMIT,0,0,NULL,0)==NPU_BOARD_RESULT_REFUSED);
        configASSERT(!client_tick_waits&&rt.counters.jobs==1);
        set(&closed[3],1);return;
    }
    configASSERT(host(LOAD, SHUFFLENET, 0, NULL, 0) == NPU_BOARD_RESULT_OK &&
                 atomic_load(&c->window) == NPU_BOARD_READ_LENT && word(usb.in + 56) == (1u << 16 | 1u));
    const struct fixture *f = &fixtures[SHUFFLENET];
    configASSERT(c->input[0].bytes == f->tensor_bytes[0] && c->output[0].bytes == f->tensor_bytes[1]);
    const unsigned requests = c->requests, replies = c->replies;
    const uint64_t pio_in = usb.pio_in, into_window = usb.into_window,
        headers_in = usb.headers_in, from_window = usb.from_window,
        pio_out = usb.pio_out, headers_out = usb.headers_out;
    uint64_t upload = 0, output = 0;
    for (unsigned n = 0; n < JOBS; ++n) {
        for (uint32_t at = 0; at < f->tensor_bytes[0]; at += HOST_CHUNK) {
            uint32_t bytes = f->tensor_bytes[0] - at < HOST_CHUNK ? f->tensor_bytes[0] - at : HOST_CHUNK;
            for (uint32_t x = 0; x < bytes; ++x) usb.chunk[x] = (uint8_t)(f->tensor[0][at + x] ^ (n * 0x25));
            configASSERT(host(UPLOAD, 0, at, usb.chunk, bytes) == NPU_BOARD_RESULT_OK);
            upload += bytes;
        }
        configASSERT(host(SUBMIT, 0, 0, NULL, 0) == NPU_BOARD_RESULT_OK &&
                     atomic_load(&c->window) == NPU_BOARD_WRITE_LENT);
        configASSERT(host(READ, 0, 0, NULL, 0) == NPU_BOARD_RESULT_OK && word(usb.in + 28) == c->output[0].bytes);
        const struct npu_runtime_slot *s = &rt.slot[3];
        unsigned i = source_of_output(s, 0);
        for (unsigned x = 0; x < c->output[0].bytes; ++x)
            configASSERT(usb.in[64 + x] ==
                         expected(SHUFFLENET, 0, i == UINT32_MAX ? 0 : ((const uint8_t *)c->input[i].cpu)[x % c->input[i].bytes]));
        output += c->output[0].bytes;
        configASSERT(host(RETURN, 0, 0, NULL, 0) == NPU_BOARD_RESULT_OK);
    }
    const unsigned job_requests = c->requests - requests, job_replies = c->replies - replies;
    configASSERT(job_requests == JOBS && job_replies == JOBS && !client_tick_waits);
    /* Each tensor byte has one class-FIFO/window copy and one PIO move. */
    const uint64_t in_copies = usb.into_window - into_window;
    const uint64_t out_copies = usb.from_window - from_window;
    configASSERT(upload == JOBS * (uint64_t)f->tensor_bytes[0] && output == JOBS * (uint64_t)f->tensor_bytes[1]);
    configASSERT(usb.into_window - into_window == upload && in_copies == upload &&
                 usb.pio_in - pio_in - (usb.headers_in - headers_in) == upload);
    configASSERT(usb.from_window - from_window == output && out_copies == output &&
                 usb.pio_out - pio_out - (usb.headers_out - headers_out) == output);
    /* A SUBMIT posted while the read window is lent is refused before any
     * runtime request. */
    configASSERT(atomic_load(&c->window) == NPU_BOARD_READ_LENT &&
                 npu_board_client_post(c, ++host_sequence, NPU_BOARD_SUBMIT, 0));
    start = xTaskGetTickCount();
    unsigned result;
    while (!(result = npu_board_client_answer(c, host_sequence))) { configASSERT(xTaskGetTickCount() - start < 20000); vTaskDelay(1); }
    configASSERT(result == NPU_BOARD_RESULT_REFUSED && c->refused == 1 && c->requests == requests + JOBS);
    /* The catalogue's largest output: mimus-linear-q's 2,816 bytes, a
     * 2,880-byte READ response, inside the image's transmit FIFO. */
    const struct fixture *g = &fixtures[MIMUS];
    configASSERT(host(LOAD, MIMUS, 0, NULL, 0) == NPU_BOARD_RESULT_OK && atomic_load(&c->window) == NPU_BOARD_READ_LENT &&
                 c->inputs == 1 && c->outputs == 1 && c->input[0].bytes == g->tensor_bytes[0] &&
                 c->output[0].bytes == 2816 && 64 + c->output[0].bytes <= TX_FIFO);
    for (uint32_t at = 0; at < g->tensor_bytes[0]; at += HOST_CHUNK) {
        uint32_t bytes = g->tensor_bytes[0] - at < HOST_CHUNK ? g->tensor_bytes[0] - at : HOST_CHUNK;
        configASSERT(host(UPLOAD, 0, at, g->tensor[0] + at, bytes) == NPU_BOARD_RESULT_OK);
    }
    configASSERT(host(SUBMIT, 0, 0, NULL, 0) == NPU_BOARD_RESULT_OK && atomic_load(&c->window) == NPU_BOARD_WRITE_LENT);
    configASSERT(host(READ, 0, 0, NULL, 0) == NPU_BOARD_RESULT_OK && word(usb.in + 28) == 2816 && usb.in_len == 64 + 2816);
    {
        const struct npu_runtime_slot *s = &rt.slot[3];
        unsigned i = source_of_output(s, 0);
        for (unsigned x = 0; x < 2816; ++x)
            configASSERT(usb.in[64 + x] ==
                         expected(MIMUS, 0, i == UINT32_MAX ? 0 : ((const uint8_t *)c->input[i].cpu)[x % c->input[i].bytes]));
    }
    configASSERT(host(RETURN, 0, 0, NULL, 0) == NPU_BOARD_RESULT_OK);
    configASSERT(host(CLOSE, 0, 0, NULL, 0) == NPU_BOARD_RESULT_OK && c->state == A733_NPU_CLOSED &&
                 atomic_load(&c->closed));
    set(&closed[3], 1);
    platform_puts("COUNT board_client jobs=3");
    platform_puts(" requests="); decimal(job_requests); platform_puts(" replies="); decimal(job_replies);
    platform_puts(" client_tick_waits="); decimal(client_tick_waits);
    platform_puts(" refused_while_lent="); decimal(c->refused);
    platform_puts(" upload_bytes="); decimal(upload); platform_puts(" output_bytes="); decimal(output);
    platform_puts("\n");
    platform_puts("COUNT board_usb input_copies_per_byte="); decimal(in_copies / upload);
    platform_puts(" input_pio_per_byte=1 output_copies_per_byte="); decimal(out_copies / output);
    platform_puts(" output_pio_per_byte=1 header_bytes_in="); decimal(usb.headers_in - headers_in);
    platform_puts(" header_bytes_out="); decimal(usb.headers_out - headers_out);
    platform_puts("\n");
}
#endif
static void bootstrap_run(void *unused)
{
    (void)unused;
    configASSERT(npu_runtime_stop_poll(&rt) == A733_NPU_PLATFORM_RETAINED && !atomic_load(&rt.failed));
    configASSERT(npu_runtime_startup_poll(&rt) == A733_NPU_PLATFORM_READY);
    set(&started, 1);
#ifdef NPU_BOARD_CLIENT_CHECK
    if (npu_case == 5||npu_case == 6) {
        board_usb();
        finish_case(npu_case==6?"PASS NPU runtime: thermal refusal, accepted-job drain, USB window handback and automatic CLOSE with no client tick wait\n":
            "PASS NPU runtime: the board client loop hands the windows over, one request and reply per inference, no tick wait\n");
    }
#endif
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
    /* Cases 3 and 4: integrity failures retain every slot. Slot 1's LOAD
     * completes before the change, which would otherwise fault it under
     * host load. */
    wait_flag(&faulted, 1);
    wait_flag(&parked[1], 1);
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
    model.immediate = npu_case == 1 || npu_case == 3 || npu_case == 4 || npu_case == 5 || npu_case == 6;
    model.late = npu_case == 2;
    model.defer_mask = npu_case == 2 ? 6 : 0;
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
#ifdef NPU_BOARD_CLIENT_CHECK
    board_client = (struct npu_board_client){.rt = &rt, .slot = 3, .job_timeout_us = 1000000,
        .wait = 20000, .task = clients[3], .bootstrap = bootstrap,.now=board_time,.session_end=60000000};
    atomic_init(&board_client.admission,(UINT64_C(60000000)<<3)|NPU_BOARD_ALLOW);
#endif
    vTaskStartScheduler(); platform_panic("scheduler returned", 0);
}
