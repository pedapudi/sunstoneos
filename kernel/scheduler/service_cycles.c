/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Measurement build only: see service_cycles.h. Each CPU writes only its own
 * cells; the report is read after every CPU has frozen its cause counters. */
#include "latency_cause.h"
#include "sunstone_latency.h"
#include <stddef.h>
extern void *memcpy(void *, const void *, size_t);

#define READ(reg) ({ uint64_t v; __asm__ volatile("mrs %0, " #reg : "=r"(v) :: "memory"); v; })
#define CORES SS_SERVICE_CYCLES_CORES
#define NO_PHASE SS_SERVICE_CYCLES_PHASES

/* The service pass in progress on each CPU and its setting. A pass runs on
 * one CPU from exception exit to return; a pass nested in it by an interrupt
 * saves and restores the outer one. */
struct local {
    unsigned services, state;
} __attribute__((aligned(64)));
static struct local local[CORES];
static struct ss_service_cycles_report report = {
    .magic = SS_SERVICE_CYCLES_MAGIC, .version = SS_SERVICE_CYCLES_VERSION, .bytes = sizeof(report),
    .cores = CORES, .phases = SS_SERVICE_CYCLES_PHASES, .kinds = SS_SERVICE_CYCLES_KINDS,
    .states = SS_SERVICE_CYCLES_STATES, .bins = SS_SERVICE_CYCLES_BINS,
    .switch_shift = SS_SERVICE_CYCLES_SWITCH_SHIFT};

static unsigned bin(uint64_t cycles)
{
    if (cycles < 64) return (unsigned)(cycles >> 3);
    unsigned e = 63u - (unsigned)__builtin_clzll(cycles);
    if (e > 17) return SS_SERVICE_CYCLES_BINS - 1;
    return 8u + (e - 6u) * 8u + (unsigned)((cycles >> (e - 3u)) & 7u);
}
/* The latency phase in progress at the given counter value: 0 idle, 1 busy,
 * or NO_PHASE outside them and while a phase boundary is being published. */
static unsigned phase_at(uint64_t now)
{
    uint64_t bounds[SS_LAT_PHASES][2];
    ss_latency_phase_bounds(bounds);
    for (unsigned p = SS_LAT_IDLE; p <= SS_LAT_BUSY; ++p) {
        const uint64_t begin = bounds[p][0], end = bounds[p][1];
        if (end == UINT64_MAX) return NO_PHASE;
        if (begin && now >= begin && (!end || now < end)) return p - SS_LAT_IDLE;
    }
    return NO_PHASE;
}
/* The setting comes from the CPU's own counter, so choosing it touches no
 * shared line. A drain inside a service pass takes the pass's setting. */
static unsigned setting(unsigned core)
{
    if (local[core].services) return local[core].state;
    return (unsigned)((READ(cntvct_el0) >> SS_SERVICE_CYCLES_SWITCH_SHIFT) & 1u);
}
bool ss_service_cycles_fast(unsigned core)
{ return core < CORES && setting(core) != 0; }
void ss_service_cycles_begin(struct ss_service_cycles_span *span, unsigned kind, unsigned core)
{
    span->kind = kind;
    span->core = core;
    span->state = core < CORES ? setting(core) : 1u;
    span->previous = core < CORES ? local[core].state : 0u;
    if (core < CORES && kind == SS_SERVICE_CYCLES_SERVICE) {
        local[core].state = span->state;
        ++local[core].services;
    }
    span->recording = core < CORES && ss_latency_cause_core_ready(core);
    span->irqs = span->recording ? ss_latency_cause_irq_count(core) : 0;
    __asm__ volatile("isb\n mrs %0, pmccntr_el0" : "=r"(span->cycles) :: "memory");
}
/* A drain called by a task can resume on another CPU; its span is dropped. */
void ss_service_cycles_end(const struct ss_service_cycles_span *span, unsigned core)
{
    uint64_t now;
    __asm__ volatile("isb\n mrs %0, pmccntr_el0" : "=r"(now) :: "memory");
    if (span->core >= CORES) return;
    if (span->kind == SS_SERVICE_CYCLES_SERVICE) {
        --local[span->core].services;
        local[span->core].state = span->previous;
    }
    if (!span->recording || core != span->core || !ss_latency_cause_core_ready(core)) return;
    const unsigned p = phase_at(ss_latency_now());
    if (p == NO_PHASE) return;
    const uint64_t cycles = now - span->cycles;
    struct ss_service_cycles_cell *cell = &report.cell[p][core][span->kind][span->state];
    if (ss_latency_cause_irq_count(core) != span->irqs) {
        ++cell->interrupted;
        cell->interrupted_sum += cycles;
        return;
    }
    ++cell->count;
    cell->sum += cycles;
    if (cycles > cell->max) cell->max = cycles;
    ++cell->bins[bin(cycles)];
}
/* Call after ss_latency_cause_snapshot succeeded: every CPU has frozen, so no
 * cell changes. */
bool ss_service_cycles_snapshot(struct ss_service_cycles_report *out)
{
    for (unsigned c = 0; c < CORES; ++c)
        if (ss_latency_cause_core_ready(c)) return false;
    report.complete = 1;
    memcpy(out, &report, sizeof(report));
    return true;
}
