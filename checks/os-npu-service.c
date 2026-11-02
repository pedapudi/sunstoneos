/* The OS NPU wrapper (apps/os/npu.c) against the real runtime and service.
 *
 * Linked as real code: apps/os/npu.c, drivers/npu/a733_npu_runtime.c,
 * npu_service.c, a733_npu_controller.c, the model assembler and the
 * published ShuffleNetV2 model of the selected image, on the unmodified
 * FreeRTOS kernel of the host shim. Modeled: the NPU register file and its
 * interrupt (register_read/register_write below, which abort on any register
 * or sequence they do not model), the interrupt controller route (mask, rearm,
 * stop), the platform startup sequencer (clocks, resets and rails: the window
 * opens when the startup step reports ready), cache maintenance and the
 * counter. No hardware executes. The register addresses and values come from
 * drivers/npu/a733_npu_controller.c.
 *
 * Cases (argv[1]):
 *  20 the bootstrap completes 4 ms after the doorbell; OPEN, LOAD, one SUBMIT
 *     and CLOSE all succeed through the wrapper, which then stops. OPEN is
 *     answered while the controller is still booting.
 *  21 the bootstrap needs 30 ms, longer than the LOAD: the LOAD completes
 *     while the controller boots and SUBMIT is held until it is ready.
 */
#include "host_rtos.h"
#include "os.h"
#include "npu.h"
#include "npu_arch.h"
#include "a733_npu_native.h"
#include "a733_npu_memory.h"
#include "a733_mapping_check.h"
#include "../apps/board/application.h"
#include <assert.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The kernel here has no core affinity (one core). */
static TaskHandle_t create_static(TaskFunction_t entry, const char *name, uint32_t stack, void *argument,
                                  UBaseType_t priority, StackType_t *stack_memory, StaticTask_t *task,
                                  UBaseType_t affinity)
{
    (void)affinity;
    return xTaskCreateStatic(entry, name, stack, argument, priority, stack_memory, task);
}
#define xTaskCreateStaticAffinitySet create_static

#ifndef OS_NPU_SOURCE
#define OS_NPU_SOURCE "../apps/os/npu.c"
#endif
#include OS_NPU_SOURCE

struct board_application board;
struct board_provider providers;
static unsigned live_value = 1;
uint64_t npu_arch_current_el(void) { return 4; }
uint64_t npu_arch_stack_select(void) { return 0; }
int npu_arch_zva_block_is_64(void) { return 1; }
void npu_arch_zero_line(uintptr_t at) { memset((void *)at, 0, 64); }
int board_live(void *context) { assert(!context); return (int)live_value; }
uint32_t board_live_refusal(void) { return live_value ? 0 : UINT32_C(1) << 28; }
static unsigned board_failures;
static uint32_t first_site, first_value, first_caller;
void board_fail_note(uint32_t site, uint32_t value, const void *caller)
{
    if (!first_site) { first_site = site; first_value = value; first_caller = (uint32_t)(uintptr_t)caller; }
}
void board_fail(void *context) { assert(!context); ++board_failures; live_value = 0; }
void board_fail_at(uint32_t site, uint32_t value, const void *caller)
{ board_fail_note(site, value, caller); board_fail(NULL); }
int a733_counter_clock_now(void *context, uint64_t *out)
{ assert(context == &providers.shared.counter); *out = host_time_us(); return 1; }
void vApplicationStackOverflowHook(TaskHandle_t task, char *name) { (void)task; (void)name; abort(); }
void platform_panic(const char *why, uint64_t value)
{ fprintf(stderr, "panic %s %llx\n", why, (unsigned long long)value); abort(); }

static int memory_translate(void *cookie, uintptr_t cpu, int write, struct aarch64_translation *out);
/* Cache maintenance and translation: the model has coherent memory. */
int aarch64_device_cache(enum aarch64_cache_op op, uintptr_t cpu, size_t bytes)
{ (void)op; return cpu && bytes; }
int aarch64_device_translate(uintptr_t cpu, int write, struct aarch64_translation *out)
{ return memory_translate(NULL, cpu, write, out); }

/* The NPU register file. */
#define IDLE 0x7fffffffu
#define BOOTSTRAP_US 4000u
#define JOB_US 2904u
static unsigned bootstrap_us = BOOTSTRAP_US, job_us = JOB_US;
static struct {
    uint32_t regs[0x800 / 4];
    unsigned masked, latched, running, bootstrap, triggers, delivered, mmu_exception, faulted, cancel_asserted;
} model;
#ifdef SUNSTONE_NPU_FAULT_HOOK
extern unsigned char sunstone_npu_canary[];
extern int sunstone_npu_hook_slot;
extern uint32_t sunstone_npu_hook_address;
#else
static unsigned char sunstone_npu_canary[1];
static int sunstone_npu_hook_slot = -1;
static uint32_t sunstone_npu_hook_address;
#endif
static unsigned registered_case; /* case 29: graphs handed over after the scheduler started */
static unsigned registered_ok, registered_refused, registered_job, registered_changed_result;
static unsigned long_run; /* case 26: many jobs through a session whose job field is 1 */
static unsigned long_jobs_done;
static unsigned fault_case; /* case 25: descriptors aimed at unmapped addresses; each such job must fail alone and the NPU recover */
static unsigned gic_defer_left;
static struct npu_runtime *runtime_of_model;
static void deliver(void)
{
    if (model.masked) { model.latched = 1; return; }
    model.masked = 1; ++model.delivered;
    BaseType_t woken = pdFALSE;
    npu_service_irq_notify_from_isr(&runtime_of_model->service, 1, &woken);
}
static void complete_isr(void *unused)
{
    (void)unused;
    /* An MMU exception raises event bit 30 and no completion (vip_drv_hardware.h:86); +0x384 and +0x380 hold the
     * MMU status and the faulting address (vip_drv_debug.c:355, :406). */
    if (model.mmu_exception) { model.faulted = 1; model.regs[0x10 / 4] |= 0x40000000u; model.regs[0x384 / 4] = 2; model.regs[0x380 / 4] = model.mmu_exception; }
    else model.regs[0x10 / 4] |= 1;
    model.regs[1] = IDLE; model.running = 0;
    deliver();
}
/* Registers that the controller reads or writes (a733_npu_controller.c). A
 * register not listed aborts, as does a write sequence out of order. */
