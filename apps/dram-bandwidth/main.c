/* SPDX-License-Identifier: GPL-2.0-only */
/* Sustained DRAM read bandwidth by core set (docs/designs/dram-bandwidth.md).
 * Eight pinned worker tasks fill a 40 MiB buffer, each its own 5 MiB slice,
 * then stream it with 128-byte loads. Five core sets run in turn: one A55, one
 * A76, both A76, six A55 and all eight. In every pass each participating core
 * reads a different slice from the previous pass, so no core re-reads what its
 * own caches hold. The result carries the elapsed counter ticks of each pass
 * (first start to last finish) and the digests that prove the data was read. */
#include "dram_bandwidth-app.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>
#include <string.h>
#include "result.h"
_Static_assert(configNUMBER_OF_CORES == BW_CORES, "eight physical cores required");
_Static_assert(sizeof(struct bw_result) <= 16384, "output grant");
static struct bw_result result;
static StaticTask_t state[BW_CORES];
static StackType_t stack[BW_CORES][1024];
static TaskHandle_t workers[BW_CORES], controller;
static uint64_t *buffer;
/* Job description, written by the controller before it notifies the workers. */
static struct {
    uint32_t mask, cores, pass, mode;       /* mode 0 fill, 1 read */
    atomic_uint arrived, go, finished;
    uint64_t start[BW_CORES], finish[BW_CORES], digest[BW_CORES];
} job;
static uint64_t counter(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0,cntpct_el0" : "=r"(v) :: "memory");
    return v;
}
static unsigned rank_of(uint32_t mask, unsigned core)
{
    return (unsigned)__builtin_popcount(mask & ((1u << core) - 1u));
}
static void worker(void *arg)
{
    const unsigned core = (unsigned)(uintptr_t)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (portGET_CORE_ID() != core) ss_fail(0xb710 | core);
        const unsigned rank = rank_of(job.mask, core);
        /* A reading core takes slice (rank + pass) modulo eight; the fill writes slice `core`. */
        const unsigned slice = job.mode == 0 ? core : (rank + job.pass) % BW_SLICES;
        uint64_t *p = buffer + (size_t)slice * (BW_SLICE_BYTES / 8);
        /* Barrier: the last core to arrive opens the gate, so all start together. */
        if (atomic_fetch_add_explicit(&job.arrived, 1, memory_order_acq_rel) + 1 == job.cores)
            atomic_store_explicit(&job.go, 1, memory_order_release);
        while (!atomic_load_explicit(&job.go, memory_order_acquire)) __asm__ volatile("yield");
        const uint64_t t0 = counter();
        uint64_t digest = 0;
        if (job.mode == 0) {
            const uint64_t base = (uint64_t)slice * (BW_SLICE_BYTES / 8);
            for (size_t i = 0; i < BW_SLICE_BYTES / 8; ++i) {
                const uint64_t w = bw_word(base + i);
                p[i] = w;
                digest ^= w;
            }
            __asm__ volatile("dsb ish" ::: "memory");
        } else {
            digest = bw_read(p, BW_SLICE_BYTES);
        }
        const uint64_t t1 = counter();
        job.start[core] = t0;
        job.finish[core] = t1;
        job.digest[core] = digest;
        if (atomic_fetch_add_explicit(&job.finished, 1, memory_order_acq_rel) + 1 == job.cores)
            xTaskNotifyGive(controller);
    }
}
void dram_bandwidth_init(void)
{
    const struct ss_grant *g = ss_grant();
    if (!g || g->main_core != 6 || g->worker_cores != 0xbf) ss_fail(0xb701);
    for (unsigned core = 0; core < BW_CORES; ++core) {
        workers[core] = xTaskCreateStaticAffinitySet(worker, "bandwidth", 1024,
            (void *)(uintptr_t)core, 2, stack[core], &state[core], 1u << core);
        if (!workers[core]) ss_fail(0xb702);
    }
}
/* Runs one pass on the cores of `mask`; returns the elapsed ticks. */
static uint64_t run(uint32_t mask, unsigned mode, unsigned pass)
{
    job.mask = mask;
    job.cores = (uint32_t)__builtin_popcount(mask);
    job.mode = mode;
    job.pass = pass;
    atomic_store(&job.arrived, 0);
    atomic_store(&job.go, 0);
    atomic_store(&job.finished, 0);
    for (unsigned core = 0; core < BW_CORES; ++core)
        if (mask >> core & 1u) xTaskNotifyGive(workers[core]);
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) != 1) ss_fail(0xb720 | mode);
    uint64_t first = UINT64_MAX, last = 0;
    for (unsigned core = 0; core < BW_CORES; ++core) {
        if (!(mask >> core & 1u)) continue;
        if (job.start[core] < first) first = job.start[core];
        if (job.finish[core] > last) last = job.finish[core];
    }
    if (ss_live() < 0) ss_fail(0xb730);
    ss_progress();
    return last - first;
}
static uint64_t pass_digest(uint32_t mask)
{
    uint64_t x = 0;
    for (unsigned core = 0; core < BW_CORES; ++core)
        if (mask >> core & 1u) x ^= job.digest[core];
    return x;
}
void dram_bandwidth_main(const struct ss_grant *g)
{
    controller = xTaskGetCurrentTaskHandle();
    buffer = g->regions[DRAM_BANDWIDTH_REGION_BUFFER].base;
    if (g->regions[DRAM_BANDWIDTH_REGION_BUFFER].bytes < BW_BUFFER_BYTES) ss_fail(0xb703);
    while (!ss_clock_ready() || ss_throttled()) {
        if (ss_live() < 0) ss_fail(0xb704);
        ss_progress();
        vTaskDelay(1);
    }
    memcpy(result.magic, BW_MAGIC, 8);
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(result.counter_hz));
    result.slice_bytes = BW_SLICE_BYTES;
    result.slices = BW_SLICES;
    result.warm = BW_WARM;
    result.reps = BW_REPS;
    struct ss_thermal_status thermal;
    ss_thermal_status(&thermal);
    for (unsigned i = 0; i < 5; ++i) result.millicelsius_before[i] = thermal.millicelsius[i];
    result.readiness = (ss_clock_ready() ? 1u : 0u) | (!ss_throttled() ? 2u : 0u);
    if (ss_clock_info(result.clock_before, sizeof(result.clock_before)) <= 0) ss_fail(0xb705);
    ss_activity_begin();
    (void)run(0xffu, 0, 0);                 /* fill: every core writes its own slice */
    for (unsigned core = 0; core < BW_CORES; ++core) result.fill_digest[core] = job.digest[core];
    static const uint32_t masks[BW_CONFIGS] = BW_CONFIG_MASKS;
    for (unsigned c = 0; c < BW_CONFIGS; ++c) {
        struct bw_config *config = &result.config[c];
        config->mask = masks[c];
        config->cores = (uint32_t)__builtin_popcount(masks[c]);
        for (unsigned pass = 0; pass < BW_WARM + BW_REPS; ++pass) {
            const uint64_t ticks = run(masks[c], 1, pass + 1);
            if (pass >= BW_WARM) {
                config->samples[pass - BW_WARM].ticks = ticks;
                config->samples[pass - BW_WARM].digest = pass_digest(masks[c]);
            }
        }
    }
    thermal = (struct ss_thermal_status){0};
    ss_thermal_status(&thermal);
    for (unsigned i = 0; i < 5; ++i) result.millicelsius_after[i] = thermal.millicelsius[i];
    result.readiness |= (ss_clock_ready() ? 4u : 0u) | (!ss_throttled() ? 8u : 0u);
    if (ss_clock_info(result.clock_after, sizeof(result.clock_after)) <= 0) ss_fail(0xb706);
    ss_activity_end(1);
    ss_finish(&result, sizeof(result));
}
