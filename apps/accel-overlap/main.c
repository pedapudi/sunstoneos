/* Concurrent GPU, NPU and CPU streaming work in the one OS image, with the completion latency of each accelerator.
 *
 * The main task on core 6 owns the GPU queue and waits for its completions (IRQ95 on core 6). The NPU producer on core 1
 * owns the EL0 ring and waits for its completions (IRQ97 on core 1). Streaming workers on cores 2-5 and 7 read and rewrite
 * 4 MiB each of the stream region. Five phases run in order: the GPU alone, the NPU alone, both, both with streaming, and
 * GPU close and reopen cycles while NPU jobs stay in flight under streaming. Each accelerator phase has a serial segment
 * (one job at a time, a blocking wait, the completion latency of every job) and a pipelined segment of one second (the job
 * rate at the declared depth). Every GPU output of the serial segment and the last of the pipelined one is compared with
 * its arithmetic result; every NPU output of the serial segment and the last of the pipelined one is compared with the
 * simulator reference. The first difference ends the session with a failure record.
 *
 * Built with ACCEL_OVERLAP_SWEEP, the five phases instead run both accelerators pipelined together for one second each, with
 * outputs checked as above, under a different set of streaming workers: none, core 2, cores 2-4, cores 2-5 and 7, and core 7
 * alone. A chase task on core 0 times dependent loads through the chase region during every phase, so each phase gives the
 * loaded DRAM latency beside the accelerators' job rates and the streaming bandwidth (docs/designs/concurrent-accel.md,
 * "Stage 1b").
 *
 * Built with ACCEL_OVERLAP_COUNTERS (the GPU-width build plus the NSI port 6 and 18 traffic counters), four phases run:
 * idle for 100 ms, the GPU alone, the NPU alone and the core-2 stream alone, one second each. The OS counter owner
 * (include/a733_nsi_counters.h) counts each phase on both ports, with a sample at half the phase, so the bytes of jobs
 * whose traffic is known calibrate the counters and the GPU port's requests per job are measured. */
#include "accel_overlap-app.h"
#include "sunstone_app.h"
#include "sunstone_npu_queue.h"
#include "sunstone_npu_queue_service.h"
#include "gpu_queue.h"
#ifdef ACCEL_OVERLAP_WIDTH
#include "add4/program.h"           /* four consecutive elements per lane (add4/vector_add4.cl) */
#define GPU_LANE_ELEMENTS 4u
#else
#include "../queue-add/program.h"
#define GPU_LANE_ELEMENTS 1u
#endif
#include "result.h"
#ifdef ACCEL_OVERLAP_COUNTERS
#include "a733_nsi_counters.h"
static void counters_poll(void);
#else
#define counters_poll() ((void)0)
#endif
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>
#include <string.h>

#ifdef ACCEL_OVERLAP_LARGE
#define GPU_ELEMENTS 262144u            /* 2 MiB of inputs and 1 MiB of output per job */
#define GPU_SLOT_BYTES (1u << 20)
#else
#define GPU_ELEMENTS 65u
#define GPU_SLOT_BYTES 320u
#endif
#define GPU_GROUPS ((GPU_ELEMENTS / GPU_LANE_ELEMENTS + 31u) / 32u)
_Static_assert(GPU_ELEMENTS % GPU_LANE_ELEMENTS == 0, "each lane adds whole groups of elements");
#define GPU_SERIAL_JOBS 2000u
#define NPU_SERIAL_JOBS 4000u
#define PIPELINE_TICKS 24000000u        /* one second */
#define GPU_WAIT_TICKS 200u             /* scheduler ticks of a GPU wait or submission retry */
#define NPU_TIMEOUT_US 1000000u
#define PLL_CYCLES 3u
#define STREAM_WORKERS 5u
#define STREAM_BYTES (4u << 20)
#define HISTOGRAM 16384u                /* buckets of HISTOGRAM_SHIFT ticks: 2 ticks each, up to 1.365 ms */
#define HISTOGRAM_SHIFT 1u
#define NPU_CORE 1u
#define MAIN_CORE 6u
#define ALL_STREAMS ((1u << STREAM_WORKERS) - 1u)

static const unsigned stream_core[STREAM_WORKERS] = {2, 3, 4, 5, 7};
#ifndef ACCEL_OVERLAP_SWEEP
static const uint32_t phase_devices[ACCEL_OVERLAP_PHASES] = {
    ACCEL_OVERLAP_GPU, ACCEL_OVERLAP_NPU, ACCEL_OVERLAP_GPU | ACCEL_OVERLAP_NPU,
    ACCEL_OVERLAP_GPU | ACCEL_OVERLAP_NPU | ACCEL_OVERLAP_STREAM,
    ACCEL_OVERLAP_GPU | ACCEL_OVERLAP_NPU | ACCEL_OVERLAP_STREAM | ACCEL_OVERLAP_PLL_CYCLES};
#else
/* Streaming sets (bit w: the worker on stream_core[w]): none, core 2, cores 2-4, cores 2-5 and 7, core 7 alone. */
#ifndef ACCEL_OVERLAP_LARGE
static const uint32_t phase_streams[ACCEL_OVERLAP_PHASES] = {0x00, 0x01, 0x07, ALL_STREAMS, 0x10};
#endif
#define CHASE_CORE 0u
#define CHASE_LINE 64u
#define CHASE_LOADS 64u
#ifdef ACCEL_OVERLAP_LARGE
/* Phases of the larger-job build: each device alone, then both under no streaming, core 2, core 7, cores 2-4, cores 2-5 and
 * 7, and core 2 and core 7 each streaming over three interleaved areas. */
#define SWEEP_PHASES ACCEL_OVERLAP_LARGE_PHASES
#define BOTH (ACCEL_OVERLAP_GPU | ACCEL_OVERLAP_NPU)
#ifndef ACCEL_OVERLAP_COUNTERS
static const uint32_t large_devices[SWEEP_PHASES] = {ACCEL_OVERLAP_GPU, ACCEL_OVERLAP_NPU, BOTH, BOTH, BOTH, BOTH, BOTH, BOTH, BOTH};
static const uint32_t large_streams[SWEEP_PHASES] = {0, 0, 0, 0x01, 0x10, 0x07, ALL_STREAMS,
                                                     0x01 | ACCEL_OVERLAP_INTERLEAVED, 0x10 | ACCEL_OVERLAP_INTERLEAVED};