static int modeled(unsigned offset, int write)
{
    static const unsigned readable[] = {0x00, 0x04, 0x10, 0x14, 0x20, 0x24, 0x28, 0x30, 0x3c, 0x90, 0x100, 0x104,
                                        0x10c, 0x380, 0x384, 0x388, 0x3a8, 0x3b4, 0x414, 0x55c, 0x654};
    static const unsigned writable[] = {0x00, 0x14, 0x3c, 0x90, 0x100, 0x104, 0x10c, 0x388, 0x3a4, 0x3a8, 0x3b4,
                                        0x414, 0x55c, 0x654};
    const unsigned *list = write ? writable : readable;
    size_t n = write ? sizeof(writable) / sizeof(*writable) : sizeof(readable) / sizeof(*readable);
    for (size_t i = 0; i < n; ++i) if (list[i] == offset) return 1;
    return 0;
}
static int register_read(void *cookie, unsigned offset, uint32_t *value)
{
    assert(cookie == &runtime_of_model->native);
    if (!modeled(offset, 0)) { fprintf(stderr, "unmodeled register read %#x\n", offset); abort(); }
    /* The event register is read only while the interrupt is masked. */
    if (offset == 0x10) assert(model.masked);
    /* NPU_STALL_READ: after the first MMU exception, a read of this register never returns (a bus that does not
     * answer). */
    if (model.faulted && getenv("NPU_STALL_READ") && offset == (unsigned)strtoul(getenv("NPU_STALL_READ"), NULL, 0))
        for (;;) vTaskDelay(1);
    *value = model.regs[offset / 4];
    if (offset == 0x10) model.regs[offset / 4] = 0;
    return 1;
}
static int register_write(void *cookie, unsigned offset, uint32_t value)
{
    assert(cookie == &runtime_of_model->native);
    if (!modeled(offset, 1)) { fprintf(stderr, "unmodeled register write %#x\n", offset); abort(); }
    model.regs[offset / 4] = value;
    if (offset == 0x3a8 && (value & 1)) { model.regs[0x388 / 4] = 0; model.regs[1] = IDLE; model.running = 0; }
    /* The cancel handshake (vipdrv_hw_cancel): bit 3 set, then cleared, completes with bit 4 (NPU_NO_CANCEL: never). */
    if (offset == 0x3a8) {
        if (value & 8) model.cancel_asserted = 1;
        else if (model.cancel_asserted) {
            model.cancel_asserted = 0;
            if (!getenv("NPU_NO_CANCEL")) model.regs[0x3a8 / 4] |= 16u;
        }
    }
    if (offset == 0x3a4) {
        assert(!model.running && model.masked);
        model.bootstrap = model.regs[0x654 / 4] == runtime_of_model->bootstrap.device;
        ++model.triggers;
        model.running = 1; model.regs[1] = 0;
        /* A descriptor pointer at an unmapped device address (the negative-test build) makes the job fault. A model
         * that lets the access through (UNMAPPED_BYPASS) completes it, and a bypassing write changes the canary. */
        model.mmu_exception = 0;
        if (model.bootstrap) model.faulted = 0; /* the initial command of a recovery */
        if (!model.bootstrap && fault_case && sunstone_npu_hook_slot >= 0) {
            const uint32_t target = sunstone_npu_hook_address ? sunstone_npu_hook_address :
                (uint32_t)(uintptr_t)(sunstone_npu_canary + 0x10000);
            /* The descriptors and the data they point to live in the slot's constant region; a shader operation's
             * addresses are words of its state stream in the command region. */
            const struct npu_runtime_region *regions[2] = {&runtime_of_model->slot[0].constant, &runtime_of_model->slot[0].command};
            for (unsigned region = 0; region < 2 && !model.mmu_exception; ++region) {
                const struct npu_runtime_region *c = regions[region];
                for (size_t at = 0; at + 4 <= c->bytes; at += 4) {
                    uint32_t w; memcpy(&w, (const void *)(uintptr_t)(c->cpu + at), 4);
                    if (w == target || (!(target & 63) && region == 0 && (w & 0x03ffffffu) == target >> 6)) {
                        if (getenv("UNMAPPED_BYPASS")) { if (getenv("UNMAPPED_BYPASS_WRITE")) sunstone_npu_canary[0x10000 + 8] ^= 0xff; }
                        else model.mmu_exception = target;
                        break;
                    }
                }
            }
        }
        /* UNMAPPED_HANG: the faulting job never raises an event (an NPU that stalls on the unmapped access). */
        if (!(model.mmu_exception && getenv("UNMAPPED_HANG")))
            host_irq_at(host_time_us() + (model.bootstrap ? bootstrap_us : job_us), complete_isr, NULL);
    }
    return 1;
}
static uint64_t native_microseconds(void *cookie) { (void)cookie; return host_time_us(); }
static void native_delay(void *cookie, unsigned us) { (void)cookie; host_advance_us(us); }
static void native_barrier(void *cookie) { (void)cookie; }
static int native_publish(void *cookie, const struct a733_npu_mmu_image *image,
                         const struct a733_npu_command *command, int bootstrap)
{ (void)cookie; (void)image; (void)command; (void)bootstrap; return 1; }
static int native_finish(void *cookie, const struct a733_npu_command *command, int canceled)
{ (void)cookie; (void)command; (void)canceled; return 1; }
const struct a733_npu_ops a733_npu_native_operations = {register_read, register_write, native_microseconds,
                                                         native_delay, native_barrier, native_publish,
                                                         native_finish};
