/* SPDX-License-Identifier: GPL-2.0-only */
/* The NPU load of stage 3b (npu.h). The producer task runs on CPU0 and keeps LATENCY_NPU_IN_FLIGHT relu jobs queued while
 * a run lasts. The board image submits through the EL0 ring (include/sunstone_npu_queue.h), as accel-overlap's producer
 * does; the QEMU models replace the ring with a stand-in device whose jobs complete, one after another, on CPU0's virtual
 * timer interrupt, so the control flow and the report are the same and only the device differs. */
#include "sunstone_app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "sunstone_latency.h"
#include "npu.h"
#include <stdatomic.h>
#include <string.h>
#ifdef SUNSTONE_LATENCY_MODEL
#include "platform.h"
#else
#include "sunstone_npu_queue.h"
#endif

/* The producer's commands. A command runs once, when its sequence moves: a completion may leave a notification behind. */
enum { NPU_RUN = 1, NPU_CLOSE };
static atomic_uint command, sequence, ready, running, drained, closed, stop;
static atomic_uint_fast64_t completed_total, submitted_total;
static struct latency_npu_run *current;
static StaticTask_t producer_state;
static StackType_t producer_stack[4096];
static TaskHandle_t producer_task;
static uint64_t open_ticks, register_ticks, close_ticks;
static uint32_t producer_core = UINT32_MAX, input_bytes, reference_bytes;

#define fail latency_npu_fail
#ifdef SUNSTONE_LATENCY_MODEL
/* The stand-in device: jobs complete one after another, LATENCY_NPU_MODEL_JOB_US apart, on CPU0's CNTV interrupt. The
 * producer runs at EL0 on CPU0, as on the board, and CNTV is open to EL0 there (platform/qemu-smp/platform.c). Only CPU0
 * touches the device, so the producer's submission and the interrupt interleave but never run at once: the producer counts
 * the job first and starts the timer only when the device was idle, and the interrupt marks the device idle before it
 * stops the timer at its last job. */