#endif
static struct accel_overlap_large_result large_result;
#ifdef ACCEL_OVERLAP_COUNTERS
/* The counter calibration: idle (100 ms), the GPU alone, the NPU alone and the core-2 stream alone (one second each). */
static const uint32_t counter_devices[ACCEL_OVERLAP_COUNTER_PHASES] = {0, ACCEL_OVERLAP_GPU, ACCEL_OVERLAP_NPU, 0};
static const uint32_t counter_streams[ACCEL_OVERLAP_COUNTER_PHASES] = {0, 0, 0, 0x01};
static const uint32_t counter_ticks[ACCEL_OVERLAP_COUNTER_PHASES] = {2400000u, 24000000u, 24000000u, 24000000u};
static struct accel_overlap_counter_result counter_result;
#endif
#define CHASE_LINES_FIELD large_result.chase_lines
#else
static struct accel_overlap_sweep_result sweep_result;  /* its base is copied from result at the end */
#define SWEEP_PHASES ACCEL_OVERLAP_PHASES
#define CHASE_LINES_FIELD sweep_result.chase_lines
#endif
static uint32_t chase_histogram[HISTOGRAM];
static atomic_uint chase_run, chase_done, chase_ready;
static StaticTask_t chase_tcb;
static StackType_t chase_stack[1024];
static TaskHandle_t chase_task;
static struct accel_overlap_sweep *chase_phase;
#endif
static struct accel_overlap_result result;
#ifndef SWEEP_PHASES
#define SWEEP_PHASES ACCEL_OVERLAP_PHASES
#endif
#ifdef ACCEL_OVERLAP_LARGE
#define STREAM_AREAS (STREAM_WORKERS + 4u)  /* two more areas each for the workers on cores 2 and 7 */
#else
#define STREAM_AREAS STREAM_WORKERS
#endif
#ifndef ACCEL_OVERLAP_SWEEP
static uint32_t gpu_histogram[2][HISTOGRAM];
#endif
static uint32_t npu_histogram[2][HISTOGRAM];

/* Commands from the main task to the NPU producer and the streaming workers. */
enum { NPU_IDLE, NPU_SERIAL, NPU_PIPELINED, NPU_CONTINUOUS, NPU_EXIT };
static atomic_uint npu_command, npu_done, npu_stop, stream_run, stream_stopped, stream_mask, stream_interleaved;
static atomic_uint_fast64_t pipeline_end, stream_sweeps;
static atomic_uint gpu_outstanding;     /* GPU jobs submitted and not yet collected */
static atomic_uint npu_ready;
static StaticTask_t npu_tcb, stream_tcb[STREAM_WORKERS];
static StackType_t npu_stack[4096], stream_stack[STREAM_WORKERS][1024];
static TaskHandle_t npu_task, main_task, stream_task[STREAM_WORKERS];
static struct accel_overlap_phase *current;
static atomic_uint npu_sequence;        /* commands given; the producer runs each once, whatever notifications it finds */

/* Every time is CNTVCT, the counter of both interrupt stamps (gpu_queue_last_irq_counter, struct ss_npu_queue_wake). */
static uint64_t now(void)
{ uint64_t v; __asm__ volatile("isb\n mrs %0,cntvct_el0" : "=r"(v)::"memory"); return v; }

/* Failure: the first check that differs names itself in the detail (bits 11:0) and its phase (bits 19:16). */
static unsigned phase_index;
static _Noreturn void fail(unsigned site, uint32_t value)
{ ss_fail_value((phase_index << 16) | (site & 0xfffu), value); }
static void live(void)
{
    if (ss_live() < 0) fail(0x001, 0);
    ss_progress();
}

/* Distributions. */
static void sample(uint32_t *histogram, uint32_t *overflow, uint32_t *min, uint32_t *max, uint64_t ticks)
{
    const uint32_t t = ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)ticks;
    if (t < *min) *min = t;
    if (t > *max) *max = t;
    if ((t >> HISTOGRAM_SHIFT) >= HISTOGRAM) ++*overflow;
    else ++histogram[t >> HISTOGRAM_SHIFT];
}
static uint32_t percentile(const uint32_t *histogram, uint32_t count, uint64_t per_million)
{
    /* The smallest bucket at or above which per_million of the samples lie; its upper edge in ticks. */
    const uint64_t rank = (count * per_million + 999999u) / 1000000u;
    uint64_t seen = 0;
    for (unsigned b = 0; b < HISTOGRAM; ++b) {
        seen += histogram[b];
        if (seen >= rank && rank) return ((b + 1u) << HISTOGRAM_SHIFT) - 1u;
    }
    return UINT32_MAX;
}
static void summarise(struct accel_overlap_distribution *d, const uint32_t *histogram)
{
    const uint32_t total = d->count + d->overflow;
    if (!total) { d->min = 0; return; }
    d->p50 = percentile(histogram, total, 500000);
    d->p99 = percentile(histogram, total, 990000);
    d->p999 = percentile(histogram, total, 999000);
    if (d->p50 > d->max) d->p50 = d->max;
    if (d->p99 > d->max) d->p99 = d->max;
    if (d->p999 > d->max) d->p999 = d->max;
}
static void record(struct accel_overlap_distribution *d, uint32_t *histogram, uint64_t ticks)
{
    uint32_t overflow = d->overflow;
    sample(histogram, &overflow, &d->min, &d->max, ticks);
    if (overflow != d->overflow) d->overflow = overflow; else ++d->count;
}
static void reset(struct accel_overlap_distribution *d, uint32_t *histogram)
{
    memset(histogram, 0, HISTOGRAM * sizeof(*histogram));
    *d = (struct accel_overlap_distribution){.min = UINT32_MAX};
}