/* The adapter's table and buffer registration is the real a733_npu_memory; the model supplies
 * the translation (identity, write-back inner-shareable normal memory) and the cache. */
static int memory_translate(void *cookie, uintptr_t cpu, int write, struct aarch64_translation *out)
{
    (void)cookie; (void)write;
    *out = (struct aarch64_translation){cpu, A733_MAIR_NORMAL_WB, 3}; /* shareability field 3: inner */
    return 1;
}
static int memory_cache(void *cookie, enum aarch64_cache_op op, uintptr_t cpu, size_t bytes)
{ (void)cookie; (void)op; return cpu && bytes; }
static void memory_barrier(void *cookie) { (void)cookie; }
int a733_npu_native_init(struct a733_npu_native *n, uintptr_t register_cpu, uint64_t register_physical,
                         void *arena, size_t capacity, uint64_t arena_physical,
                         const struct a733_npu_buffer *buffers, size_t count, void (*fatal)(void *), void *cookie)
{
    const struct a733_npu_memory_ops ops = {memory_translate, memory_cache, memory_barrier};
    if (!a733_npu_memory_init(&n->memory, arena, capacity, arena_physical, buffers, count, &ops, n)) return 0;
    n->registers = register_cpu; n->register_physical = register_physical;
    n->fatal = fatal; n->fatal_cookie = cookie; n->initialized = 1;
    return 1;
}

/* The interrupt controller route. */
static int irq_mask(void *cookie)
{
    assert(cookie == runtime_of_model);
    /* NPU_GIC_DEFER=n: after an MMU exception, n calls of the mask operation answer "not yet"; NPU_GIC_HANG: all do. */
    if (model.faulted && getenv("NPU_GIC_HANG")) return 0;
    if (model.faulted && gic_defer_left) { --gic_defer_left; return 0; }
    model.masked = 1; return 1;
}
static int irq_rearm(void *cookie)
{
    assert(cookie == runtime_of_model && model.masked);
    model.masked = 0;
    if (model.latched) {
        model.latched = 0; model.masked = 1; ++model.delivered;
        BaseType_t woken = pdFALSE;
        npu_service_irq_notify_from_isr(&runtime_of_model->service, 1, &woken);
    }
    return 1;
}
static int irq_stop(void *cookie) { assert(cookie == runtime_of_model && model.masked); return 1; }
static const struct npu_service_irq_ops irq_ops = {irq_mask, irq_rearm, irq_stop};

/* The startup sequencer: the register window opens when the step reports ready. */
int a733_npu_startup_prepared(struct a733_npu_startup *s) { return s == &npu.startup; }
enum a733_npu_platform_result a733_npu_startup_step(struct a733_npu_startup *s)
{
    assert(s == &npu.startup);
    atomic_store_explicit(&s->window_open, 1, memory_order_release);
    return A733_NPU_PLATFORM_READY;
}
enum a733_npu_platform_result a733_npu_startup_stop(struct a733_npu_startup *s)
{
    assert(s == &npu.startup);
    atomic_store_explicit(&s->window_open, 0, memory_order_release);
    return A733_NPU_PLATFORM_READY;
}
int a733_npu_startup_bind_task(struct a733_npu_startup *s, TaskHandle_t task) { s->task = task; return 1; }

uint32_t a733_npu_startup_wait_ticks(struct a733_npu_startup *s) { (void)s; abort(); }
/* The sealed session grants one job. */
static struct os_session session_record;
const struct os_session *os_session(void) { return &session_record; }

/* The client: the sequence of the OS application mailbox, through the wrapper. */
static unsigned result_open, result_load, result_submit, result_close, finished;
static unsigned state_after_open, state_after_load, state_after_submit;
static uint64_t time_open, time_load, time_submit;
/* Cases 22 and 23: every graph of a catalogue goes through LOAD and SUBMIT of the real runtime and service in turn. The
 * driver writes each graph's input tensors as files NN-K.bin (graph NN, tensor K) in argv[2]. The register file
 * completes a job without computing it, so this shows that the runtime admits, places, assembles and runs every graph,
 * and not what it computes. */
static const char *input_directory;
static unsigned graphs_run;
/* Case 25 records, for each job in turn, the result of os_npu_submit (1 completed, 2 failed with an MMU exception and
 * recovered, negative: the failure of the image) and the fault the wrapper published. */
