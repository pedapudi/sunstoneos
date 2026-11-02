/* SPDX-License-Identifier: GPL-2.0-only */
/* CPU memory latency under device load, with the interconnect priority of the CPU ports as the variable
 * (docs/designs/dram-bandwidth.md, "CPU memory latency under device load"). The worker tasks run at EL1.
 * For each of three priority settings the controller runs three probes in turn while the image's load runs:
 * a pointer chase on an A76 core, a pointer chase on an A55 core, and a 10 kHz control-loop proxy on the A76 core.
 * The load is selected at build time: none, streaming reads on the other worker cores, an NPU job stream or
 * 4 KiB DMA0 copies, the last two driven by the controller on core 6. */
#include "dram_qos-app.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>
#include <string.h>
#include "result.h"
#include "portable.h"
#include "sunstone_dma.h"
#ifndef DQ_LOAD
#define DQ_LOAD 0
#endif
_Static_assert(sizeof(struct dq_result) <= 16384, "output grant");
static struct dq_result result;
static StaticTask_t state[DQ_CORES];
static StackType_t stack[DQ_CORES][2048];
static TaskHandle_t workers[DQ_CORES], controller;
static uint8_t *buffer;
static uint32_t perm[DQ_NODES];
static struct {
    uint32_t kind, probe_core, streamers, setting, stream;
    atomic_uint generation, finished, stop, go, ready;
    uint64_t stream_bytes[DQ_CORES];
    struct dq_probe probe;
    uint32_t port_words[2][4], npu_words[2], error;
} job;
#define counter dq_counter
static void dq_error(uint32_t code) { job.error = code; for (;;) dq_relax(); }
static uint8_t *chase_base(void) { return buffer; }
static uint8_t *slot(unsigned core) { return buffer + DQ_CHASE_BYTES + (size_t)core * DQ_SLOT_BYTES; }
/* Priority settings: ports 19 and 20 are cpu0 and cpu1 of the sun60iw2 port enum. */
static void apply_setting(unsigned s)
{
    static uint32_t original[2][2];
    static int saved;
    for (unsigned c = 0; c < 2; ++c) {
        const uint32_t b = DQ_PORT(19 + c);
        if (!saved) { original[c][0] = dq_reg_read(b + 0xc); original[c][1] = dq_reg_read(b + 0x14); }
        uint32_t qos = original[c][0], pri = original[c][1];
        if (s >= 1) pri = (pri & ~0xfu) | (3u << 2) | 3u;
        if (s >= 2) qos |= 1u << 16;
        dq_reg_write(b + 0x14, pri);
        dq_dmb();
        dq_reg_write(b + 0xc, qos);
        dq_dmb();
        job.port_words[c][0] = dq_reg_read(b + 0xc);
        job.port_words[c][1] = dq_reg_read(b + 0x14);
        job.port_words[c][2] = dq_reg_read(b + 0x10);
        job.port_words[c][3] = dq_reg_read(b + 0x18);
    }
    saved = 1;
    job.npu_words[0] = dq_reg_read(DQ_PORT(18) + 0xc);
    job.npu_words[1] = dq_reg_read(DQ_PORT(18) + 0x14);
}
static void restore_setting(void) { apply_setting(0); }
static void note(struct dq_probe *p, uint32_t cycles)
{
    p->hist[dq_bin(cycles)]++;
    if (cycles > p->max_cycles) p->max_cycles = cycles;
    p->samples++;
}
/* Pointer chase: timed dependent loads around the random cycle until the window ends. */
static void chase(void)
{
    struct dq_probe *p = &job.probe;
    void *node = chase_base();
    uint32_t floor = 0xffffffffu;
    void *warm = chase_base();
    for (unsigned i = 0; i < 64; ++i) { uint32_t c = dq_timed_load(&warm); (void)c; }
    for (unsigned i = 0; i < 64; ++i) { void *same = chase_base(); uint32_t c = dq_timed_load(&same); if (c < floor) floor = c; }
    p->overhead = floor;                        /* an L1 hit: the cost of the timing sequence */
    const uint64_t start = counter(), end = start + DQ_WINDOW_TICKS;
    for (;;) {
        for (unsigned i = 0; i < 1024; ++i) note(p, dq_timed_load(&node));
        if (counter() >= end) break;
    }
    p->ticks = counter() - start;
}
/* Control-loop proxy: every 100 microseconds a fixed routine of dependent loads, timed in cycles. */
static void control(void)
{
    struct dq_probe *p = &job.probe;
    const uint64_t period = dq_counter_hz() / DQ_CONTROL_HZ, start = counter() + period;
    uint32_t index = 12345;
    for (uint64_t k = 0; ; ++k) {
        const uint64_t release = start + k * period;
        if (release >= start + DQ_CONTROL_TICKS) break;
        while (counter() < release) dq_relax();
        const uint64_t began = counter();
        if (began - release > p->late_max) p->late_max = (uint32_t)(began - release);
        void *node = chase_base() + (size_t)perm[index % DQ_NODES] * 64u;
        index = index * 1103515245u + 12345u;
        const uint32_t c0 = dq_cycles();
        for (unsigned i = 0; i < DQ_CONTROL_LOADS; ++i) node = *(void **)node;
        const uint32_t cycles = dq_cycles() - c0;
        __asm__ volatile("" :: "r"(node) : "memory");
        note(p, cycles);
        if (counter() > release + period) p->misses++;
    }
    p->ticks = counter() - start;
}
void dram_qos_worker(void *arg)
{
    const unsigned core = (unsigned)(uintptr_t)arg;
    unsigned seen = 0;
    dq_cycles_enable();
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const unsigned generation = atomic_load_explicit(&job.generation, memory_order_acquire);
        if (generation == seen) continue;
        seen = generation;
        const int probing = core == job.probe_core, streaming = (job.streamers >> core & 1u) != 0;
        if (!probing && !streaming) continue;
        if (portGET_CORE_ID() != core) dq_error(0xd710 | core);
        if (job.kind == 3) {                          /* priority job */
            if (job.setting == 99) restore_setting(); else apply_setting(job.setting);
        } else if (probing) {
            /* The probe starts when every streamer is reading. */
            const unsigned wanted = (unsigned)__builtin_popcount(job.streamers);
            for (uint64_t limit = counter() + 2400000; atomic_load_explicit(&job.ready, memory_order_acquire) != wanted &&
                 counter() < limit;) dq_relax();
            if (job.kind == 1) chase(); else control();
            atomic_store_explicit(&job.stop, 1, memory_order_release);
        } else {
            uint64_t bytes = 0;
            const uint64_t *p = (const uint64_t *)slot(core);
            atomic_fetch_add_explicit(&job.ready, 1, memory_order_acq_rel);
            while (!atomic_load_explicit(&job.stop, memory_order_acquire)) {
                (void)dq_read(p, DQ_SLOT_BYTES);
                bytes += DQ_SLOT_BYTES;
            }
            job.stream_bytes[core] = bytes;
        }
        atomic_fetch_add_explicit(&job.finished, 1, memory_order_acq_rel);
        xTaskNotifyGive(controller);
    }
}
void dram_qos_init(void)
{
    const struct ss_grant *g = ss_grant();
    if (!g || g->main_core != 6 || g->worker_cores != 0xbf) ss_fail(0xd701);
    for (unsigned core = 0; core < DQ_CORES; ++core) {
        if (core == 6) continue;
        workers[core] = xTaskCreateStaticAffinitySet(dram_qos_worker, "qos", 2048, (void *)(uintptr_t)core, 2,
            stack[core], &state[core], 1u << core);
        if (!workers[core]) ss_fail(0xd702);
    }
}
/* NPU and DMA drivers on the controller's core. They are polled while a probe runs. */
#if DQ_LOAD == 2
static unsigned npu_model;
static uint8_t npu_in[SS_NPU_MAX_TENSOR_BYTES], npu_out[SS_NPU_MAX_TENSOR_BYTES];
static size_t npu_in_bytes, npu_out_bytes;
static struct { uint64_t started; } npu_state;
#elif DQ_LOAD == 3
static struct ss_dma_buffers dma_buffers;
static unsigned dma_phase;
#endif
/* One step of the load; returns after at most one non-blocking call. */
static void load_step(struct dq_setting *s)
{
#if DQ_LOAD == 2
    if (!npu_state.started) npu_state.started = counter();
    int r = ss_npu_run(npu_model, npu_in, npu_in_bytes, npu_out, npu_out_bytes, 100000);
    if (r == 1) {
        const uint64_t t = counter() - npu_state.started;
        npu_state.started = 0;
        s->npu_jobs++;
        s->npu_ticks_sum += t;
        if (t > s->npu_ticks_max) s->npu_ticks_max = t;
        if (!s->npu_ticks_min || t < s->npu_ticks_min) s->npu_ticks_min = t;
    } else if (r < 0) {
        s->npu_failed++;
    }
#elif DQ_LOAD == 3
    int r = 0;
    switch (dma_phase) {
    case 0: r = ss_dma_submit(); if (r == 1) { dma_phase = 1; } break;
    case 1: r = ss_dma_complete(); if (r == 1) dma_phase = 2; break;
    case 2: r = ss_dma_reap(); if (r == 1) { dma_phase = 0; s->dma_copies++; } break;
    }
    if (r < 0) s->dma_failed++;
#else
    (void)s;
#endif
}
static void run(unsigned kind, unsigned probe_core, uint32_t streamers, unsigned setting, struct dq_setting *s)
{
    job.kind = kind;
    job.probe_core = probe_core;
    job.streamers = streamers;
    job.setting = setting;
    memset(&job.probe, 0, sizeof job.probe);
    memset(job.stream_bytes, 0, sizeof job.stream_bytes);
    atomic_store(&job.finished, 0);
    atomic_store(&job.stop, 0);
    atomic_store(&job.ready, 0);
    atomic_fetch_add_explicit(&job.generation, 1, memory_order_release);
    uint32_t mask = streamers | (1u << probe_core);
    unsigned participants = (unsigned)__builtin_popcount(mask);
    for (unsigned core = 0; core < DQ_CORES; ++core)
        if (mask >> core & 1u) xTaskNotifyGive(workers[core]);
    const uint64_t deadline = counter() + 6 * 24000000u;
    uint64_t waited = 0;
    while (atomic_load_explicit(&job.finished, memory_order_acquire) != participants) {
        if (job.error) ss_fail(job.error);
        if (counter() > deadline) ss_fail(0xd720 | kind);
        if (kind != 3 && (DQ_LOAD == 2 || DQ_LOAD == 3)) { load_step(s); ss_progress(); }
        else (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        (void)waited;
    }
    if (ss_live() < 0) ss_fail(0xd730);
    ss_progress();
}
void dram_qos_main(const struct ss_grant *g)
{
    controller = xTaskGetCurrentTaskHandle();
    buffer = g->regions[DRAM_QOS_REGION_BUFFER].base;
    if (g->regions[DRAM_QOS_REGION_BUFFER].bytes < DQ_BUFFER_BYTES) ss_fail(0xd703);
    while (!ss_clock_ready() || ss_throttled()) {
        if (ss_live() < 0) ss_fail(0xd704);
        ss_progress();
        vTaskDelay(1);
    }
    /* A random single cycle through all nodes (Sattolo's algorithm), written as next pointers. */
    for (uint32_t i = 0; i < DQ_NODES; ++i) perm[i] = i;
    uint64_t x = 0x9e3779b97f4a7c15ull;
    for (uint32_t i = DQ_NODES - 1; i > 0; --i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        const uint32_t j = (uint32_t)(x % i);
        const uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    for (uint32_t i = 0; i < DQ_NODES; ++i)
        *(uint64_t *)(buffer + (size_t)perm[i] * 64u) = (uint64_t)(uintptr_t)(buffer + (size_t)perm[(i + 1) % DQ_NODES] * 64u);
    memset(buffer + DQ_CHASE_BYTES, 0x5a, (size_t)DQ_CORES * DQ_SLOT_BYTES);
    memcpy(result.magic, DQ_MAGIC, 8);
    result.counter_hz = dq_counter_hz();
    result.buffer_base = (uint64_t)(uintptr_t)buffer;
    result.load = DQ_LOAD;
    struct ss_thermal_status thermal;
    ss_thermal_status(&thermal);
    for (unsigned i = 0; i < 5; ++i) result.millicelsius_before[i] = thermal.millicelsius[i];
    result.readiness = (ss_clock_ready() ? 1u : 0u) | (!ss_throttled() ? 2u : 0u);
    if (ss_clock_info(result.clock_before, sizeof(result.clock_before)) <= 0) ss_fail(0xd705);
#if DQ_LOAD == 2
    {
        unsigned index;
        struct ss_npu_model m;
        if (ss_npu_model_find("shufflenet-v2-published", &index) != 1 || ss_npu_model_info(index, &m) != 1) ss_fail(0xd706);
        if (m.input_bytes > sizeof npu_in || m.output_bytes > sizeof npu_out) ss_fail(0xd707);
        npu_model = index; npu_in_bytes = m.input_bytes; npu_out_bytes = m.output_bytes;
        memset(npu_in, 0x40, npu_in_bytes);
    }
#elif DQ_LOAD == 3
    for (unsigned spins = 0;; ++spins) {
        int r = ss_dma_open(&dma_buffers);
        if (r == 1) break;
        if (r < 0 || spins > 2000000) ss_fail(0xd708);
        ss_progress();
    }
#endif
    ss_activity_begin();
    static const unsigned cores_a76 = 7, cores_a55 = 0;
    for (unsigned setting = 0; setting < DQ_SETTINGS; ++setting) {
        struct dq_setting *s = &result.setting[setting];
        run(3, cores_a76, 0, setting, s);
        memcpy(s->port_words, job.port_words, sizeof s->port_words);
        memcpy(s->npu_port_words, job.npu_words, sizeof s->npu_port_words);
        const uint32_t all = 0xbfu;
        for (unsigned probe = 0; probe < DQ_PROBES; ++probe) {
            const unsigned core = probe == 1 ? cores_a55 : cores_a76;
            const uint32_t streamers = DQ_LOAD == 1 ? (all & ~(1u << core)) : 0u;
            const uint64_t t0 = counter();
            run(probe == 2 ? 2 : 1, core, streamers, setting, s);
            s->probe[probe] = job.probe;
            for (unsigned c = 0; c < DQ_CORES; ++c) s->probe[probe].stream_bytes += job.stream_bytes[c];
            if (DQ_LOAD == 2) s->npu_ticks_window += counter() - t0;
            if (DQ_LOAD == 3) s->dma_ticks_window += counter() - t0;
        }
    }
    run(3, cores_a76, 0, 99, &result.setting[0]);        /* restore the inherited words */
    thermal = (struct ss_thermal_status){0};
    ss_thermal_status(&thermal);
    for (unsigned i = 0; i < 5; ++i) result.millicelsius_after[i] = thermal.millicelsius[i];
    result.readiness |= (ss_clock_ready() ? 4u : 0u) | (!ss_throttled() ? 8u : 0u);
    ss_activity_end(1);
#if DQ_LOAD == 3
    for (unsigned spins = 0; spins < 2000000; ++spins) { if (ss_dma_close() == 1) break; ss_progress(); }
#elif DQ_LOAD == 2
    for (unsigned spins = 0; spins < 2000000; ++spins) { if (ss_npu_close() >= 1) break; ss_progress(); }
#endif
    ss_finish(&result, sizeof(result));
}