/* CPU streaming: every sweep reads and rewrites each word of the worker's 4 MiB, and checks the word it reads. */
static void stream_worker(void *argument)
{
    const unsigned w = (unsigned)(uintptr_t)argument;
    const struct ss_grant *grant = ss_grant();
    volatile uint64_t *words = (volatile uint64_t *)((uint8_t *)grant->regions[ACCEL_OVERLAP_REGION_STREAM].base +
                                                     (size_t)w * STREAM_BYTES);
    const size_t count = STREAM_BYTES / sizeof(uint64_t);
    uint64_t sweep = 0;
    for (size_t i = 0; i < count; ++i) words[i] = 0;
#ifdef ACCEL_OVERLAP_LARGE
    /* The workers on cores 2 (w 0) and 7 (w 4) each own two more 4 MiB areas after the five areas of the workers, which they
     * stream together with their own in an interleaved phase. */
    volatile uint64_t *extra[2] = {0, 0};
    uint64_t extra_sweep = 0;
    if (w == 0 || w == 4) {
        uint8_t *after = (uint8_t *)grant->regions[ACCEL_OVERLAP_REGION_STREAM].base +
                         (size_t)(STREAM_WORKERS + (w == 4 ? 2u : 0u)) * STREAM_BYTES;
        for (unsigned k = 0; k < 2; ++k) {
            extra[k] = (volatile uint64_t *)(after + (size_t)k * STREAM_BYTES);
            for (size_t i = 0; i < count; ++i) extra[k][i] = 0;
        }
    }
#endif
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (atomic_load_explicit(&npu_command, memory_order_acquire) == NPU_EXIT) break;
        /* Only the workers of the phase's set run and report their stop. */
        if (!(atomic_load_explicit(&stream_mask, memory_order_acquire) >> w & 1u)) continue;
        while (atomic_load_explicit(&stream_run, memory_order_acquire)) {
            if (portGET_CORE_ID() != stream_core[w]) fail(0x010, portGET_CORE_ID());
#ifdef ACCEL_OVERLAP_LARGE
            if (atomic_load_explicit(&stream_interleaved, memory_order_acquire)) {
                if (!extra[0]) fail(0x013, w);
                /* Three sequential streams in one loop: each step reads and rewrites one word of each area. */
                for (size_t i = 0; i < count; ++i) {
                    const uint64_t a = words[i], b = extra[0][i], c = extra[1][i];
                    if (a != sweep || b != extra_sweep || c != extra_sweep) fail(0x014, (uint32_t)i);
                    words[i] = sweep + 1; extra[0][i] = extra_sweep + 1; extra[1][i] = extra_sweep + 1;
                }
                ++sweep; ++extra_sweep;
                atomic_fetch_add_explicit(&stream_sweeps, 3, memory_order_relaxed);
                continue;
            }
#endif
            for (size_t i = 0; i < count; ++i) {
                const uint64_t old = words[i];
                if (old != sweep) fail(0x011, (uint32_t)i);
                words[i] = sweep + 1;
            }
            ++sweep;
            atomic_fetch_add_explicit(&stream_sweeps, 1, memory_order_relaxed);
        }
        atomic_fetch_add_explicit(&stream_stopped, 1, memory_order_release);
        xTaskNotifyGive(main_task);
    }
    vTaskSuspend(NULL);
}
static void stream_start(uint32_t mask)
{
    atomic_store_explicit(&stream_interleaved, mask >> 8 & 1u, memory_order_relaxed);
    mask &= 0xffu;
    atomic_store_explicit(&stream_sweeps, 0, memory_order_relaxed);
    atomic_store_explicit(&stream_stopped, 0, memory_order_relaxed);
    atomic_store_explicit(&stream_mask, mask, memory_order_relaxed);
    atomic_store_explicit(&stream_run, 1, memory_order_release);
    for (unsigned w = 0; w < STREAM_WORKERS; ++w)
        if (mask >> w & 1u) xTaskNotifyGive(stream_task[w]);
}
static void stream_stop(void)
{
    atomic_store_explicit(&stream_run, 0, memory_order_release);
    const unsigned workers = (unsigned)__builtin_popcount(atomic_load_explicit(&stream_mask, memory_order_relaxed));
    const uint64_t began = now();
    while (atomic_load_explicit(&stream_stopped, memory_order_acquire) != workers) {
        live();
        if (now() - began > 24000000u) fail(0x012, atomic_load(&stream_stopped));
        (void)ulTaskNotifyTake(pdTRUE, 10);
    }
    current->sweeps = atomic_load_explicit(&stream_sweeps, memory_order_relaxed);
}

#ifdef ACCEL_OVERLAP_SWEEP
/* The chase (core 0): the first word of every 64-byte line of the chase region holds the address of the next line, in one
 * cycle through all lines (Sattolo's shuffle gives a single cycle), so each load waits for the one before and no prefetcher
 * can predict the next address. */
static void chase_task_entry(void *unused)
{
    (void)unused;
    const struct ss_grant *grant = ss_grant();
    uint8_t *base = grant->regions[ACCEL_OVERLAP_REGION_CHASE].base;
    const size_t lines = grant->regions[ACCEL_OVERLAP_REGION_CHASE].bytes / CHASE_LINE;
    if (portGET_CORE_ID() != CHASE_CORE || lines < 2) fail(0x080, (uint32_t)lines);
    for (size_t i = 0; i < lines; ++i) *(uintptr_t *)(base + i * CHASE_LINE) = (uintptr_t)(base + i * CHASE_LINE);
    uint64_t x = now() | 1u;
    for (size_t i = lines - 1; i > 0; --i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        const size_t j = (size_t)(x % i);
        uintptr_t *a = (uintptr_t *)(base + i * CHASE_LINE), *b = (uintptr_t *)(base + j * CHASE_LINE);
        const uintptr_t t = *a; *a = *b; *b = t;
    }
    /* One walk of the cycle: it returns to the first line after exactly every line. */
    uintptr_t p = (uintptr_t)base;
    uint64_t steps = 0;
    do { p = *(volatile uintptr_t *)p; ++steps; } while (p != (uintptr_t)base && steps <= lines);
    if (steps != lines) fail(0x081, (uint32_t)steps);
    CHASE_LINES_FIELD = steps;
    atomic_store_explicit(&chase_ready, 1, memory_order_release);
    xTaskNotifyGive(main_task);
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (atomic_load_explicit(&npu_command, memory_order_acquire) == NPU_EXIT) break;
        if (!atomic_load_explicit(&chase_run, memory_order_acquire)) continue;
        struct accel_overlap_sweep *c = chase_phase;
        reset(&c->chase, chase_histogram);
        c->loads_per_sample = CHASE_LOADS;
        const uint64_t began = now();
        while (atomic_load_explicit(&chase_run, memory_order_acquire)) {
            const uint64_t t = now();
            for (unsigned k = 0; k < CHASE_LOADS; ++k) p = *(volatile uintptr_t *)p;
            record(&c->chase, chase_histogram, now() - t);
            c->loads += CHASE_LOADS;
        }
        c->chase_ticks = now() - began;
        summarise(&c->chase, chase_histogram);
        atomic_store_explicit(&chase_done, 1, memory_order_release);
        xTaskNotifyGive(main_task);
    }
    vTaskSuspend(NULL);
}
static void chase_begin(struct accel_overlap_sweep *c)
{
    chase_phase = c;
    atomic_store_explicit(&chase_done, 0, memory_order_relaxed);
    atomic_store_explicit(&chase_run, 1, memory_order_release);
    xTaskNotifyGive(chase_task);
}
static void chase_end(void)
{
    atomic_store_explicit(&chase_run, 0, memory_order_release);
    const uint64_t began = now();
    while (!atomic_load_explicit(&chase_done, memory_order_acquire)) {
        live();
        if (now() - began > 24000000u) fail(0x082, 0);
        (void)ulTaskNotifyTake(pdTRUE, 10);
    }
}
#endif

/* The NPU producer (core 1). */
static struct ss_npu_queue *queue;
static ss_npu_handle relu;
static const uint8_t *npu_reference;
static size_t npu_reference_bytes;
static struct ss_npu_fence fences[ACCEL_OVERLAP_NPU_IN_FLIGHT];