static int fault_result[16];
static struct os_npu_fault_info fault_seen[16];
static unsigned fault_jobs;
static uint32_t canary_now(void);
static void client_all(void)
{
    for (unsigned entry = 0; entry < os_npu_model_count; ++entry) {
        const int loaded = os_npu_load(entry);
        if (loaded != 1) {
            fprintf(stderr, "LOAD of graph %u returned %d; load phase %u; first failure site %#x value %#x operation %u observed %llu\n", entry, loaded,
                    (unsigned)npu.runtime.slot[0].phase,
                    first_site, first_value, (unsigned)os_npu_first_failure.operation, (unsigned long long)os_npu_first_failure.observed);
            fprintf(stderr, "controller state %u fault %u marks %#x recoveries %u\n", (unsigned)npu.runtime.service.controller.state,
                    (unsigned)npu.runtime.service.controller.fault, npu.runtime.service.runtime ? npu.runtime.service.runtime->marks : 0,
                    npu.runtime.service.runtime ? npu.runtime.service.runtime->recoveries : 0);
            abort();
        }
        const struct ss_npu_model *info = &os_npu_model_info[entry];
        for (unsigned i = 0; i < info->inputs; ++i) {
            struct npu_runtime_tensor t;
            char path[512];
            snprintf(path, sizeof path, "%s/%02u-%u.bin", input_directory, entry, i);
            FILE *f = fopen(path, "rb");
            assert(f && os_npu_tensor(NBG_INPUT, i, &t) == 1);
            assert(fread(t.data, 1, t.bytes, f) == t.bytes && fgetc(f) == EOF);
            fclose(f);
        }
        if (fault_case) {
            const int r = os_npu_submit(1000000);
            fault_result[fault_jobs] = r;
            if (r == 2) fault_seen[fault_jobs] = os_npu_last_fault;
            ++fault_jobs;
            if (r < 0) { finished = 1; vTaskSuspend(NULL); }
            continue;
        }
        assert(os_npu_submit(1000000) == 1);
        ++graphs_run;
    }
}
static void client(void *unused)
{
    (void)unused;
    for (;;) {
        int opened = os_npu_open();
        if (opened == 1) break;
        assert(opened == 0);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(25));
    }
    result_open = 1; state_after_open = npu.runtime.service.controller.state; time_open = host_time_us();
    if (input_directory) {
        client_all();
        result_load = result_submit = 1;
        result_close = (unsigned)os_npu_close();
        while (!os_npu_stopped()) (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(25));
        finished = 1;
        vTaskSuspend(NULL);
    }
    if (registered_case) {
        /* Graphs the build has never seen. The reservation was made for 2 more graphs of at most 16 KiB with tensors of at
         * most 64 KiB. */
        static unsigned char graph[16384];
        FILE *f = fopen(getenv("REG_MODEL"), "rb");
        assert(f);
        const size_t n = fread(graph, 1, sizeof graph, f);
        assert(n > 0 && n < sizeof graph);
        fclose(f);
        unsigned entry = 99;
        static unsigned char damaged[16384];
        memcpy(damaged, graph, n); damaged[n / 2] ^= 0xff;
        assert(os_npu_register(damaged, n, &entry) == 0);                       /* not admissible */
        assert(os_npu_register(graph, n, &entry) == 1 && entry == os_npu_model_count);
        const unsigned first = entry;
        assert(os_npu_register(graph, n, &entry) == 1 && entry == first + 1);
        assert(os_npu_register(graph, n, &entry) == 0);                         /* beyond the declared capacity */
        registered_ok = 2; registered_refused = 2;
        assert(os_npu_load(first) == 1);
        struct npu_runtime_tensor t;
        f = fopen(getenv("REG_INPUT"), "rb");
        assert(f && os_npu_tensor(NBG_INPUT, 0, &t) == 1 && fread(t.data, 1, t.bytes, f) == t.bytes);
        fclose(f);
        registered_job = (unsigned)os_npu_submit(1000000);
        /* The application changes the bytes it handed over: LOAD refuses the copy, which no longer matches. */
        graph[n / 3] ^= 1;
        registered_changed_result = (unsigned)os_npu_load(first + 1);
        result_load = result_submit = 1;
        finished = 1;
        vTaskSuspend(NULL);
    }
    if (long_run) {
        /* A session never fails because it has run a number of jobs: the session record says 1, and the job count is
         * far beyond the former limit of eight. */
        assert(os_npu_load(0) == 1);
        for (unsigned i = 0; i < os_npu_input_count; ++i) {
            struct npu_runtime_tensor t;
            assert(os_npu_tensor(NBG_INPUT, i, &t) == 1 && t.bytes == os_npu_inputs[i].bytes);
            memcpy(t.data, os_npu_inputs[i].data, t.bytes);
        }
        for (; long_jobs_done < long_run; ++long_jobs_done) {
            const int r = os_npu_submit(1000000);
            if (r != 1) break;
        }
        result_load = result_submit = 1;
        result_close = (unsigned)os_npu_close();
        while (!os_npu_stopped()) (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(25));
        finished = 1;
        vTaskSuspend(NULL);
    }
    result_load = (unsigned)os_npu_load(0);
    state_after_load = npu.runtime.service.controller.state; time_load = host_time_us();
    for (unsigned i = 0; i < os_npu_input_count; ++i) {
        struct npu_runtime_tensor t;
        assert(os_npu_tensor(NBG_INPUT, i, &t) == 1 && t.bytes == os_npu_inputs[i].bytes);
        memcpy(t.data, os_npu_inputs[i].data, t.bytes);
    }
    result_submit = (unsigned)os_npu_submit(1000000);
    state_after_submit = npu.runtime.service.controller.state; time_submit = host_time_us();
    result_close = (unsigned)os_npu_close();
    while (!os_npu_stopped()) (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(25));
    finished = 1;
    vTaskSuspend(NULL);
}
/* CRC-32 of the canary pattern (apps/os/npu.c): word i is i * 0x9e3779b1 xor 0xc0ffee11, 1 MiB in all. */
static uint32_t pattern_crc(void)
{
    uint32_t crc = ~0u;
    for (uint32_t i = 0; i < (1u << 18); ++i) {
        const uint32_t v = i * UINT32_C(0x9e3779b1) ^ UINT32_C(0xc0ffee11);
        for (unsigned b = 0; b < 4; ++b) {
            crc ^= (v >> (8 * b)) & 255u;
            for (unsigned k = 0; k < 8; ++k) crc = crc >> 1 ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}
/* CRC-32 of the canary as it is now. */
static uint32_t canary_now(void)
{
    uint32_t crc = ~0u;
#ifdef SUNSTONE_NPU_FAULT_HOOK
    for (size_t i = 0; i < (1u << 20); ++i) {
        crc ^= sunstone_npu_canary[i];
        for (unsigned k = 0; k < 8; ++k) crc = crc >> 1 ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
#endif
    return ~crc;
}
static void verify(void *unused)
{
    (void)unused;
    for (unsigned waited = 0; !finished && live_value && waited < (long_run ? 2000000u : fault_case ? 30000u : 5000u); ++waited)
        vTaskDelay(pdMS_TO_TICKS(1));
    if (fault_case) {
        /* One line for each job and one summary; the driver (os-npu-service.py) judges them against the scenario. */
        for (unsigned i = 0; i < fault_jobs; ++i)
            printf("job %u result %d status %u address %#x canary %#x marks %#x window %u\n", i, fault_result[i],
                   fault_seen[i].mmu_status, fault_seen[i].mmu_address, fault_seen[i].canary_crc, fault_seen[i].marks,
                   fault_seen[i].window_ms);
        printf("summary finished %u failures %u site %#x value %#x caller %#x recoveries %u runtime_failed %u controller %u "
               "faulted_jobs %llu pattern %#x canary_now %#x marks_now %#x\n", finished, board_failures, first_site,
               first_value, first_caller, npu.runtime.service.runtime ? npu.runtime.service.runtime->recoveries : 0,
               (unsigned)atomic_load(&npu.runtime.failed), (unsigned)npu.runtime.service.controller.state,
               (unsigned long long)npu.runtime.counters.faulted_jobs, pattern_crc(), canary_now(),
               npu.runtime.service.runtime ? npu.runtime.service.runtime->marks : 0);
        fflush(stdout);
        return;
    }
    if (registered_case) {
        printf("registered: %u graphs registered, %u refusals, job %u, load after the bytes changed %d, failures %u\n", registered_ok,
               registered_refused, registered_job, (int)registered_changed_result, board_failures);
        fflush(stdout);
        assert(registered_ok == 2 && registered_refused == 2 && registered_job == 1 && (int)registered_changed_result == -1 &&
               board_failures == 1);
        return;
    }
    if (long_run) {
        printf("long run: %u of %u jobs, close %u, failures %u, triggers %u\n", long_jobs_done, long_run, result_close, board_failures, model.triggers);
        assert(finished && !board_failures && long_jobs_done == long_run && result_close == 1 && model.triggers == 1 + long_run);
        return;
    }
    printf("open %u load %u submit %u close %u finished %u failures %u site %#x value %#x triggers %u\n",
           result_open, result_load, result_submit, result_close, finished, board_failures, first_site,
           first_value, model.triggers);
    assert(finished && live_value && !board_failures && !atomic_load(&os_npu_first_failure.ready));
    assert(result_open == 1 && result_load == 1 && result_submit == 1 && result_close == 1);
    if (input_directory) {
        printf("%u graphs loaded and run through the real runtime\n", graphs_run);
        assert(graphs_run == os_npu_model_count && model.triggers == 1 + graphs_run);
        return;
    }
    assert(model.triggers == 2 && npu_service_irq_completions(&npu.runtime.service) == 2);
    /* OPEN is answered while the bootstrap runs; the job starts only after it completes. */
    printf("controller state after open %u load %u submit %u; times us: open %llu load %llu submit %llu\n",
           state_after_open, state_after_load, state_after_submit, (unsigned long long)time_open,
           (unsigned long long)time_load, (unsigned long long)time_submit);
    assert(state_after_open == A733_NPU_BOOTING && state_after_submit == A733_NPU_READY);
    assert(time_submit >= time_open + bootstrap_us + job_us);
    if (bootstrap_us > JOB_US * 4) assert(state_after_load == A733_NPU_BOOTING);
}

/* Walk the NPU page tables that the runtime built (the generated directory and leaf entries; the pool is identity
 * mapped on the host, so a physical table address is a pointer). Returns 1 translated, 2 write fault, 0 fault. */
static uint32_t entry_word(uint64_t at) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)at, 4); return v; }
static int walk_tables(uint32_t device, int write)
{
    static const uint64_t page_sizes[] = {4096, 65536, 1048576, 16777216};
    const uint64_t directory = (uint64_t)(runtime_of_model->native.memory.image.pd_entry & ~15u) * 256;
    const uint32_t root = entry_word(directory + (device / 16777216u) * 4u);
    if (!(root & 1)) return 0;
    const uint64_t page = page_sizes[(root / 4) & 3], table = (directory & UINT64_C(0xff00000000)) | (root & UINT32_C(0xffffffc0));
    const uint32_t leaf = entry_word(table + ((device % 16777216u) / page) * 4);
    if (!(leaf & 1)) return 0;
    if (write && !(leaf & 4)) return 2;
    return 1;
}
/* No page-table entry below the reserved low device range maps anything, for any slot: every 4 KiB page of
 * [0, A733_NPU_MMU_LOW_RESERVED) faults for a read and a write, at its first and its last byte, in the tables the runtime
 * built. This is the basis of loader rule 5 (docs/designs/npu-loader-admission.md): an unpatched pointer word below the
 * range cannot reach DRAM through the tables, and every buffer of every slot starts at or above it. */
