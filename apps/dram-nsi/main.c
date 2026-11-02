/* SPDX-License-Identifier: GPL-2.0-only */
/* DRAM read bandwidth against core count with the interconnect words of boot0
 * (docs/designs/dram-bandwidth.md, "Interconnect words"). The eight worker tasks run at EL1 (the
 * OS grants that to dram_nsi_worker only). A probe job on core 6 reads the nine NSI words that boot0
 * writes after DRAM initialization; an image built with DN_WRITE then writes boot0's values in
 * boot0's order and reads them back. Around every read window the last core to arrive clears and
 * enables the counters of the DRAM target-agent ports 32 and 33 and the last core to finish reads
 * them. A phase runs the variants of result.h once with the words as found and, with DN_WRITE, once
 * after the write. */
#include "dram_nsi-app.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>
#include <string.h>
#include "result.h"
#include "portable.h"
_Static_assert(configNUMBER_OF_CORES == DN_CORES, "eight physical cores required");
_Static_assert(sizeof(struct dn_result) <= 16384, "output grant");
_Static_assert(DN_REGION_BYTES + DN_STAGGER_WRAP <= DN_SLOT_BYTES, "a region fits its slot");
static struct dn_result result;
static StaticTask_t state[DN_CORES];
static StackType_t stack[DN_CORES][1024];
static TaskHandle_t workers[DN_CORES], controller;
static uint64_t *buffer;
/* Job description, written by the controller before it notifies the workers. */
static struct {
    uint32_t mask, cores, mode, stagger, fill, stop, counters, write, error;
    atomic_uint generation, arrived, go, finished, ended, barrier_count, barrier_gate;
    struct dn_counters ctr;
    struct dn_probe probe;
    uint64_t gate, deadline;
    uint64_t ticks[DN_CORES], chunks[DN_CORES], digest[DN_CORES];
} job;
#define counter dn_counter
/* Workers run at EL1 and cannot call the application ABI: a failure records its code for the controller. */
static void dn_error(uint32_t code) { job.error = code; for (;;) dn_relax(); }
static const uint32_t word_address[DN_WORDS] = DN_WORD_ADDRESSES, word_value[DN_WORDS] = DN_WORD_VALUES;
/* The words of the probe: reads in a fixed order. */
static void probe_read(uint32_t *words)
{
    for (unsigned i = 0; i < DN_WORDS; ++i) words[i] = dn_reg_read(word_address[i]);
    dn_dmb();
}
/* boot0 routine 0x48f38: a barrier, then the nine stores in this order with a barrier after each
 * (the last store has none in boot0; the barrier after it here only orders the read-back). */