static uint32_t npu_output_differs(void)
{
    int r;
    const uint64_t began = now();
    while ((r = ss_npu_queue_acquire_handle(queue, relu)) != 1) {
        if (r < 0 || now() - began > 48000000u) fail(0x020, (uint32_t)r);
        taskYIELD();
    }
    struct ss_npu_tensor t;
    /* The tensor may be longer than the simulator's output (alignment); its first bytes are the output. */
    if (!ss_npu_queue_tensor(queue, relu, SS_NPU_OUTPUT, 0, &t) || t.bytes < npu_reference_bytes) fail(0x021, (uint32_t)t.bytes);
    return memcmp(t.data, npu_reference, npu_reference_bytes) != 0;
}
static void npu_submit(struct ss_npu_fence *fence)
{
    int r;
    while ((r = ss_npu_queue_submit(queue, relu, (struct ss_npu_fence){0}, NPU_TIMEOUT_US, fence)) == 0) taskYIELD();
    if (r < 0) fail(0x022, (uint32_t)r);
}
static void npu_finish(struct ss_npu_fence fence)
{
    const int r = ss_npu_queue_wait(queue, fence);
    if (r != 1) fail(0x023, (uint32_t)r);
    if (atomic_load_explicit(&gpu_outstanding, memory_order_acquire)) ++current->overlap;
    if (ss_npu_queue_release(queue, fence) != 1) fail(0x024, 0);
}
static void npu_serial(struct accel_overlap_device *d)
{
    reset(&d->wake, npu_histogram[0]);
    reset(&d->trip, npu_histogram[1]);
    while (d->serial_jobs < NPU_SERIAL_JOBS || !atomic_load_explicit(&npu_stop, memory_order_acquire)) {
        struct ss_npu_fence f;
        const uint64_t began = now();
        npu_submit(&f);
        npu_finish(f);
        const uint64_t end = now();
        const struct ss_npu_queue_wake w = queue->wake;
        if (w.resumed && w.irq && w.resumed >= w.irq && w.resumed <= end) record(&d->wake, npu_histogram[0], w.resumed - w.irq);
        else ++d->wake.missing;
        record(&d->trip, npu_histogram[1], end - began);
        if (npu_output_differs()) { ++d->mismatches; fail(0x025, d->serial_jobs); }
        ++d->serial_jobs;
    }
    summarise(&d->wake, npu_histogram[0]);
    summarise(&d->trip, npu_histogram[1]);
}
/* Pipelined jobs at the declared depth until the end time (or, continuous, until the main task stops them). */
static void npu_pipelined(struct accel_overlap_device *d, int continuous)
{
    unsigned submitted = 0, completed = 0;
    const uint64_t began = now();
    d->depth = ACCEL_OVERLAP_NPU_IN_FLIGHT;
    for (;;) {
        const int more = continuous ? !atomic_load_explicit(&npu_stop, memory_order_acquire)
                                    : now() < atomic_load_explicit(&pipeline_end, memory_order_acquire);
        while (more && submitted - completed < ACCEL_OVERLAP_NPU_IN_FLIGHT) {
            npu_submit(&fences[submitted % ACCEL_OVERLAP_NPU_IN_FLIGHT]);
            ++submitted;
        }
        if (completed == submitted) break;
        npu_finish(fences[completed % ACCEL_OVERLAP_NPU_IN_FLIGHT]);
        ++completed;
    }
    d->pipelined_ticks = now() - began;
    d->pipelined_jobs = completed;
    if (npu_output_differs()) { ++d->mismatches; fail(0x026, completed); }
}
static void npu_producer(void *unused)
{
    (void)unused;
    const struct ss_grant *grant = ss_grant();
    const uint8_t *vectors = grant->regions[ACCEL_OVERLAP_REGION_VECTORS].base;
    const struct accel_overlap_vectors *v = (const void *)vectors;
    const size_t vectors_bytes = grant->regions[ACCEL_OVERLAP_REGION_VECTORS].bytes;
    if (v->magic != ACCEL_OVERLAP_VECTORS_MAGIC || v->version != 1 || v->input_offset > vectors_bytes ||
        v->input_bytes > vectors_bytes - v->input_offset || v->reference_offset > vectors_bytes ||
        v->reference_bytes > vectors_bytes - v->reference_offset) fail(0x030, v->magic);
    npu_reference = vectors + v->reference_offset;
    npu_reference_bytes = v->reference_bytes;
    const uint64_t began = now();
    int opened;
    while ((opened = ss_npu_queue_open(ACCEL_OVERLAP_NPU_IN_FLIGHT, &queue)) != 1) {
        if (opened < 0 || now() - began > 192000000u) fail(0x031, (uint32_t)opened);
        vTaskDelay(1);
    }
    if (!ss_npu_queue_register(queue, 0, &relu)) fail(0x032, 0);
    struct ss_npu_tensor t;
    if (!ss_npu_queue_tensor(queue, relu, SS_NPU_INPUT, 0, &t) || t.bytes < v->input_bytes) fail(0x033, (uint32_t)t.bytes);
    memcpy(t.data, vectors + v->input_offset, v->input_bytes);
    if (!ss_npu_queue_dirty(queue, relu, SS_NPU_INPUT, 0, 0, v->input_bytes)) fail(0x034, 0);
    if (ss_npu_queue_tensor(queue, relu, SS_NPU_INPUT, 1, &t)) fail(0x035, 1);
    atomic_store_explicit(&npu_ready, 1, memory_order_release);
    xTaskNotifyGive(main_task);
    unsigned handled = 0;
    for (;;) {
        /* A completion may leave a notification behind (the wait's index is this task's own): a command runs once, when
         * its sequence moves. */
        while (atomic_load_explicit(&npu_sequence, memory_order_acquire) == handled)
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ++handled;
        const unsigned command = atomic_load_explicit(&npu_command, memory_order_acquire);
        if (command == NPU_EXIT) break;
        if (command == NPU_SERIAL) npu_serial(&current->npu);
        else if (command == NPU_PIPELINED) npu_pipelined(&current->npu, 0);
        else if (command == NPU_CONTINUOUS) npu_pipelined(&current->npu, 1);
        atomic_store_explicit(&npu_done, command, memory_order_release);
        xTaskNotifyGive(main_task);
    }
    const struct ss_npu_queue_pll_watch p = queue->pll;
    result.pll.reference = p.reference; result.pll.diff = p.diff; result.pll.last = p.last; result.pll.unlocked = p.unlocked;
    result.pll.writes = p.writes; result.pll.in_flight = p.in_flight; result.pll.completions = p.completions;
    result.pll.first_site = p.first_site; result.pll.first_word = p.first_word;
    const uint64_t closing = now();
    int closed;
    while ((closed = ss_npu_queue_close(queue)) != 1) {
        if (closed < 0 || now() - closing > 48000000u) fail(0x036, (uint32_t)closed);
        vTaskDelay(1);
    }
    atomic_store_explicit(&npu_done, NPU_EXIT, memory_order_release);
    xTaskNotifyGive(main_task);
    vTaskSuspend(NULL);
}
static void npu_begin(unsigned command)
{
    atomic_store_explicit(&npu_done, NPU_IDLE, memory_order_relaxed);
    atomic_store_explicit(&npu_command, command, memory_order_relaxed);
    atomic_fetch_add_explicit(&npu_sequence, 1, memory_order_release);
    xTaskNotifyGive(npu_task);
}
static void npu_end(unsigned command)
{
    const uint64_t began = now();
    while (atomic_load_explicit(&npu_done, memory_order_acquire) != command) {
        live();
        counters_poll();
        if (now() - began > 120000000u) fail(0x037, atomic_load(&npu_done));
        (void)ulTaskNotifyTake(pdTRUE, 10);
    }
}