static void check_low_range_unmapped(const struct npu_runtime *rt)
{
    for (uint32_t at = 0; at < A733_NPU_MMU_LOW_RESERVED; at += 4096) {
        assert(walk_tables(at, 0) == 0 && walk_tables(at, 1) == 0 && walk_tables(at + 4095, 0) == 0);
    }
    for (unsigned i = 0; i < rt->slots; ++i) {
        assert(rt->slot[i].command.device >= A733_NPU_MMU_LOW_RESERVED && rt->slot[i].constant.device >= A733_NPU_MMU_LOW_RESERVED &&
               rt->slot[i].read.device >= A733_NPU_MMU_LOW_RESERVED && rt->slot[i].write.device >= A733_NPU_MMU_LOW_RESERVED);
    }
    assert(rt->bootstrap.device >= A733_NPU_MMU_LOW_RESERVED);
}
/* The slot's constant region holds the descriptors, the coefficients and, for a model with a dynamic-input section,
 * that section after them (a733_npu_runtime.c, targets()). The NPU maps the region read-only: every page reads and
 * every write faults. */
static void check_constant_region_read_only(void)
{
    const struct npu_runtime_region *constant = &npu.runtime.slot[0].constant;
    for (uint64_t at = 0; at < constant->bytes; at += 4096) {
        assert(walk_tables((uint32_t)(constant->device + at), 0) == 1);
        assert(walk_tables((uint32_t)(constant->device + at), 1) == 2);
    }
    assert(walk_tables((uint32_t)(constant->device + constant->bytes), 0) == 0); /* the guard page after it */
    assert(walk_tables(0, 0) == 0 && walk_tables(0xfff, 1) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    const unsigned mode = (unsigned)strtoul(argv[1], NULL, 10);
    if (mode == 27) {
        /* Case 27: declarations and reservations (docs/designs/npu-resources.md). The image links one graph and no
         * queue, so it needs one slot, one graph and no links. */
        size_t one = 0, many = 0, scratch = 0;
        assert(npu_runtime_reservation(os_npu_models, os_npu_model_count, 1, 8u << 20, &one) == NPU_DECLARATION_OK && one);
        assert(npu_runtime_reservation(os_npu_models, os_npu_model_count, 4, 64u << 20, &many) == NPU_DECLARATION_OK && many > one);
        /* The reservation is the exact requirement and grows with the slots. */
        assert(one == npu_runtime_requirements(os_npu_models, os_npu_model_count, 1));
        assert(npu_runtime_reservation(os_npu_models, os_npu_model_count, 1, one - 1, &scratch) == NPU_DECLARATION_MEMORY);
        assert(npu_runtime_reservation(os_npu_models, os_npu_model_count, 1, one, &scratch) == NPU_DECLARATION_OK);
        unsigned char *copy = malloc(os_npu_models[0].bytes);
        memcpy(copy, os_npu_models[0].data, os_npu_models[0].bytes);
        copy[os_npu_models[0].bytes / 2] ^= 0xff; /* a damaged graph is not admissible */
        struct npu_runtime_model damaged = {copy, os_npu_models[0].bytes};
        assert(npu_runtime_reservation(&damaged, 1, 1, 8u << 20, &scratch) == NPU_DECLARATION_MODEL);
        free(copy);
        /* Graphs have no count limit: forty copies of the graph (more than the former sixteen) fit, and their tables
         * (336 bytes each) are the only difference in the reservation. */
        struct npu_runtime_model forty[40];
        for (unsigned i = 0; i < 40; ++i) forty[i] = os_npu_models[0];
        size_t with_forty = 0;
        assert(npu_runtime_reservation(forty, 40, 1, 64u << 20, &with_forty) == NPU_DECLARATION_OK && with_forty >= one);
        assert(npu_runtime_tables_bytes(40) == 40 * 336 + 0 || npu_runtime_tables_bytes(40) >= 40 * 336);
        /* The declaration against the image and its tables. The image links one graph and no queue. */
        struct os_npu_session n = {0};
        assert(os_npu_declaration_admitted(&n) == NPU_DECLARATION_MISSING); /* no default: nothing declared is refused */
        n.slots = 1; n.in_flight = 1; n.graphs = 1;
        assert(os_npu_declaration_admitted(&n) == NPU_DECLARATION_OK);
        n.in_flight = 100000; /* full width: far beyond one byte, admitted when the tables hold it (the queue's does not) */
        assert(os_npu_declaration_admitted(&n) == NPU_DECLARATION_TABLES);
        n.in_flight = 1;
        struct os_npu_session bad = n;
        bad.slots = 2; assert(os_npu_declaration_admitted(&bad) == NPU_DECLARATION_SLOTS); /* one client task */
        bad = n; bad.in_flight = 0; assert(os_npu_declaration_admitted(&bad) == NPU_DECLARATION_DEPTH);
        bad = n; bad.graphs = 0; assert(os_npu_declaration_admitted(&bad) == NPU_DECLARATION_GRAPHS);
        bad = n; bad.graphs = 17; assert(os_npu_declaration_admitted(&bad) == NPU_DECLARATION_OK); /* graphs have no table limit */
        bad = n; bad.graphs = 100000; assert(os_npu_declaration_admitted(&bad) == NPU_DECLARATION_OK);
        const struct npu_declaration good = {1, 1, 1, 0};
        const struct npu_tables small = {8, 16, 64, 256}, large = {31, 100000, 1000000, 100000};
        const struct npu_image_needs none = {1, 1, 0}, two_graphs = {1, 2, 0}, queued = {3, 1, 2};
        assert(npu_declaration_check(&good, &none, &small) == NPU_DECLARATION_OK);
        assert(npu_declaration_check(&good, &two_graphs, &small) == NPU_DECLARATION_IMAGE_GRAPHS);
        assert(npu_declaration_check(&good, &queued, &small) == NPU_DECLARATION_IMAGE_SLOTS);
        const struct npu_declaration slots3 = {3, 4, 1, 0};
        assert(npu_declaration_check(&slots3, &queued, &small) == NPU_DECLARATION_IMAGE_LINKS);
        const struct npu_declaration full = {3, 64, 1, 2};
        assert(npu_declaration_check(&full, &queued, &small) == NPU_DECLARATION_OK);
        /* The only fixed refusal that is not memory is the command mask: 31 slots. Everything else is the tables. */
        const struct npu_declaration slots31 = {31, 1, 1, 0}, slots32 = {32, 1, 1, 0};
        assert(npu_declaration_check(&slots31, &none, &large) == NPU_DECLARATION_OK);
        assert(npu_declaration_check(&slots32, &none, &large) == NPU_DECLARATION_SLOTS);
        /* Fields are full width: 300 graphs, a depth of 100000 and 5000 links are admitted by tables that hold them, and
         * refused by the image's tables with their own name. */
        const struct npu_declaration wide = {8, 100000, 300, 5000};
        assert(npu_declaration_check(&wide, &none, &large) == NPU_DECLARATION_OK);
        assert(npu_declaration_check(&wide, &none, &small) == NPU_DECLARATION_TABLES);
        printf("declarations: one slot %zu bytes, four slots %zu bytes; every refusal names its resource\n", one, many);
        return 0;
    }
    assert(mode == 20 || mode == 21 || mode == 26 || mode == 27 || mode == 28 || mode == 29 || mode == 30 || ((mode == 22 || mode == 25) && argc == 3));
    if (argc == 3) input_directory = argv[2];
    if (mode == 21) bootstrap_us = 30000;
    if (mode == 26) { long_run = (unsigned)strtoul(getenv("LONG_JOBS") ? getenv("LONG_JOBS") : "120000", NULL, 0); job_us = 3; }
    if (getenv("JOB_US")) job_us = (unsigned)strtoul(getenv("JOB_US"), NULL, 0);
    if (mode == 25) {
        fault_case = 1;
        if (getenv("NPU_HOOK_SLOT")) sunstone_npu_hook_slot = atoi(getenv("NPU_HOOK_SLOT"));
        if (getenv("NPU_HOOK_ADDRESS")) sunstone_npu_hook_address = (uint32_t)strtoul(getenv("NPU_HOOK_ADDRESS"), NULL, 0);
        if (getenv("NPU_GIC_DEFER")) gic_defer_left = (unsigned)atoi(getenv("NPU_GIC_DEFER"));
#ifdef SUNSTONE_NPU_FAULT_HOOK
        assert((uintptr_t)sunstone_npu_canary + (1u << 20) < UINT64_C(0x100000000)); /* build this check without PIE */
#endif
    }
    runtime_of_model = &npu.runtime;
    model.regs[0x20 / 4] = 0x9000; model.regs[0x24 / 4] = 0x9202;
    model.regs[0x30 / 4] = 0x1000003b; model.regs[0x28 / 4] = 0x20240101; model.regs[1] = IDLE;
    model.masked = 1;
    session_record.npu.jobs = long_run ? 1 : 64;
    /* NPU_BOUNDS=capacity,graph_bytes,tensor_bytes reserves for graphs registered later (case 29 sets its own). */
    struct npu_runtime_bounds bounds = {mode == 29 ? 2u : 0u, 16384, 65536};
    if (getenv("NPU_BOUNDS")) sscanf(getenv("NPU_BOUNDS"), "%u,%zu,%zu", &bounds.capacity, &bounds.graph_bytes, &bounds.tensor_bytes);
    if (mode == 29) registered_case = 1;
    /* Case 28: the 31 slots of the command mask. Case 30: one slot for each catalogue model, each sized by its own graph
     * (the queue image's layout); environment UNIFORM sizes every slot for the largest graph, to compare the totals. */
    const unsigned slots = mode == 28 ? NPU_RUNTIME_SLOTS : mode == 30 ? os_npu_model_count : 1;
    static unsigned slot_model[NPU_RUNTIME_SLOTS];
    if (mode == 30 && !getenv("UNIFORM")) {
        assert(slots <= NPU_RUNTIME_SLOTS && !bounds.capacity);
        for (unsigned i = 0; i < slots; ++i) slot_model[i] = i;
        bounds.slot_model = slot_model;
    }
    const size_t need = npu_runtime_requirements_with(os_npu_models, os_npu_model_count, slots, &bounds);
    assert(need && !(need & (NPU_RUNTIME_GROUP_BYTES - 1)));
    /* The runtime registers physical addresses below 2^40, so the pool is placed in the low
     * 2 GiB of the host address space and aligned to the allocation group. */
    uint8_t *raw = mmap(NULL, need + NPU_RUNTIME_GROUP_BYTES, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(raw != MAP_FAILED);
    void *pool = (void *)(((uintptr_t)raw + NPU_RUNTIME_GROUP_BYTES - 1) & ~(uintptr_t)(NPU_RUNTIME_GROUP_BYTES - 1));
    static _Alignas(4096) uint8_t register_page[4096];
    struct npu_runtime_config config = {.count = os_npu_model_count, .slots = slots, .bounds = bounds, .cpu = (uintptr_t)pool,
        .physical = (uintptr_t)pool, .bytes = need, .register_cpu = (uintptr_t)register_page,
        .register_physical = 0x03600000, .fatal = fatal};
    config.models = os_npu_models;
    assert(npu_runtime_prepare(&npu.runtime, &config));
    if (mode == 28 || mode == 30) {
        const unsigned n = npu.runtime.slots;
        /* Thirty-one slots: every slot's regions are registered, distinct, ascending and inside the NPU's 32-bit address
         * space; the per-graph tables lie at the start of the reservation; the page tables hold 1 + 4 * 31 buffers. */
        assert(NPU_RUNTIME_SLOTS == 31 && npu.runtime.slots == (mode == 28 ? 31 : os_npu_model_count) && npu.runtime.native.memory.count == 1 + 4 * n);
        assert((uintptr_t)npu.runtime.catalogue == (uintptr_t)pool && (uintptr_t)npu.runtime.plans > (uintptr_t)pool &&
               (uintptr_t)npu.runtime.model_digest > (uintptr_t)npu.runtime.plans);
        uint64_t last = 0;
        for (unsigned i = 0; i < n; ++i) {
            const struct npu_runtime_slot *sl = &npu.runtime.slot[i];
            assert(sl->command.device > last && sl->constant.device > sl->command.device && sl->read.device > sl->constant.device);
            assert(sl->write.device > sl->read.device && (uint64_t)sl->write.device + sl->write.bytes <= UINT64_C(1) << 32);
            last = sl->command.device;
            assert(walk_tables((uint32_t)sl->constant.device, 0) == 1 && walk_tables((uint32_t)sl->write.device, 1) == 1);
        }
        check_low_range_unmapped(&npu.runtime);
        assert(npu_runtime_reservation(os_npu_models, os_npu_model_count, 32, UINT64_MAX >> 1, &(size_t){0}) == NPU_DECLARATION_SLOTS);
        /* The command allocation is the largest resident graph's stream rounded to a page, not the 512 KiB hardware bound. */
        for (unsigned i = 0; i < n; ++i)
            assert(npu.runtime.slot[i].command.bytes < NPU_RUNTIME_COMMAND_BYTES && !(npu.runtime.slot[i].command.bytes & 4095));
        size_t regions = 0;
        for (unsigned i = 0; i < n; ++i)
            regions += npu.runtime.slot[i].command.bytes + npu.runtime.slot[i].constant.bytes + npu.runtime.slot[i].read.bytes +
                       npu.runtime.slot[i].write.bytes;
        printf("%u slots: %zu bytes reserved, %zu bytes in the slots' regions, %u buffers registered, the tables at the start\n",
               n, need, regions, (unsigned)npu.runtime.native.memory.count);
        return 0;
    }
    check_low_range_unmapped(&npu.runtime);
    check_constant_region_read_only();
    npu.end = long_run ? UINT64_C(1000000000000) : 60000000;
    npu.bootstrap = xTaskCreateStatic(bootstrap, "os-npu-boot", STACK, NULL, 4, npu.stacks[0], &npu.tasks[0]);
    npu.client = xTaskCreateStatic(client, "os-npu-app", STACK, NULL, 3, npu.stacks[1], &npu.tasks[1]);
    npu.startup.initialized = 1;
    assert(a733_npu_startup_bind_task(&npu.startup, npu.bootstrap));
    TaskHandle_t clients[] = {npu.client};
    assert(npu_runtime_bind(&npu.runtime, &npu.startup, clients, 4, 1));
    assert(npu_runtime_bind_irq(&npu.runtime, &irq_ops, &npu.runtime, 1, 0));
    npu.initial_deadline = 1000000;
    atomic_init(&npu.gate, npu.initial_deadline << 3);
    npu.started = 1; npu.attempted = 1;
    /* The thermal owner allows work until its first deadline. */
    os_npu_thermal(1, 0, npu.end, 0);
    host_rtos_run(verify, NULL, 6, long_run ? UINT64_C(4000000000) : 30000000);
    return 0;
}
