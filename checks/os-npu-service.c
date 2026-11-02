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
static uint32_t first_site, first_value;
void board_fail_note(uint32_t site, uint32_t value, const void *caller)
{
    (void)caller;
    if (!first_site) { first_site = site; first_value = value; }
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
    unsigned masked, latched, running, bootstrap, triggers, delivered;
} model;
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
    model.regs[0x10 / 4] |= 1; model.regs[1] = IDLE; model.running = 0;
    deliver();
}
/* Registers that the controller reads or writes (a733_npu_controller.c). A
 * register not listed aborts, as does a write sequence out of order. */
static int modeled(unsigned offset, int write)
{
    static const unsigned readable[] = {0x00, 0x04, 0x10, 0x14, 0x20, 0x24, 0x28, 0x30, 0x3c, 0x90, 0x100, 0x104,
                                        0x10c, 0x388, 0x3a8, 0x3b4, 0x414, 0x55c, 0x654};
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
    if (offset == 0x3a4) {
        assert(!model.running && model.masked);
        model.bootstrap = model.regs[0x654 / 4] == runtime_of_model->bootstrap.device;
        ++model.triggers;
        model.running = 1; model.regs[1] = 0;
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
static int irq_mask(void *cookie) { assert(cookie == runtime_of_model); model.masked = 1; return 1; }
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
static void verify(void *unused)
{
    (void)unused;
    for (unsigned waited = 0; !finished && live_value && waited < 5000; ++waited) vTaskDelay(pdMS_TO_TICKS(1));
    printf("open %u load %u submit %u close %u finished %u failures %u site %#x value %#x triggers %u\n",
           result_open, result_load, result_submit, result_close, finished, board_failures, first_site,
           first_value, model.triggers);
    assert(finished && live_value && !board_failures && !atomic_load(&os_npu_first_failure.ready));
    assert(result_open == 1 && result_load == 1 && result_submit == 1 && result_close == 1);
    assert(model.triggers == 2 && npu_service_irq_completions(&npu.runtime.service) == 2);
    /* OPEN is answered while the bootstrap runs; the job starts only after it completes. */
    printf("controller state after open %u load %u submit %u; times us: open %llu load %llu submit %llu\n",
           state_after_open, state_after_load, state_after_submit, (unsigned long long)time_open,
           (unsigned long long)time_load, (unsigned long long)time_submit);
    assert(state_after_open == A733_NPU_BOOTING && state_after_submit == A733_NPU_READY);
    assert(time_submit >= time_open + bootstrap_us + job_us);
    if (bootstrap_us > JOB_US * 4) assert(state_after_load == A733_NPU_BOOTING);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const unsigned mode = (unsigned)strtoul(argv[1], NULL, 10);
    assert(mode == 20 || mode == 21);
    if (mode == 21) bootstrap_us = 30000;
    runtime_of_model = &npu.runtime;
    model.regs[0x20 / 4] = 0x9000; model.regs[0x24 / 4] = 0x9202;
    model.regs[0x30 / 4] = 0x1000003b; model.regs[0x28 / 4] = 0x20240101; model.regs[1] = IDLE;
    model.masked = 1;
    session_record.npu.jobs = 1;
    const unsigned slots = 1;
    const size_t need = npu_runtime_requirements(os_npu_models, os_npu_model_count, slots);
    assert(need && !(need & (NPU_RUNTIME_GROUP_BYTES - 1)));
    /* The runtime registers physical addresses below 2^40, so the pool is placed in the low
     * 2 GiB of the host address space and aligned to the allocation group. */
    uint8_t *raw = mmap(NULL, need + NPU_RUNTIME_GROUP_BYTES, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(raw != MAP_FAILED);
    void *pool = (void *)(((uintptr_t)raw + NPU_RUNTIME_GROUP_BYTES - 1) & ~(uintptr_t)(NPU_RUNTIME_GROUP_BYTES - 1));
    static _Alignas(4096) uint8_t register_page[4096];
    struct npu_runtime_config config = {.count = os_npu_model_count, .slots = slots, .cpu = (uintptr_t)pool,
        .physical = (uintptr_t)pool, .bytes = need, .register_cpu = (uintptr_t)register_page,
        .register_physical = 0x03600000, .fatal = fatal};
    memcpy(config.models, os_npu_models, os_npu_model_count * sizeof(os_npu_models[0]));
    assert(npu_runtime_prepare(&npu.runtime, &config));
    npu.end = 60000000;
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
    host_rtos_run(verify, NULL, 6, 10000000);
    return 0;
}