/* The GPU owner (main task, core 6). */
static struct gpu_queue_context context;
static uint32_t *gpu_in;
static const volatile uint32_t *gpu_out;
static struct gpu_queue_job gpu_job;
static uint32_t gpu_sequence;

#ifdef ACCEL_OVERLAP_COUNTERS
/* The counter calls, from the main task through the GPU call gateway. A status other than OK stops the run after the phase
 * it happened in; the record keeps the phase's status and the owner's first failing address. */
static struct accel_overlap_counter_phase *counter_phase;   /* the phase being counted, or none */
static uint64_t counter_live_due;
static int counters_ok;
static uint32_t counters_call(uint32_t op, uint32_t bank, struct a733_nsi_counter_call *c)
{
    *c = (struct a733_nsi_counter_call){.op = op, .bank = bank};
    const int r = gpu_queue_nsi_counters(context, c);
    const uint32_t status = r != GPU_QUEUE_OK ? 0x100u | ((uint32_t)r & 0xffu) : c->status;
    if (status && counters_ok) {
        counters_ok = 0;
        counter_result.fail_address = c->fail_address; counter_result.fail_value = c->fail_value;
    }
    return status;
}
static void counters_note(struct accel_overlap_counter_phase *p, uint32_t status)
{ if (status && !p->status) p->status = status; }
static void counters_open(void)
{
    struct a733_nsi_counter_call c;
    counters_ok = 1;
    counter_result.open_status = counters_call(A733_NSI_OPEN, 0, &c);
    memcpy(counter_result.reset, c.reset, sizeof(counter_result.reset));
}
static void counters_begin(struct accel_overlap_counter_phase *p, uint32_t ticks)
{
    struct a733_nsi_counter_call c;
    for (unsigned b = 0; b < A733_NSI_COUNTER_BANKS; ++b) {
        counters_note(p, counters_call(A733_NSI_BEGIN, b, &c));
        p->clear[b] = c.clear;
    }
    p->begin = now();
    counter_live_due = p->begin + ticks / 2u;
    counter_phase = p;
}
/* The sample at half the phase, with counting on. */
static void counters_poll(void)
{
    struct accel_overlap_counter_phase *p = counter_phase;
    if (!p || p->live_at || now() < counter_live_due) return;
    struct a733_nsi_counter_call c;
    p->live_at = now();
    for (unsigned b = 0; b < A733_NSI_COUNTER_BANKS; ++b) {
        counters_note(p, counters_call(A733_NSI_SAMPLE, b, &c));
        memcpy(p->live[b], c.value, sizeof(p->live[b]));
    }
}
static void counters_end(struct accel_overlap_counter_phase *p)
{
    struct a733_nsi_counter_call c;
    counter_phase = NULL;
    p->end = now();
    for (unsigned b = 0; b < A733_NSI_COUNTER_BANKS; ++b) {
        counters_note(p, counters_call(A733_NSI_END, b, &c));
        memcpy(p->value[b], c.value, sizeof(p->value[b]));
        memcpy(p->held[b], c.held, sizeof(p->held[b]));
    }
}
/* A phase with no accelerator job: the main task waits out the phase, sampling at its half. */
static void counters_idle(void)
{
    while (now() < atomic_load_explicit(&pipeline_end, memory_order_acquire)) {
        live();
        counters_poll();
        vTaskDelay(1);
    }
}
#endif

static void gpu_inputs(uint32_t rotation)
{
    for (unsigned i = 0; i < GPU_ELEMENTS; ++i) {
        const unsigned at = (i + rotation) % GPU_ELEMENTS;
        gpu_in[i] = UINT32_MAX - 3u * at;
        gpu_in[GPU_ELEMENTS + i] = 7u * at + 9u;
    }
}
static uint32_t gpu_output_differs(uint32_t rotation)
{
    for (unsigned i = 0; i < GPU_ELEMENTS; ++i)
        if (gpu_out[i] != 4u * ((i + rotation) % GPU_ELEMENTS) + 8u) return i + 1u;
    return 0;
}
static void gpu_submit(void)
{
    gpu_job.sequence = ++gpu_sequence;
    atomic_fetch_add_explicit(&gpu_outstanding, 1, memory_order_release);
    const TickType_t began = xTaskGetTickCount();
    int r;
    while ((r = gpu_queue_submit(context, &gpu_job)) == GPU_QUEUE_PARKED || r == GPU_QUEUE_BUSY) {
        if (ss_live() < 0 || xTaskGetTickCount() - began >= GPU_WAIT_TICKS) break;
        ss_progress(); vTaskDelay(1);
    }
    if (r != GPU_QUEUE_OK) fail(0x040, (uint32_t)r);
}
static void gpu_collect(uint32_t sequence)
{
    struct gpu_queue_record record;
    const int r = gpu_queue_wait(context, &record, GPU_QUEUE_WAIT_FOREVER);
    if (r != GPU_QUEUE_OK || record.result != GPU_QUEUE_OK || record.sequence != sequence) fail(0x041, (uint32_t)r);
    atomic_fetch_sub_explicit(&gpu_outstanding, 1, memory_order_release);
}
#ifndef ACCEL_OVERLAP_SWEEP
static void gpu_serial(struct accel_overlap_device *d)
{
    reset(&d->wake, gpu_histogram[0]);
    reset(&d->trip, gpu_histogram[1]);
    for (unsigned i = 0; i < GPU_SERIAL_JOBS; ++i) {
        const uint32_t rotation = gpu_sequence + 1u;
        gpu_inputs(rotation);
        const uint64_t began = now();
        gpu_submit();
        gpu_collect(gpu_sequence);
        const uint64_t end = now();
        const uint64_t irq = gpu_queue_last_irq_counter(context), resumed = gpu_queue_last_wake_counter(context);
        if (irq && resumed >= irq && resumed <= end) record(&d->wake, gpu_histogram[0], resumed - irq);
        else ++d->wake.missing;
        record(&d->trip, gpu_histogram[1], end - began);
        const uint32_t differs = gpu_output_differs(rotation);
        if (differs) { ++d->mismatches; fail(0x042, differs); }
        ++d->serial_jobs;
        if (!(i & 127)) live();
    }
    summarise(&d->wake, gpu_histogram[0]);
    summarise(&d->trip, gpu_histogram[1]);
}
#endif
/* Jobs in flight may not share an output range (drivers/gpu/pvr_compute_fast.c, conflicts): each of the GPU_QUEUE_DEPTH jobs
 * in flight writes its own slot of the output lease, a whole number of cache lines apart. */