#ifndef LATENCY_NPU_MODEL_JOB_US
#define LATENCY_NPU_MODEL_JOB_US 250u
#endif
static atomic_uint_fast64_t device_done, device_queued;
static atomic_uint device_idle = 1;
static uint64_t model_job_ticks;
static void model_arm(void)
{
    const uint64_t at = ss_latency_now() + model_job_ticks;
    __asm__ volatile("msr cntv_cval_el0,%0\n msr cntv_ctl_el0,%1\n isb" ::"r"(at), "r"((uint64_t)1) : "memory");
}
void latency_npu_model_irq(void)
{
    const uint64_t done = atomic_load(&device_done) + 1;
    atomic_store(&device_done, done);
    if (done < atomic_load(&device_queued)) model_arm();
    else {
        atomic_store(&device_idle, 1);
        __asm__ volatile("msr cntv_ctl_el0,xzr\n isb" ::: "memory");
    }
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(producer_task, &woken);
    portYIELD_FROM_ISR(woken);
}
static void device_open(void)
{
    const uint64_t t0 = ss_latency_now();
    model_job_ticks = ss_latency_counter_frequency() / 1000000u * LATENCY_NPU_MODEL_JOB_US;
    open_ticks = ss_latency_now() - t0;
    register_ticks = 0;
    input_bytes = reference_bytes = 5632u;
}
static uint64_t device_submit(void)
{
    const uint64_t fence = atomic_fetch_add(&device_queued, 1) + 1;
    if (atomic_exchange(&device_idle, 0)) model_arm();
    return fence;
}
static void device_wait(uint64_t fence)
{
    while (atomic_load(&device_done) < fence) (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
static void device_release(uint64_t fence) { (void)fence; }
static int device_output_differs(void) { return 0; }
static void device_close(void) { close_ticks = 0; }
void latency_npu_fail(uint32_t detail, uint32_t value)
{
    platform_puts("FAIL latency npu detail="); platform_hex(detail);
    platform_puts(" value="); platform_hex(value); platform_puts("\n");
    configASSERT(0);
    for (;;) {}
}
#else
void latency_npu_fail(uint32_t detail, uint32_t value) { ss_fail_value(detail, value); }
/* The vectors region of stage 1b (apps/accel-overlap/pack.py, version 1): the graph's input and its simulator reference. */
struct vectors { uint32_t magic, version, input_offset, input_bytes, reference_offset, reference_bytes; };
#define VECTORS_MAGIC 0x56564f41u
#define REGION_VECTORS 2u
static struct ss_npu_queue *queue;
static ss_npu_handle relu;
static const uint8_t *reference;
static struct ss_npu_fence fences[LATENCY_NPU_IN_FLIGHT];
static uint64_t ticks_of_us(uint64_t us) { return ss_latency_counter_frequency() / 1000000u * us; }
static void device_open(void)
{
    const struct ss_grant *grant = ss_grant();
    if (!grant || grant->npu_core != LATENCY_NPU_CORE || grant->region_count <= REGION_VECTORS) fail(LATENCY_NPU_FAIL_GRANT, 0);
    const uint8_t *base = grant->regions[REGION_VECTORS].base;
    const size_t bytes = grant->regions[REGION_VECTORS].bytes;
    const struct vectors *v = (const void *)base;
    if (bytes < sizeof(*v) || v->magic != VECTORS_MAGIC || v->version != 1 || v->input_offset > bytes ||
        v->input_bytes > bytes - v->input_offset || v->reference_offset > bytes || v->reference_bytes > bytes - v->reference_offset ||
        !v->input_bytes || v->reference_bytes != v->input_bytes) fail(LATENCY_NPU_FAIL_VECTORS, 0);
    input_bytes = v->input_bytes;
    reference_bytes = v->reference_bytes;
    reference = base + v->reference_offset;
    const uint64_t began = ss_latency_now();
    int opened;
    while ((opened = ss_npu_queue_open(LATENCY_NPU_IN_FLIGHT, &queue)) != 1) {
        if (opened < 0 || ss_latency_now() - began > ticks_of_us(LATENCY_NPU_OPEN_US)) fail(LATENCY_NPU_FAIL_OPEN, (uint32_t)opened);
        vTaskDelay(1);
    }
    const uint64_t registering = ss_latency_now();
    open_ticks = registering - began;
    struct ss_npu_tensor t;
    if (!ss_npu_queue_register(queue, 0, &relu)) fail(LATENCY_NPU_FAIL_REGISTER, 0);
    if (!ss_npu_queue_tensor(queue, relu, SS_NPU_INPUT, 0, &t) || t.bytes < input_bytes) fail(LATENCY_NPU_FAIL_REGISTER, 1);
    memcpy(t.data, base + v->input_offset, input_bytes);
    if (!ss_npu_queue_dirty(queue, relu, SS_NPU_INPUT, 0, 0, input_bytes)) fail(LATENCY_NPU_FAIL_REGISTER, 2);
    if (ss_npu_queue_tensor(queue, relu, SS_NPU_INPUT, 1, &t)) fail(LATENCY_NPU_FAIL_REGISTER, 3);
    register_ticks = ss_latency_now() - registering;
}
static uint64_t device_submit(void)
{
    const uint64_t index = atomic_load_explicit(&submitted_total, memory_order_relaxed);
    struct ss_npu_fence *fence = &fences[index % LATENCY_NPU_IN_FLIGHT];
    const uint64_t began = ss_latency_now();
    int r;
    while ((r = ss_npu_queue_submit(queue, relu, (struct ss_npu_fence){0}, LATENCY_NPU_JOB_US, fence)) == 0) {
        if (ss_latency_now() - began > ticks_of_us(LATENCY_NPU_JOB_US)) fail(LATENCY_NPU_FAIL_SUBMIT, 0);
        taskYIELD();
    }
    if (r < 0) fail(LATENCY_NPU_FAIL_SUBMIT, (uint32_t)r);
    return index + 1;
}
static void device_wait(uint64_t fence)
{
    const int r = ss_npu_queue_wait(queue, fences[(fence - 1) % LATENCY_NPU_IN_FLIGHT]);
    if (r != 1) fail(LATENCY_NPU_FAIL_WAIT, (uint32_t)r);
}
static void device_release(uint64_t fence)
{
    if (ss_npu_queue_release(queue, fences[(fence - 1) % LATENCY_NPU_IN_FLIGHT]) != 1) fail(LATENCY_NPU_FAIL_RELEASE, 0);
}
/* The last output of a run against the simulator reference; the queue cleans and invalidates it before the CPU reads it. */
static int device_output_differs(void)
{
    const uint64_t began = ss_latency_now();
    int r;
    while ((r = ss_npu_queue_acquire_handle(queue, relu)) != 1) {
        if (r < 0 || ss_latency_now() - began > ticks_of_us(LATENCY_NPU_JOB_US)) fail(LATENCY_NPU_FAIL_OUTPUT, (uint32_t)r);
        taskYIELD();
    }
    struct ss_npu_tensor t;
    if (!ss_npu_queue_tensor(queue, relu, SS_NPU_OUTPUT, 0, &t) || t.bytes < reference_bytes) fail(LATENCY_NPU_FAIL_OUTPUT, 1);
    return memcmp(t.data, reference, reference_bytes) != 0;
}
static void device_close(void)
{
    const uint64_t began = ss_latency_now();
    int r;
    while ((r = ss_npu_queue_close(queue)) != 1) {
        if (r < 0 || ss_latency_now() - began > ticks_of_us(LATENCY_NPU_CLOSE_US)) fail(LATENCY_NPU_FAIL_CLOSE, (uint32_t)r);
        vTaskDelay(1);
    }
    close_ticks = ss_latency_now() - began;
}
#endif

/* Continuous jobs at the declared depth until the control task stops the run, then the drain and the output check. */
static void run_jobs(struct latency_npu_run *run)
{
    uint64_t submitted = atomic_load_explicit(&submitted_total, memory_order_relaxed);
    uint64_t done = atomic_load_explicit(&completed_total, memory_order_relaxed);
    run->jobs_before = done;
    const uint64_t first = submitted;
    for (;;) {
        const int more = !atomic_load_explicit(&stop, memory_order_acquire);
        while (more && submitted - done < LATENCY_NPU_IN_FLIGHT) {
            const uint64_t fence = device_submit();
            submitted = fence;
            atomic_store_explicit(&submitted_total, submitted, memory_order_release);
        }
        if (done == submitted) break;
        device_wait(done + 1);
        device_release(done + 1);
        ++done;
        atomic_store_explicit(&completed_total, done, memory_order_release);
    }
    run->drained = ss_latency_now();
    run->submitted = submitted - first;
    run->completed = done - run->jobs_before;
    run->compared = 1;
    run->mismatches = (uint32_t)device_output_differs();
    if (run->mismatches) fail(LATENCY_NPU_FAIL_MISMATCH, (uint32_t)run->completed);
}
static void producer(void *unused)
{
    (void)unused;
    producer_core = portGET_CORE_ID();
    if (producer_core != LATENCY_NPU_CORE) fail(LATENCY_NPU_FAIL_GRANT, producer_core);
    device_open();
    atomic_store_explicit(&ready, 1, memory_order_release);
    unsigned handled = 0;
    for (;;) {
        while (atomic_load_explicit(&sequence, memory_order_acquire) == handled) (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ++handled;
        if (atomic_load_explicit(&command, memory_order_acquire) == NPU_CLOSE) break;
        run_jobs(current);
        atomic_store_explicit(&running, 0, memory_order_release);
        atomic_store_explicit(&drained, 1, memory_order_release);
    }
    device_close();
    atomic_store_explicit(&closed, 1, memory_order_release);
    vTaskSuspend(NULL);
}
static void post(unsigned c)
{
    atomic_store_explicit(&command, c, memory_order_release);
    atomic_fetch_add_explicit(&sequence, 1, memory_order_acq_rel);
    xTaskNotifyGive(producer_task);
}

void latency_npu_init(void)
{
    /* Below the wake tasks (configMAX_PRIORITIES - 1), above CPU0's idle memory task (1). */
    producer_task = ss_task_create_static_affinity_set(producer, "latency-npu", 4096, NULL, 2, producer_stack,
                                                       &producer_state, 1U << LATENCY_NPU_CORE);
    configASSERT(producer_task);
}
int latency_npu_ready(void) { return (int)atomic_load_explicit(&ready, memory_order_acquire); }
uint64_t latency_npu_jobs(void) { return atomic_load_explicit(&completed_total, memory_order_acquire); }
unsigned latency_npu_in_flight(void)
{
    const uint64_t done = atomic_load_explicit(&completed_total, memory_order_acquire);
    return (unsigned)(atomic_load_explicit(&submitted_total, memory_order_acquire) - done);
}
void latency_npu_start(struct latency_npu_run *run)
{
    configASSERT(!atomic_load_explicit(&running, memory_order_acquire));
    current = run;
    run->requested = ss_latency_now();
    atomic_store_explicit(&stop, 0, memory_order_release);
    atomic_store_explicit(&drained, 0, memory_order_release);
    atomic_store_explicit(&running, 1, memory_order_release);
    post(NPU_RUN);
}
void latency_npu_stop(struct latency_npu_run *run)
{
    run->stop_requested = ss_latency_now();
    atomic_store_explicit(&stop, 1, memory_order_release);
}
int latency_npu_drained(void) { return (int)atomic_load_explicit(&drained, memory_order_acquire); }
int latency_npu_running(void) { return (int)atomic_load_explicit(&running, memory_order_acquire); }
void latency_npu_close(void) { post(NPU_CLOSE); }
int latency_npu_closed(void) { return (int)atomic_load_explicit(&closed, memory_order_acquire); }
void latency_npu_setup(struct latency_npu_report *report)
{
    report->open_ticks = open_ticks;
    report->register_ticks = register_ticks;
    report->close_ticks = close_ticks;
    report->producer_core = producer_core;
    report->graph_input_bytes = input_bytes;
    report->graph_reference_bytes = reference_bytes;
}
