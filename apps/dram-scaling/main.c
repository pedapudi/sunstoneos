/* SPDX-License-Identifier: GPL-2.0-only */
/* DRAM read bandwidth against core count, buffer placement and access pattern
 * (docs/designs/dram-bandwidth.md). Eight pinned worker tasks fill a 40 MiB buffer, each
 * its own 5 MiB slot. A variant names a core set, an access pattern and a start stagger.
 * For each variant the selected cores start together at a barrier and stream their own
 * 3 MiB region for a fixed 12 ms window. Every core reports the chunks it read and the
 * time to its last chunk, so the host computes a rate per core and the sum of the rates
 * of cores that read at the same time, without a tail where fast cores have finished. */
#include "dram_scaling-app.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>
#include <string.h>
#include "result.h"
#include "portable.h"
_Static_assert(configNUMBER_OF_CORES == DS_CORES, "eight physical cores required");
_Static_assert(sizeof(struct ds_result) <= 16384, "output grant");
_Static_assert(DS_REGION_BYTES + DS_STAGGER_WRAP <= DS_SLOT_BYTES, "a region fits its slot");
static struct ds_result result;
static StaticTask_t state[DS_CORES];
static StackType_t stack[DS_CORES][1024];
static TaskHandle_t workers[DS_CORES], controller;
static uint64_t *buffer;
/* Job description, written by the controller before it notifies the workers. */
static struct {
    uint32_t mask, cores, mode, stagger, fill, stop;
    atomic_uint generation, arrived, go, finished, barrier_count, barrier_gate;
    uint64_t gate, deadline;
    uint64_t ticks[DS_CORES], chunks[DS_CORES], digest[DS_CORES];
} job;
#define counter ds_counter
static void worker(void *arg)
{
    const unsigned core = (unsigned)(uintptr_t)arg;
    unsigned seen = 0;
    for (;;) {
        /* The OS may notify the application's tasks for its own purposes, so a wake-up
         * counts only when the job generation has advanced and names this core. */
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const unsigned generation = atomic_load_explicit(&job.generation, memory_order_acquire);
        if (generation == seen) continue;
        seen = generation;
        if (!(job.mask >> core & 1u)) continue;
        if (portGET_CORE_ID() != core) ss_fail(0xc710 | core);
        uint64_t *slot = buffer + (size_t)core * (DS_SLOT_BYTES / 8);
        /* Barrier: the last core to arrive stamps the common gate and opens it. */
        if (atomic_fetch_add_explicit(&job.arrived, 1, memory_order_acq_rel) + 1 == job.cores) {
            job.gate = counter();
            job.deadline = job.gate + DS_WINDOW_TICKS;
            atomic_store_explicit(&job.go, 1, memory_order_release);
        }
        while (!atomic_load_explicit(&job.go, memory_order_acquire)) ds_relax();
        uint64_t digest = 0, chunks = 0, end;
        if (job.fill) {
            const uint64_t base = (uint64_t)core * (DS_SLOT_BYTES / 8);
            for (size_t i = 0; i < DS_SLOT_BYTES / 8; ++i) {
                const uint64_t w = ds_word(base + i);
                slot[i] = w;
                digest ^= w;
            }
            ds_publish();
            end = counter();
        } else {
            const uint32_t mode = job.mode;
            const uint64_t *region = slot + ds_region_offset(core, job.stagger) / 8;
            const uint32_t bytes = ds_chunk_bytes(mode);
            const uint32_t per_check = 65536u / bytes;      /* a deadline check about every 64 KiB */
            const uint64_t deadline = job.deadline;
            uint32_t j = 0;
            if (mode >= 5) {
                /* Rounds: each core reads one 16 KiB chunk, then all wait for each other. The last
                 * core to arrive decides, before it opens the barrier, whether the window is over. */
                const unsigned rank = (unsigned)__builtin_popcount(job.mask & ((1u << core) - 1u));
                unsigned gate = 0;
                for (;;) {
                    const uint64_t *p = mode == 5
                        ? buffer + (size_t)(((uint64_t)j * job.cores + rank) % DS_SHARED_CHUNKS) * (bytes / 8)
                        : region + (size_t)ds_chunk_index(mode, j) * (bytes / 8);
                    digest ^= ds_read(p, bytes);
                    ++j;
                    if (atomic_fetch_add_explicit(&job.barrier_count, 1, memory_order_acq_rel) + 1 == job.cores) {
                        job.stop = counter() >= deadline;
                        atomic_store_explicit(&job.barrier_count, 0, memory_order_relaxed);
                        atomic_store_explicit(&job.barrier_gate, gate + 1, memory_order_release);
                    } else {
                        while (atomic_load_explicit(&job.barrier_gate, memory_order_acquire) == gate) ds_relax();
                    }
                    ++gate;
                    if (job.stop) break;
                }
                end = counter();
                chunks = j;
                goto finished;
            }
            do {
                for (uint32_t k = 0; k < per_check; ++k, ++j)
                    digest ^= (mode == 4 ? ds_read_pf : ds_read)(region + (size_t)ds_chunk_index(mode, j) * (bytes / 8), bytes);
                end = counter();
            } while (end < deadline);
            chunks = j;
        }
    finished:
        job.ticks[core] = end - job.gate;
        job.chunks[core] = chunks;
        job.digest[core] = digest;
        atomic_fetch_add_explicit(&job.finished, 1, memory_order_acq_rel);
        xTaskNotifyGive(controller);
    }
}
void dram_scaling_init(void)
{
    const struct ss_grant *g = ss_grant();
    if (!g || g->main_core != 6 || g->worker_cores != 0xbf) ss_fail(0xc701);
    for (unsigned core = 0; core < DS_CORES; ++core) {
        workers[core] = xTaskCreateStaticAffinitySet(worker, "scaling", 1024,
            (void *)(uintptr_t)core, 2, stack[core], &state[core], 1u << core);
        if (!workers[core]) ss_fail(0xc702);
    }
}
/* Runs one job on the cores of `mask`. */
static void run(uint32_t mask, unsigned fill, unsigned mode, unsigned stagger)
{
    job.mask = mask;
    job.cores = (uint32_t)__builtin_popcount(mask);
    job.mode = mode;
    job.stagger = stagger;
    job.fill = fill;
    atomic_store(&job.arrived, 0);
    atomic_store(&job.go, 0);
    atomic_store(&job.finished, 0);
    atomic_store(&job.barrier_count, 0);
    atomic_store(&job.barrier_gate, 0);
    job.stop = 0;
    atomic_fetch_add_explicit(&job.generation, 1, memory_order_release);
    for (unsigned core = 0; core < DS_CORES; ++core)
        if (mask >> core & 1u) xTaskNotifyGive(workers[core]);
    const uint64_t deadline = counter() + 2 * 24000000u;
    while (atomic_load_explicit(&job.finished, memory_order_acquire) != job.cores) {
        if (counter() > deadline) ss_fail(0xc720 | fill);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    }
    if (ss_live() < 0) ss_fail(0xc730);
    ss_progress();
}
void dram_scaling_main(const struct ss_grant *g)
{
    controller = xTaskGetCurrentTaskHandle();
    buffer = g->regions[DRAM_SCALING_REGION_BUFFER].base;
    if (g->regions[DRAM_SCALING_REGION_BUFFER].bytes < DS_BUFFER_BYTES) ss_fail(0xc703);
    while (!ss_clock_ready() || ss_throttled()) {
        if (ss_live() < 0) ss_fail(0xc704);
        ss_progress();
        vTaskDelay(1);
    }
    memcpy(result.magic, DS_MAGIC, 8);
    result.counter_hz = ds_counter_hz();
    result.buffer_base = (uint64_t)(uintptr_t)buffer;
    result.window_ticks = DS_WINDOW_TICKS;
    result.warm = DS_WARM;
    result.reps = DS_REPS;
    struct ss_thermal_status thermal;
    ss_thermal_status(&thermal);
    for (unsigned i = 0; i < 5; ++i) result.millicelsius_before[i] = thermal.millicelsius[i];
    result.readiness = (ss_clock_ready() ? 1u : 0u) | (!ss_throttled() ? 2u : 0u);
    if (ss_clock_info(result.clock_before, sizeof(result.clock_before)) <= 0) ss_fail(0xc705);
    ss_activity_begin();
    run(0xffu, 1, 0, 0);                    /* fill: every core writes its own slot */
    for (unsigned core = 0; core < DS_CORES; ++core) result.fill_digest[core] = job.digest[core];
    static const struct ds_variant variants[DS_CONFIGS] = DS_VARIANTS;
    for (unsigned c = 0; c < DS_CONFIGS; ++c) {
        struct ds_config *config = &result.config[c];
        config->mask = variants[c].mask;
        config->mode = variants[c].mode;
        config->stagger = variants[c].stagger;
        for (unsigned window = 0; window < DS_WARM + DS_REPS; ++window) {
            run(variants[c].mask, 0, variants[c].mode, variants[c].stagger);
            if (window < DS_WARM) continue;
            for (unsigned core = 0; core < DS_CORES; ++core) {
                if (!(variants[c].mask >> core & 1u)) continue;
                config->sample[window - DS_WARM][core].chunks = (uint32_t)job.chunks[core];
                config->sample[window - DS_WARM][core].ticks = (uint32_t)job.ticks[core];
                config->digest[core] = job.digest[core];
            }
        }
    }
    thermal = (struct ss_thermal_status){0};
    ss_thermal_status(&thermal);
    for (unsigned i = 0; i < 5; ++i) result.millicelsius_after[i] = thermal.millicelsius[i];
    result.readiness |= (ss_clock_ready() ? 4u : 0u) | (!ss_throttled() ? 8u : 0u);
    if (ss_clock_info(result.clock_after, sizeof(result.clock_after)) <= 0) ss_fail(0xc706);
    ss_activity_end(1);
    ss_finish(&result, sizeof(result));
}