static void gpu_slot(unsigned slot)
{
    gpu_job.binding[0].address = slot * GPU_SLOT_BYTES;
    gpu_job.binding[0].sync_offset = slot * GPU_SLOT_BYTES;
#ifdef ACCEL_OVERLAP_LARGE
    /* One cache line per pipelined job: the conflict check sees four distinct slots, and the queue maintains 64 bytes. The
     * GPU MMU confines the program to the lease, not to the range, so the job still writes the whole slot. */
    gpu_job.binding[0].sync_bytes = 64u;
#endif
}
/* Progress at most every 100 ms of the pipelined segment, whatever the job length. */
static uint64_t last_live;
static void live_paced(void)
{
    const uint64_t t = now();
    if (t - last_live >= 2400000u) { live(); last_live = t; }
}
#ifdef ACCEL_OVERLAP_LARGE
static struct accel_overlap_large_phase *large_phase;
/* After the pipelined second, each slot runs a serial job with a one-line sync range (plain), then one with the whole slot
 * (synced), whose output is compared. The synced job's round trip less the plain one's is the slot's output maintenance. */
static void gpu_slot_jobs(uint32_t rotation)
{
    struct accel_overlap_large_phase *l = large_phase;
    const volatile uint32_t *base = gpu_out;
    for (unsigned slot = 0; slot < GPU_QUEUE_DEPTH; ++slot) {
        uint64_t trip[2];
        for (unsigned synced = 0; synced < 2; ++synced) {
            gpu_slot(slot);
            if (synced) gpu_job.binding[0].sync_bytes = GPU_SLOT_BYTES;
            const uint64_t t = now();
            gpu_submit();
            gpu_collect(gpu_sequence);
            trip[synced] = now() - t;
        }
        l->plain_ticks_sum += trip[0]; if (trip[0] > l->plain_ticks_max) l->plain_ticks_max = trip[0];
        l->synced_ticks_sum += trip[1]; if (trip[1] > l->synced_ticks_max) l->synced_ticks_max = trip[1];
        ++l->slot_jobs;
        gpu_out = base + slot * (GPU_SLOT_BYTES / 4u);
        const uint32_t differs = gpu_output_differs(rotation);
        gpu_out = base;
        if (differs) { ++l->run.gpu.mismatches; fail(0x044, slot << 24 | (differs & 0xffffffu)); }
        live();
    }
    gpu_slot(0);
}
#endif
static void gpu_pipelined(struct accel_overlap_device *d)
{
    const uint32_t rotation = gpu_sequence + 1u;
    gpu_inputs(rotation);
#ifdef ACCEL_OVERLAP_LARGE
    gpu_job.binding[1].sync_bytes = GPU_ELEMENTS * 8u;
#endif
    last_live = now();
    d->depth = GPU_QUEUE_DEPTH;
    const uint32_t first = gpu_sequence + 1u;
    uint32_t submitted = 0, collected = 0;
    const uint64_t began = now();
    for (;;) {
        const int more = now() < atomic_load_explicit(&pipeline_end, memory_order_acquire);
        while (more && submitted - collected < GPU_QUEUE_DEPTH) {
            gpu_slot(submitted % GPU_QUEUE_DEPTH); gpu_submit(); ++submitted;
#ifdef ACCEL_OVERLAP_LARGE
            gpu_job.binding[1].sync_bytes = 0;  /* the first job cleaned the rewritten inputs; the others leave them alone */
#endif
        }
        if (collected == submitted) break;
        gpu_collect(first + collected);
        ++collected;
        live_paced();
        counters_poll();
    }
    d->pipelined_ticks = now() - began;
    d->pipelined_jobs = collected;
#ifdef ACCEL_OVERLAP_LARGE
    gpu_slot_jobs(rotation);
    return;
#endif
    gpu_slot(0);
    /* The last job of every slot wrote the same sums. */
    const volatile uint32_t *base = gpu_out;
    for (unsigned slot = 0; slot < GPU_QUEUE_DEPTH && slot < collected; ++slot) {
        gpu_out = base + slot * (GPU_SLOT_BYTES / 4u);
        const uint32_t differs = gpu_output_differs(rotation);
        gpu_out = base;
        if (differs) { ++d->mismatches; fail(0x043, slot << 8 | differs); }
    }
}
#ifndef ACCEL_OVERLAP_SWEEP
/* One close and reopen of the GPU: firmware stop and power off (PLL_GPU0 off), then power on and firmware start, while the
 * NPU producer keeps jobs in flight. A checked serial job follows each reopen. */
static void gpu_cycle(void)
{
    uint64_t t = now();
    int r = gpu_queue_close(context, 4000);
    if (r != GPU_QUEUE_OK) fail(0x050, (uint32_t)r);
    uint64_t close_ticks = now() - t;
    if (close_ticks > result.pll.close_ticks_max) result.pll.close_ticks_max = close_ticks;
    live();
    t = now();
    r = gpu_queue_open(context, 4000);
    if (r != GPU_QUEUE_OK) fail(0x051, (uint32_t)r);
    const uint64_t open_ticks = now() - t;
    if (open_ticks > result.pll.open_ticks_max) result.pll.open_ticks_max = open_ticks;
    live();
    const uint32_t rotation = gpu_sequence + 1u;
    gpu_inputs(rotation);
    gpu_submit();
    gpu_collect(gpu_sequence);
    if (gpu_output_differs(rotation)) { ++result.pll.cycle_mismatches; fail(0x052, result.pll.cycles); }
    ++result.pll.cycles;
    /* PLL_NPU kept its word and its lock through the GPU PLL's stop and start (EL1 copies the watch at each NPU completion). */
    const struct ss_npu_queue_pll_watch w = queue->pll;
    if (w.unlocked || w.first_site) fail(0x053, w.first_site << 28 | (w.first_word & 0x0fffffffu));
}
#endif

static int at_top(void)
{
    char text[256];
    const char prefix[] = "CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 ";
    return ss_clock_info(text, sizeof(text)) >= (int)sizeof(prefix) - 1 && !memcmp(text, prefix, sizeof(prefix) - 1) &&
           strstr(text, " dsu_mhz=1274 ") != NULL;
}
static void clock_words(void)
{
    char line[192]; unsigned long v[7] = {0};
    static const char *const keys[] = {"cpu_l_mhz=", "cpu_b_mhz=", "cpu_l_mv=", "cpu_b_mv=", "transitions=", "dsu_mhz=", "core="};
    if (ss_clock_info(line, sizeof(line)) > 0)
        for (unsigned i = 0; i < 7; ++i) {
            const char *at = strstr(line, keys[i]);
            if (!at) continue;
            unsigned long x = 0;
            for (at += strlen(keys[i]); *at >= '0' && *at <= '9'; ++at) x = x * 10 + (unsigned long)(*at - '0');
            v[i] = x;
        }
    result.clock[0] = v[0] | v[1] << 16 | v[6] << 32 | v[4] << 40;
    result.clock[1] = v[2] | v[3] << 16 | v[5] << 32;
}

