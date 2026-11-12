/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "latency_trace.h"
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
#include "sunstone_latency.h"

static struct ss_sched_trace_report trace;
static struct {
    _Alignas(64) unsigned frozen, drain_active, drain_best, irq_active;
    /* Swap two buffers on a new maximum; copying every record under the API
     * mask would itself create a long span. Snapshot performs the copy. */
    struct ss_sched_trace_drain drain[2];
    struct ss_sched_trace_irq irq;
} state[SS_SCHED_TRACE_CORES];
static uint64_t tick_start;

static int enabled(unsigned core)
{
    return core < SS_SCHED_TRACE_CORES &&
        !__atomic_load_n(&state[core].frozen, __ATOMIC_RELAXED);
}
void ss_sched_trace_freeze(unsigned core)
{
    if (core < SS_SCHED_TRACE_CORES)
        __atomic_store_n(&state[core].frozen, 1, __ATOMIC_RELEASE);
}
void ss_sched_trace_snapshot(struct ss_sched_trace_report *out)
{
    *out = trace;
    out->magic = SS_SCHED_TRACE_MAGIC;
    out->version = 1;
    out->waits_capacity = SS_SCHED_TRACE_WAITS;
    out->ticks_capacity = SS_SCHED_TRACE_TICKS;
    for (unsigned core = 0; core < SS_SCHED_TRACE_CORES; ++core)
        out->core[core].drain = state[core].drain[state[core].drain_best];
}
void ss_sched_trace_startup(unsigned core, unsigned marker)
{
    if (core < SS_SCHED_TRACE_CORES && marker < 2 && !trace.startup[core][marker])
        trace.startup[core][marker] = ss_sched_trace_now();
}
static uint64_t command_ticks(const struct ss_sched_trace_command *command)
{ return command->stamp[6] - command->stamp[0]; }
void ss_sched_trace_command(unsigned core, const struct ss_sched_trace_command *command)
{
    if (!enabled(core)) return;
    struct ss_sched_trace_core *row = &trace.core[core];
    ++row->command_count;
    if (command_ticks(command) > command_ticks(&row->command)) row->command = *command;
    /* SS_K_SUSPEND is command 2; keep its own maximum because this is the
     * measured simultaneous worker shutdown after the busy phase. */
    if (command->command == 2 && (command->flags & 1) &&
        command_ticks(command) > command_ticks(&row->self_suspend)) row->self_suspend = *command;
    const uint64_t stage[4] = { command->stamp[1] - command->stamp[0],
        command->stamp[2] - command->stamp[1], command->stamp[4] - command->stamp[3],
        command->stamp[5] - command->stamp[4] };
    for (unsigned i = 0; i < 4; ++i)
        if (stage[i] > row->command_stage_max[i]) row->command_stage_max[i] = stage[i];
}
void ss_sched_trace_drain_begin(unsigned core)
{
    if (!enabled(core)) return;
    const unsigned current = state[core].drain_best ^ 1u;
    struct ss_sched_trace_drain *d = &state[core].drain[current];
    d->count = 0;
    d->dropped = 0;
    state[core].drain_active = current + 1;
    d->start = ss_sched_trace_now();
}
void ss_sched_trace_drain_end(unsigned core)
{
    const uint64_t end = ss_sched_trace_now();
    if (!enabled(core) || !state[core].drain_active) return;
    const unsigned current = state[core].drain_active - 1;
    struct ss_sched_trace_drain *d = &state[core].drain[current];
    d->end = end;
    const struct ss_sched_trace_drain *best = &state[core].drain[state[core].drain_best];
    if (d->end - d->start > best->end - best->start) state[core].drain_best = current;
    ++trace.core[core].drain_count;
    state[core].drain_active = 0;
}
void ss_sched_trace_record(unsigned core, const struct ss_sched_trace_record *record)
{
    if (!enabled(core) || !state[core].drain_active) return;
    struct ss_sched_trace_drain *d = &state[core].drain[state[core].drain_active - 1];
    if (d->count < SS_SCHED_TRACE_RECORDS) d->record[d->count++] = *record;
    else ++d->dropped;
}
void ss_sched_trace_irq_begin(unsigned core, uint64_t start)
{
    if (!enabled(core)) return;
    state[core].irq = (struct ss_sched_trace_irq){.stamp = {start}};
    state[core].irq_active = 1;
}
void ss_sched_trace_irq_stamp(unsigned core, unsigned stamp)
{
    const uint64_t now = ss_sched_trace_now();
    if (enabled(core) && state[core].irq_active && stamp < 4) state[core].irq.stamp[stamp] = now;
}
void ss_sched_trace_irq_end(unsigned core, unsigned result)
{
    if (!enabled(core) || !state[core].irq_active) return;
    struct ss_sched_trace_irq *irq = &state[core].irq;
    struct ss_sched_trace_core *row = &trace.core[core];
    irq->result = result;
    ++row->irq_count;
    if (irq->stamp[3] - irq->stamp[0] > row->irq.stamp[3] - row->irq.stamp[0]) row->irq = *irq;
    state[core].irq_active = 0;
}
void ss_sched_trace_service(unsigned core, unsigned part, uint64_t begin, uint64_t end)
{
    if (enabled(core) && state[core].irq_active && part < 5)
        state[core].irq.service[part] += end - begin;
}
void ss_sched_trace_service_pass(unsigned core)
{
    if (enabled(core) && state[core].irq_active) ++state[core].irq.passes;
}
void ss_sched_trace_tick_begin(void)
{ if (enabled(0)) tick_start = ss_sched_trace_now(); }
void ss_sched_trace_tick_end(void)
{
    const uint64_t end = ss_sched_trace_now();
    if (!enabled(0) || !tick_start) return;
    if (trace.tick_count < SS_SCHED_TRACE_TICKS)
        trace.tick[trace.tick_count++] = (struct ss_sched_trace_tick){tick_start, end};
    else ++trace.tick_dropped;
    tick_start = 0;
}
struct ss_sched_trace_wait ss_sched_trace_lock_begin(const struct ss_sched_trace_lock *lock,
    unsigned id, uintptr_t site, uint64_t serving, uint64_t next, unsigned tickets)
{
    struct ss_sched_trace_wait wait = { .start = ss_sched_trace_now(), .site = site, .lock = id,
        .serving = serving, .next = next, .flags = tickets ? SS_SCHED_WAIT_TICKETS : 0 };
    uint64_t first = atomic_load_explicit(&lock->generation, memory_order_acquire);
    uint64_t holder = atomic_load_explicit(&lock->core_plus_one, memory_order_relaxed);
    wait.holder_start = atomic_load_explicit(&lock->acquired, memory_order_relaxed);
    wait.holder_site = atomic_load_explicit(&lock->site, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    uint64_t last = atomic_load_explicit(&lock->generation, memory_order_relaxed);
    if (!(first & 1) && first == last) {
        wait.flags |= SS_SCHED_WAIT_SNAPSHOT;
        if (holder) {
            wait.flags |= SS_SCHED_WAIT_HOLDER;
            wait.holder_core = holder - 1;
            wait.holder_generation = first;
        }
    }
    return wait;
}
void ss_sched_trace_lock_acquired(struct ss_sched_trace_lock *lock, unsigned core,
                                  struct ss_sched_trace_wait *wait)
{
    wait->end = ss_sched_trace_now();
    /* The real lock excludes shadow writers here. A matched completion is
     * exact even if the holder's tick ended after the waiter arrived. */
    if ((wait->flags & SS_SCHED_WAIT_HOLDER) &&
        atomic_load_explicit(&lock->completed_generation, memory_order_relaxed) == wait->holder_generation) {
        wait->holder_end = atomic_load_explicit(&lock->completed_end, memory_order_relaxed);
        wait->flags |= SS_SCHED_WAIT_COMPLETED;
    }
    uint64_t generation = atomic_load_explicit(&lock->generation, memory_order_relaxed);
    atomic_fetch_add_explicit(&lock->generation, 1, memory_order_acq_rel);
    /* If an arriving reader observes any new relaxed payload value, its
     * acquire fence also observes the odd generation before accepting the
     * final generation sample. The real lock excludes writers, not readers. */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&lock->core_plus_one, core + 1, memory_order_relaxed);
    atomic_store_explicit(&lock->acquired, wait->end, memory_order_relaxed);
    atomic_store_explicit(&lock->site, wait->site, memory_order_relaxed);
    atomic_store_explicit(&lock->generation, generation + 2, memory_order_release);
    if (!enabled(core)) return;
    struct ss_sched_trace_core *row = &trace.core[core];
    const uint64_t elapsed = wait->end - wait->start;
    if (elapsed > row->wait_max.end - row->wait_max.start) row->wait_max = *wait;
    const uint64_t frequency = ss_latency_counter_frequency();
    if (!frequency || elapsed <= frequency / 200000) return;
    if (row->wait_count < SS_SCHED_TRACE_WAITS) row->wait[row->wait_count++] = *wait;
    else ++row->wait_dropped;
}
void ss_sched_trace_lock_release(struct ss_sched_trace_lock *lock)
{
    const uint64_t now = ss_sched_trace_now();
    uint64_t generation = atomic_load_explicit(&lock->generation, memory_order_relaxed);
    atomic_fetch_add_explicit(&lock->generation, 1, memory_order_acq_rel);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&lock->completed_generation, generation, memory_order_relaxed);
    atomic_store_explicit(&lock->completed_end, now, memory_order_relaxed);
    atomic_store_explicit(&lock->core_plus_one, 0, memory_order_relaxed);
    atomic_store_explicit(&lock->generation, generation + 2, memory_order_release);
}
#endif