static void probe_write(void)
{
    dn_dmb();
    for (unsigned i = 0; i < DN_WORDS; ++i) {
        dn_reg_write(word_address[i], word_value[i]);
        dn_dmb();
    }
}
static void probe_extra(uint32_t *x)
{
    x[0] = dn_reg_read(0x02002580u);
    x[1] = dn_reg_read(0x02002584u);
    x[2] = dn_reg_read(0x02002588u);
    for (unsigned t = 0; t < 2; ++t) {
        const uint32_t b = DN_TA_PORT(32 + t);
        x[3 + 5 * t] = dn_reg_read(b + 0x10);
        x[4 + 5 * t] = dn_reg_read(b + 0x14);
        x[5 + 5 * t] = dn_reg_read(b + 0x18);
        x[6 + 5 * t] = dn_reg_read(b + 0xc0);
        x[7 + 5 * t] = dn_reg_read(b + 0xc8);
    }
}
/* Counter sequence of __nsi_pmu_timer_store_v2: period, enable off, clear, enable on. */
static void counters_start(void)
{
    for (unsigned t = 0; t < 2; ++t) {
        const uint32_t b = DN_TA_PORT(32 + t);
        dn_reg_write(b + 0xc0, 0);
        dn_reg_write(b + 0xc8, 0x7fffffffu);
        dn_reg_write(b + 0xc4, 1);
        dn_reg_write(b + 0xc0, 1);
    }
    dn_dmb();
}
static void counters_read(void)
{
    job.ctr.ticks = (uint32_t)counter();
    for (unsigned t = 0; t < 2; ++t) {
        const uint32_t b = DN_TA_PORT(32 + t);
        job.ctr.rq_rd[t] = dn_reg_read(b + 0xcc);
        job.ctr.rq_wr[t] = dn_reg_read(b + 0xd0);
        job.ctr.dt_rd[t] = dn_reg_read(b + 0xd4);
        job.ctr.dt_wr[t] = dn_reg_read(b + 0xd8);
        job.ctr.la_rd[t] = dn_reg_read(b + 0xdc);
    }
    job.ctr.ticks = (uint32_t)counter() - job.ctr.ticks;      /* cost of the reads */
    for (unsigned t = 0; t < 2; ++t) dn_reg_write(DN_TA_PORT(32 + t) + 0xc0, 0);
    dn_dmb();
}
void dram_nsi_worker(void *arg)
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
        if (portGET_CORE_ID() != core) dn_error(0xc710 | core);
        if (job.fill == 2) {                /* probe job: one core touches the interconnect */
            probe_read(job.probe.words_before);
            probe_extra(job.probe.extra);
            if (job.write) {
                probe_write();
                probe_read(job.probe.words_written);
            }
            atomic_fetch_add_explicit(&job.ended, 1, memory_order_acq_rel);
            atomic_fetch_add_explicit(&job.finished, 1, memory_order_acq_rel);
            xTaskNotifyGive(controller);
            continue;
        }
        uint64_t *slot = buffer + (size_t)core * (DN_SLOT_BYTES / 8);
        /* Barrier: the last core to arrive stamps the common gate and opens it. */
        if (atomic_fetch_add_explicit(&job.arrived, 1, memory_order_acq_rel) + 1 == job.cores) {
            if (job.counters) counters_start();
            job.gate = counter();
            job.deadline = job.gate + DN_WINDOW_TICKS;
            atomic_store_explicit(&job.go, 1, memory_order_release);
        }
        while (!atomic_load_explicit(&job.go, memory_order_acquire)) dn_relax();
        uint64_t digest = 0, chunks = 0, end;
        if (job.fill) {
            const uint64_t base = (uint64_t)core * (DN_SLOT_BYTES / 8);
            for (size_t i = 0; i < DN_SLOT_BYTES / 8; ++i) {
                const uint64_t w = dn_word(base + i);
                slot[i] = w;
                digest ^= w;
            }
            dn_publish();
            end = counter();
        } else {
            const uint32_t mode = job.mode;
            const uint64_t *region = slot + dn_region_offset(core, job.stagger) / 8;
            const uint32_t bytes = dn_chunk_bytes(mode);
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
                        ? buffer + (size_t)(((uint64_t)j * job.cores + rank) % DN_SHARED_CHUNKS) * (bytes / 8)
                        : region + (size_t)dn_chunk_index(mode, j) * (bytes / 8);
                    digest ^= dn_read(p, bytes);
                    ++j;
                    if (atomic_fetch_add_explicit(&job.barrier_count, 1, memory_order_acq_rel) + 1 == job.cores) {
                        job.stop = counter() >= deadline;
                        atomic_store_explicit(&job.barrier_count, 0, memory_order_relaxed);
                        atomic_store_explicit(&job.barrier_gate, gate + 1, memory_order_release);
                    } else {
                        while (atomic_load_explicit(&job.barrier_gate, memory_order_acquire) == gate) dn_relax();
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
                    digest ^= dn_read(region + (size_t)dn_chunk_index(mode, j) * (bytes / 8), bytes);
                end = counter();
            } while (end < deadline);
            chunks = j;
        }
    finished:
        job.ticks[core] = end - job.gate;
        job.chunks[core] = chunks;
        job.digest[core] = digest;
        /* The last core to end reads the counters while the others are idle. */
        if (atomic_fetch_add_explicit(&job.ended, 1, memory_order_acq_rel) + 1 == job.cores && job.counters)
            counters_read();
        atomic_fetch_add_explicit(&job.finished, 1, memory_order_acq_rel);
        xTaskNotifyGive(controller);
    }
}
void dram_nsi_init(void)
{
    const struct ss_grant *g = ss_grant();
    if (!g || g->main_core != 6 || g->worker_cores != 0xbf) ss_fail(0xc701);
    for (unsigned core = 0; core < DN_CORES; ++core) {
        workers[core] = xTaskCreateStaticAffinitySet(dram_nsi_worker, "nsi", 1024,
            (void *)(uintptr_t)core, 2, stack[core], &state[core], 1u << core);
        if (!workers[core]) ss_fail(0xc702);
    }
}
/* Runs one job on the cores of `mask`. */
static void run(uint32_t mask, unsigned fill, unsigned mode, unsigned stagger, unsigned counters)
{
    job.mask = mask;
    job.cores = (uint32_t)__builtin_popcount(mask);
    job.mode = mode;
    job.stagger = stagger;
    job.fill = fill;
    job.counters = counters;
    atomic_store(&job.ended, 0);
    atomic_store(&job.arrived, 0);
    atomic_store(&job.go, 0);
    atomic_store(&job.finished, 0);
    atomic_store(&job.barrier_count, 0);
    atomic_store(&job.barrier_gate, 0);
    job.stop = 0;
    atomic_fetch_add_explicit(&job.generation, 1, memory_order_release);
    for (unsigned core = 0; core < DN_CORES; ++core)
        if (mask >> core & 1u) xTaskNotifyGive(workers[core]);
    const uint64_t deadline = counter() + 2 * 24000000u;
    while (atomic_load_explicit(&job.finished, memory_order_acquire) != job.cores) {
        if (job.error) ss_fail(job.error);
        if (counter() > deadline) ss_fail(0xc720 | fill);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    }
    if (ss_live() < 0) ss_fail(0xc730);
    ss_progress();
}
void dram_nsi_main(const struct ss_grant *g)
{
    controller = xTaskGetCurrentTaskHandle();
    buffer = g->regions[DRAM_NSI_REGION_BUFFER].base;
    if (g->regions[DRAM_NSI_REGION_BUFFER].bytes < DN_BUFFER_BYTES) ss_fail(0xc703);
    while (!ss_clock_ready() || ss_throttled()) {
        if (ss_live() < 0) ss_fail(0xc704);
        ss_progress();
        vTaskDelay(1);
    }
    memcpy(result.magic, DN_MAGIC, 8);
    result.counter_hz = dn_counter_hz();
    result.buffer_base = (uint64_t)(uintptr_t)buffer;
    result.window_ticks = DN_WINDOW_TICKS;
    result.warm = DN_WARM;
    result.reps = DN_REPS;
    struct ss_thermal_status thermal;
    ss_thermal_status(&thermal);
    for (unsigned i = 0; i < 5; ++i) result.millicelsius_before[i] = thermal.millicelsius[i];
    result.readiness = (ss_clock_ready() ? 1u : 0u) | (!ss_throttled() ? 2u : 0u);
    if (ss_clock_info(result.clock_before, sizeof(result.clock_before)) <= 0) ss_fail(0xc705);
    ss_activity_begin();
    /* The probe job: core 6 reads the nine words before any read window. A data abort here ends the image
     * at the watchdog. */
    job.write = 0;
    run(0x40u, 2, 0, 0, 0);
    result.probe = job.probe;
#ifdef DN_WRITE
    result.probe.write_enabled = 1;
#endif
#ifdef DN_PROBE_ONLY
    result.readiness |= 16u;
#else
    run(0xffu, 1, 0, 0, 0);                 /* fill: every core writes its own slot */
    for (unsigned core = 0; core < DN_CORES; ++core) result.fill_digest[core] = job.digest[core];
    static const struct dn_variant variants[DN_CONFIGS] = DN_VARIANTS;
#ifdef DN_WRITE
    const unsigned phases = DN_PHASES;
#else
    const unsigned phases = 1;
#endif
    for (unsigned phase = 0; phase < phases; ++phase) {
        if (phase == 1) {
            /* Phase 0 ran with the words as found. The second probe job writes boot0's values in boot0's order
             * and reads them back; phase 1 then repeats the variants. */
            job.write = 1;
            run(0x40u, 2, 0, 0, 0);
            for (unsigned i = 0; i < DN_WORDS; ++i) result.probe.words_written[i] = job.probe.words_written[i];
            job.write = 0;
        }
        for (unsigned c = 0; c < DN_CONFIGS; ++c) {
            struct dn_config *config = &result.config[phase][c];
            config->mask = variants[c].mask;
            config->mode = variants[c].mode;
            config->stagger = variants[c].stagger;
            for (unsigned window = 0; window < DN_WARM + DN_REPS; ++window) {
                run(variants[c].mask, 0, variants[c].mode, variants[c].stagger, 1);
                if (window < DN_WARM) continue;
                for (unsigned core = 0; core < DN_CORES; ++core) {
                    if (!(variants[c].mask >> core & 1u)) continue;
                    config->sample[window - DN_WARM][core].chunks = (uint32_t)job.chunks[core];
                    config->sample[window - DN_WARM][core].ticks = (uint32_t)job.ticks[core];
                    config->digest[core] = job.digest[core];
                }
                config->counters[window - DN_WARM] = job.ctr;
            }
        }
    }
#endif
    thermal = (struct ss_thermal_status){0};
    ss_thermal_status(&thermal);
    for (unsigned i = 0; i < 5; ++i) result.millicelsius_after[i] = thermal.millicelsius[i];
    result.readiness |= (ss_clock_ready() ? 4u : 0u) | (!ss_throttled() ? 8u : 0u);
    if (ss_clock_info(result.clock_after, sizeof(result.clock_after)) <= 0) ss_fail(0xc706);
    ss_activity_end(1);
    ss_finish(&result, sizeof(result));
}