void accel_overlap_init(void)
{
    const struct ss_grant *g = ss_grant();
    if (!g || g->main_core != MAIN_CORE || g->npu_core != NPU_CORE || !(g->worker_cores >> NPU_CORE & 1u)) ss_fail(0x060);
    for (unsigned w = 0; w < STREAM_WORKERS; ++w)
        if (!(g->worker_cores >> stream_core[w] & 1u)) ss_fail(0x061);
    if (g->regions[ACCEL_OVERLAP_REGION_STREAM].bytes < (size_t)STREAM_AREAS * STREAM_BYTES) ss_fail(0x062);
    npu_task = ss_task_create_static_affinity_set(npu_producer, "npu-producer", sizeof(npu_stack) / sizeof(npu_stack[0]),
        NULL, 3, npu_stack, &npu_tcb, 1ul << NPU_CORE);
    if (!npu_task) ss_fail(0x063);
    for (unsigned w = 0; w < STREAM_WORKERS; ++w) {
        stream_task[w] = ss_task_create_static_affinity_set(stream_worker, "stream", 1024, (void *)(uintptr_t)w, 2,
            stream_stack[w], &stream_tcb[w], 1ul << stream_core[w]);
        if (!stream_task[w]) ss_fail(0x064);
    }
#ifdef ACCEL_OVERLAP_SWEEP
    if (!(g->worker_cores >> CHASE_CORE & 1u) || g->regions[ACCEL_OVERLAP_REGION_CHASE].bytes < (16u << 20)) ss_fail(0x065);
    chase_task = ss_task_create_static_affinity_set(chase_task_entry, "chase", sizeof(chase_stack) / sizeof(chase_stack[0]),
        NULL, 2, chase_stack, &chase_tcb, 1ul << CHASE_CORE);
    if (!chase_task) ss_fail(0x066);
#endif
}

void accel_overlap_main(const struct ss_grant *grant)
{
    main_task = xTaskGetCurrentTaskHandle();
    if (!(grant->devices & SS_DEVICE_GPU) || grant->region_count != 6 ||
        grant->regions[ACCEL_OVERLAP_REGION_GPU_INPUT].bytes < GPU_ELEMENTS * 8u ||
        grant->regions[ACCEL_OVERLAP_REGION_GPU_OUTPUT].bytes < GPU_QUEUE_DEPTH * GPU_SLOT_BYTES) fail(0x070, grant->region_count);
    gpu_in = grant->regions[ACCEL_OVERLAP_REGION_GPU_INPUT].base;
    gpu_out = grant->regions[ACCEL_OVERLAP_REGION_GPU_OUTPUT].base;
    uint64_t began = now();
    while (!ss_clock_ready() || ss_throttled()) {
        live();
        if (now() - began > 240000000u) fail(0x071, (uint32_t)ss_clock_ready());
        vTaskDelay(1);
    }
    result.initial_top = (uint32_t)at_top();
    if (!result.initial_top) fail(0x072, 0);
    clock_words();
    const struct gpu_queue_context_config config = {.job_us = 200000, .program = &gpu_queue_program, .lease_count = 2,
        .lease = {{.region = ACCEL_OVERLAP_REGION_GPU_OUTPUT}, {.region = ACCEL_OVERLAP_REGION_GPU_INPUT}}};
    struct gpu_queue_lease_info info[2];
    if (gpu_queue_context_create(&config, 4000, &context, info) != GPU_QUEUE_OK) fail(0x073, 0);
    if (info[0].address != (uintptr_t)gpu_out || info[1].address != (uintptr_t)gpu_in) fail(0x074, 0);
    gpu_job = (struct gpu_queue_job){.bindings = 2, .argument_bytes = sizeof(uint32_t), .groups = {GPU_GROUPS, 1, 1},
        .binding = {{.lease = 0, .sync_bytes = GPU_ELEMENTS * 4u}, {.lease = 1, .sync_bytes = GPU_ELEMENTS * 8u}}};
    for (unsigned i = 0; i < sizeof(uint32_t); ++i) gpu_job.arguments[i] = (unsigned char)(GPU_ELEMENTS >> (8u * i));
    began = now();
    while (!atomic_load_explicit(&npu_ready, memory_order_acquire)) {
        live();
        if (now() - began > 240000000u) fail(0x075, 0);
        (void)ulTaskNotifyTake(pdTRUE, 10);
    }
    result.stream_workers = STREAM_WORKERS;
    result.stream_bytes_per_worker = STREAM_BYTES;
#ifdef ACCEL_OVERLAP_SWEEP
    began = now();
    while (!atomic_load_explicit(&chase_ready, memory_order_acquire)) {
        live();
        if (now() - began > 240000000u) fail(0x078, 0);
        (void)ulTaskNotifyTake(pdTRUE, 10);
    }
#if defined(ACCEL_OVERLAP_COUNTERS)
    counters_open();
    for (phase_index = 0; phase_index < ACCEL_OVERLAP_COUNTER_PHASES && counters_ok; ++phase_index) {
#elif defined(ACCEL_OVERLAP_WIDTH)
    for (phase_index = 0; phase_index < 1u; ++phase_index) {   /* the GPU-alone phase only */
#else
    for (phase_index = 0; phase_index < SWEEP_PHASES; ++phase_index) {
#endif
#ifdef ACCEL_OVERLAP_LARGE
        struct accel_overlap_large_phase *l = &large_result.phase[phase_index];
        struct accel_overlap_phase *p = &l->run;
        struct accel_overlap_sweep *c = &l->sweep;
        large_phase = l;
#ifdef ACCEL_OVERLAP_COUNTERS
        l->stream_mode = counter_streams[phase_index];
        const uint32_t devices = counter_devices[phase_index], mode = l->stream_mode;
        const uint32_t phase_ticks = counter_ticks[phase_index];
        struct accel_overlap_counter_phase *k = &counter_result.phase[phase_index];
#else
        l->stream_mode = large_streams[phase_index];
        const uint32_t devices = large_devices[phase_index], mode = l->stream_mode;
        const uint32_t phase_ticks = PIPELINE_TICKS;
#endif
#else
        struct accel_overlap_phase *p = &result.phase[phase_index];
        struct accel_overlap_sweep *c = &sweep_result.sweep[phase_index];
        const uint32_t devices = ACCEL_OVERLAP_GPU | ACCEL_OVERLAP_NPU, mode = phase_streams[phase_index];
        const uint32_t phase_ticks = PIPELINE_TICKS;
#endif
        current = p;
        c->stream_mask = mode & 0xffu;
        p->devices = devices | (c->stream_mask ? ACCEL_OVERLAP_STREAM : 0u);
        const int gpu = devices & ACCEL_OVERLAP_GPU, npu = devices & ACCEL_OVERLAP_NPU;
        const uint64_t start = now();
#ifdef ACCEL_OVERLAP_COUNTERS
        counters_begin(k, phase_ticks);
#endif
        chase_begin(c);
        if (c->stream_mask) stream_start(mode);
        atomic_store_explicit(&npu_stop, 1, memory_order_release);
        atomic_store_explicit(&pipeline_end, now() + phase_ticks, memory_order_release);
        if (npu) npu_begin(NPU_PIPELINED);
        if (gpu) gpu_pipelined(&p->gpu);
        if (npu) npu_end(NPU_PIPELINED);
#ifdef ACCEL_OVERLAP_COUNTERS
        if (!gpu && !npu) counters_idle();
#endif
        if (c->stream_mask) { stream_stop(); p->stream_ticks = now() - start; }
        chase_end();
#ifdef ACCEL_OVERLAP_COUNTERS
        counters_end(k);
#endif
        p->ticks = now() - start;
        p->throttled = (uint32_t)ss_throttled();
        if (p->throttled) fail(0x076, p->throttled);
        live();
    }
#else
    for (phase_index = 0; phase_index < ACCEL_OVERLAP_PHASES; ++phase_index) {
        struct accel_overlap_phase *p = &result.phase[phase_index];
        current = p;
        p->devices = phase_devices[phase_index];
        const int gpu = p->devices & ACCEL_OVERLAP_GPU, npu = p->devices & ACCEL_OVERLAP_NPU;
        const uint64_t start = now();
        if (p->devices & ACCEL_OVERLAP_STREAM) stream_start(ALL_STREAMS);
        if (p->devices & ACCEL_OVERLAP_PLL_CYCLES) {
            atomic_store_explicit(&npu_stop, 0, memory_order_release);
            npu_begin(NPU_CONTINUOUS);
            for (unsigned c = 0; c < PLL_CYCLES; ++c) gpu_cycle();
            atomic_store_explicit(&npu_stop, 1, memory_order_release);
            npu_end(NPU_CONTINUOUS);
            /* PLL_NPU at the completions after the last cycle. The counts of writes and reads go to the result, which host.py
             * compares with the hypothesis. */
            const struct ss_npu_queue_pll_watch w = queue->pll;
            if (w.unlocked || w.first_site) fail(0x054, w.first_site << 28 | (w.first_word & 0x0fffffffu));
        } else {
            /* Serial segment: the NPU repeats jobs until it has its count and the GPU has finished. */
            atomic_store_explicit(&npu_stop, gpu ? 0u : 1u, memory_order_release);
            if (npu) npu_begin(NPU_SERIAL);
            if (gpu) gpu_serial(&p->gpu);
            atomic_store_explicit(&npu_stop, 1, memory_order_release);
            if (npu) npu_end(NPU_SERIAL);
            /* Pipelined segment of one second. */
            atomic_store_explicit(&pipeline_end, now() + PIPELINE_TICKS, memory_order_release);
            if (npu) npu_begin(NPU_PIPELINED);
            if (gpu) gpu_pipelined(&p->gpu);
            if (npu) npu_end(NPU_PIPELINED);
        }
        if (p->devices & ACCEL_OVERLAP_STREAM) { stream_stop(); p->stream_ticks = now() - start; }
        p->ticks = now() - start;
        p->throttled = (uint32_t)ss_throttled();
        if (p->throttled) fail(0x076, p->throttled);
        live();
    }
#endif
    phase_index = SWEEP_PHASES;
#ifdef ACCEL_OVERLAP_COUNTERS
    {
        struct a733_nsi_counter_call c;
        if (counter_result.open_status == A733_NSI_C_OK) counter_result.close_status = counters_call(A733_NSI_CLOSE, 0, &c);
    }
#endif
    npu_begin(NPU_EXIT);
    for (unsigned w = 0; w < STREAM_WORKERS; ++w) xTaskNotifyGive(stream_task[w]);
#ifdef ACCEL_OVERLAP_SWEEP
    xTaskNotifyGive(chase_task);
#endif
    npu_end(NPU_EXIT);
    if (gpu_queue_close(context, 4000) != GPU_QUEUE_OK) fail(0x077, 0);
    result.final_top = (uint32_t)at_top();
    result.magic = ACCEL_OVERLAP_MAGIC; result.version = ACCEL_OVERLAP_VERSION; result.bytes = sizeof(result);
    result.phases = ACCEL_OVERLAP_PHASES;
#ifdef ACCEL_OVERLAP_LARGE
    struct accel_overlap_large_result *r = &large_result;
#if defined(ACCEL_OVERLAP_COUNTERS)
    r->magic = ACCEL_OVERLAP_MAGIC; r->version = ACCEL_OVERLAP_COUNTERS_VERSION; r->bytes = sizeof(counter_result);
#elif defined(ACCEL_OVERLAP_WIDTH)
    r->magic = ACCEL_OVERLAP_MAGIC; r->version = ACCEL_OVERLAP_WIDTH_VERSION; r->bytes = sizeof(*r);
#else
    r->magic = ACCEL_OVERLAP_MAGIC; r->version = ACCEL_OVERLAP_LARGE_VERSION; r->bytes = sizeof(*r);
#endif
    r->phases = ACCEL_OVERLAP_LARGE_PHASES;
    memcpy(r->clock, result.clock, sizeof(r->clock));
    r->initial_top = result.initial_top; r->final_top = result.final_top;
    r->stream_workers = result.stream_workers; r->stream_bytes_per_worker = result.stream_bytes_per_worker;
    r->pll = result.pll;
    r->gpu_elements = GPU_ELEMENTS; r->npu_tensor_bytes = (uint32_t)npu_reference_bytes;
#ifdef ACCEL_OVERLAP_COUNTERS
    r->phases = ACCEL_OVERLAP_COUNTER_PHASES;
    counter_result.large = *r;
    ss_finish(&counter_result, sizeof(counter_result));
#else
    ss_finish(r, sizeof(*r));
#endif
#elif defined(ACCEL_OVERLAP_SWEEP)
    result.version = ACCEL_OVERLAP_SWEEP_VERSION;
    sweep_result.base = result;
    sweep_result.base.bytes = sizeof(sweep_result);
    ss_finish(&sweep_result, sizeof(sweep_result));
#else
    ss_finish(&result, sizeof(result));
#endif
}
void platform_puts(const char *text) { (void)text; ss_fail(0x8f0); }
