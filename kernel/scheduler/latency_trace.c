/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "latency_trace.h"
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
#include "sunstone_latency.h"

struct ss_sched_irq_history {
    _Atomic uint64_t generation, stamp[5], service[5], passes, result, sequence, previous_end;
};
struct ss_sched_switch_history {
    _Atomic uint64_t generation, begin, end, selected_slot, sequence, previous_end, irq_sequence;
};
static struct ss_sched_trace_report trace;
static struct {
    _Alignas(64) unsigned frozen, drain_active, drain_best, irq_active;
    /* Swap two buffers on a new maximum; copying every record under the API
     * mask would itself create a long span. Snapshot performs the copy. */
    struct ss_sched_trace_drain drain[2];
    struct ss_sched_trace_irq irq;
    uint64_t guard_end;
    _Atomic uint64_t irq_sequence, irq_end, switch_sequence, switch_end;
    struct ss_sched_irq_history irq_history[SS_SCHED_TRACE_HISTORY];
    struct ss_sched_switch_history switch_history[SS_SCHED_TRACE_HISTORY];
    struct ss_sched_trace_switch active_switch;
    unsigned direct_suspend_active;
    struct ss_sched_trace_command direct_suspend;
    struct {
        uint64_t start, end, serving, ticket;
        unsigned valid;
    } fifo;
} state[SS_SCHED_TRACE_CORES];
static uint64_t tick_start;
struct ss_sched_dispatch_entry {
    _Atomic uint64_t generation, dispatch;
    _Atomic uint64_t word[7];
};
static struct {
    _Atomic uint64_t next, completed, owner;
    struct ss_sched_dispatch_entry irq[SS_SCHED_TRACE_HISTORY];
} cpu0_dispatch;
/* SVC entry clears this tag using X0/X1 before either register is restored.
 * A voluntary selection cannot inherit the preceding IRQ's origin. */
_Atomic uint64_t ss_sched_trace_switch_irq[SS_SCHED_TRACE_CORES];
/* Assembly writes the first restore marker with only X0/X1 still saved.
 * Each CPU owns its row; snapshot reads after all diagnostic writers freeze. */
uint64_t ss_sched_trace_startup_markers[SS_SCHED_TRACE_CORES][2];

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
    out->version = SS_SCHED_TRACE_VERSION;
    out->waits_capacity = SS_SCHED_TRACE_WAITS;
    out->ticks_capacity = SS_SCHED_TRACE_TICKS;
    out->history_capacity = SS_SCHED_TRACE_HISTORY;
    out->cpu0_window.capacity = SS_SCHED_TRACE_HISTORY;
    for (unsigned core = 0; core < SS_SCHED_TRACE_CORES; ++core) {
        out->core[core].drain = state[core].drain[state[core].drain_best];
        for (unsigned marker = 0; marker < 2; ++marker)
            out->startup[core][marker] = ss_sched_trace_startup_markers[core][marker];
    }
}
void ss_sched_trace_startup(unsigned core, unsigned marker)
{
    if (core < SS_SCHED_TRACE_CORES && marker < 2 && !ss_sched_trace_startup_markers[core][marker])
        ss_sched_trace_startup_markers[core][marker] = ss_sched_trace_now();
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
    if (command->command == 2 && (command->flags & 1)) {
        bool direct = (command->flags & 64) != 0, best_direct = (row->self_suspend.flags & 64) != 0;
        uint64_t length = direct ? command->stamp[6] - command->stamp[3] : command_ticks(command);
        uint64_t best = best_direct ? row->self_suspend.stamp[6] - row->self_suspend.stamp[3] :
            command_ticks(&row->self_suspend);
        if ((direct && !best_direct) || (direct == best_direct && length > best)) row->self_suspend = *command;
    }
    const uint64_t stage[4] = { command->stamp[1] - command->stamp[0],
        command->stamp[2] - command->stamp[1], command->stamp[4] - command->stamp[3],
        command->stamp[5] - command->stamp[4] };
    /* The final two legacy maxima describe drain and slice work. A direct
     * suspension has different stages, retained in its complete record. */
    for (unsigned i = 0; i < ((command->flags & 64) ? 2u : 4u); ++i)
        if (stage[i] > row->command_stage_max[i]) row->command_stage_max[i] = stage[i];
}
void ss_sched_trace_direct_suspend_begin(unsigned core, const struct ss_sched_trace_command *command)
{
    if (!enabled(core)) return;
    state[core].direct_suspend = *command;
    state[core].direct_suspend.command = 2;
    state[core].direct_suspend.flags = (command->flags | UINT64_C(65)) & ~UINT64_C(58);
    state[core].direct_suspend_active = 1;
}
void ss_sched_trace_direct_suspend_mark(unsigned core)
{
    if (enabled(core) && state[core].direct_suspend_active)
        state[core].direct_suspend.stamp[4] = ss_sched_trace_now();
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
uint64_t ss_sched_trace_dispatch_begin(unsigned core, unsigned interrupt, uint64_t pc, uint64_t spsr)
{
    if (core || !enabled(core)) return 0;
    uint64_t sequence = atomic_fetch_add_explicit(&cpu0_dispatch.next, 1, memory_order_relaxed) + 1;
    struct ss_sched_dispatch_entry *event = &cpu0_dispatch.irq[(sequence - 1) % SS_SCHED_TRACE_HISTORY];
    atomic_fetch_add_explicit(&event->generation, 1, memory_order_acq_rel);
    /* Pair payload observations with the reader fence after publishing odd. */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&event->dispatch, sequence, memory_order_relaxed);
    const uint64_t words[7] = {0, (interrupt & UINT64_C(0xffff)) |
        ((spsr & UINT64_C(0xffffffff)) << 16), ss_sched_trace_now(), 0, pc, 0, 0};
    for (unsigned i = 0; i < 7; ++i) atomic_store_explicit(&event->word[i], words[i], memory_order_relaxed);
    atomic_fetch_add_explicit(&event->generation, 1, memory_order_release);
    return sequence;
}
void ss_sched_trace_dispatch_end(unsigned core, uint64_t sequence)
{
    if (core || !sequence || !enabled(core)) return;
    struct ss_sched_dispatch_entry *event = &cpu0_dispatch.irq[(sequence - 1) % SS_SCHED_TRACE_HISTORY];
    if (atomic_load_explicit(&event->dispatch, memory_order_relaxed) != sequence) return;
    atomic_fetch_add_explicit(&event->generation, 1, memory_order_acq_rel);
    /* Pair payload observations with the reader fence after publishing odd. */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&event->word[3], ss_sched_trace_now(), memory_order_relaxed);
    atomic_fetch_add_explicit(&event->generation, 1, memory_order_release);
    atomic_store_explicit(&cpu0_dispatch.completed, sequence, memory_order_release);
}
static void dispatch_owner(unsigned field, uint64_t value, uint64_t irq_sequence)
{
    const uint64_t sequence = atomic_load_explicit(&cpu0_dispatch.owner, memory_order_relaxed);
    if (!sequence) return;
    struct ss_sched_dispatch_entry *event = &cpu0_dispatch.irq[(sequence - 1) % SS_SCHED_TRACE_HISTORY];
    if (atomic_load_explicit(&event->dispatch, memory_order_relaxed) != sequence) return;
    atomic_fetch_add_explicit(&event->generation, 1, memory_order_acq_rel);
    /* Pair payload observations with the reader fence after publishing odd. */
    atomic_thread_fence(memory_order_release);
    if (field == 6) atomic_store_explicit(&event->word[0], irq_sequence, memory_order_relaxed);
    atomic_store_explicit(&event->word[field], value, memory_order_relaxed);
    atomic_fetch_add_explicit(&event->generation, 1, memory_order_release);
}
void ss_sched_trace_irq_begin(unsigned core, uint64_t start)
{
    if (!enabled(core)) return;
    state[core].irq = (struct ss_sched_trace_irq){.stamp = {start}};
    state[core].guard_end = 0;
    state[core].irq_active = 1;
    if (!core) {
        atomic_store_explicit(&cpu0_dispatch.owner,
            atomic_load_explicit(&cpu0_dispatch.completed, memory_order_acquire), memory_order_relaxed);
        dispatch_owner(5, start, 0);
    }
}
void ss_sched_trace_irq_guard(unsigned core)
{
    const uint64_t now = ss_sched_trace_now();
    if (enabled(core) && state[core].irq_active) state[core].guard_end = now;
}
void ss_sched_trace_irq_fast(unsigned core)
{
    if (!enabled(core) || !state[core].irq_active) return;
    for (unsigned i = 1; i < 4; ++i) state[core].irq.stamp[i] = state[core].guard_end;
    ss_sched_trace_irq_end(core, 3);
}
void ss_sched_trace_irq_stamp(unsigned core, unsigned stamp)
{
    const uint64_t now = ss_sched_trace_now();
    if (enabled(core) && state[core].irq_active && stamp < 4) state[core].irq.stamp[stamp] = now;
}
static void irq_history_store(struct ss_sched_irq_history *out, const struct ss_sched_trace_irq_chain *in)
{
    atomic_fetch_add_explicit(&out->generation, 1, memory_order_acq_rel);
    /* Pair payload observations with the reader fence after publishing odd. */
    atomic_thread_fence(memory_order_release);
    for (unsigned i = 0; i < 5; ++i) {
        atomic_store_explicit(&out->stamp[i], in->stamp[i], memory_order_relaxed);
        atomic_store_explicit(&out->service[i], in->service[i], memory_order_relaxed);
    }
    atomic_store_explicit(&out->passes, in->passes, memory_order_relaxed);
    atomic_store_explicit(&out->result, in->result, memory_order_relaxed);
    atomic_store_explicit(&out->sequence, in->sequence, memory_order_relaxed);
    atomic_store_explicit(&out->previous_end, in->previous_end, memory_order_relaxed);
    atomic_fetch_add_explicit(&out->generation, 1, memory_order_release);
}
static void switch_history_store(struct ss_sched_switch_history *out, const struct ss_sched_trace_switch *in)
{
    atomic_fetch_add_explicit(&out->generation, 1, memory_order_acq_rel);
    /* Pair payload observations with the reader fence after publishing odd. */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&out->begin, in->begin, memory_order_relaxed);
    atomic_store_explicit(&out->end, in->end, memory_order_relaxed);
    atomic_store_explicit(&out->selected_slot, in->selected_slot, memory_order_relaxed);
    atomic_store_explicit(&out->sequence, in->sequence, memory_order_relaxed);
    atomic_store_explicit(&out->previous_end, in->previous_end, memory_order_relaxed);
    atomic_store_explicit(&out->irq_sequence, in->irq_sequence, memory_order_relaxed);
    atomic_fetch_add_explicit(&out->generation, 1, memory_order_release);
}
#ifdef SS_SCHED_TRACE_HOST_TEST
extern void ss_sched_trace_test_history_read(unsigned, unsigned, uint64_t) __attribute__((weak));
#define SS_HISTORY_READ(core, kind, sequence) do { if (ss_sched_trace_test_history_read) \
    ss_sched_trace_test_history_read(core, kind, sequence); } while (0)
#else
#define SS_HISTORY_READ(core, kind, sequence) ((void)0)
#endif
static bool irq_history_read(unsigned core, uint64_t sequence, struct ss_sched_trace_irq_chain *out)
{
    const struct ss_sched_irq_history *in = &state[core].irq_history[(sequence - 1) % SS_SCHED_TRACE_HISTORY];
    uint64_t generation = atomic_load_explicit(&in->generation, memory_order_acquire);
    if (generation & 1) return false;
    for (unsigned i = 0; i < 5; ++i) {
        out->stamp[i] = atomic_load_explicit(&in->stamp[i], memory_order_relaxed);
        out->service[i] = atomic_load_explicit(&in->service[i], memory_order_relaxed);
    }
    out->passes = atomic_load_explicit(&in->passes, memory_order_relaxed);
    out->result = atomic_load_explicit(&in->result, memory_order_relaxed);
    out->sequence = atomic_load_explicit(&in->sequence, memory_order_relaxed);
    SS_HISTORY_READ(core, 0, sequence);
    out->previous_end = atomic_load_explicit(&in->previous_end, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    return generation == atomic_load_explicit(&in->generation, memory_order_relaxed) && out->sequence == sequence;
}
static bool switch_history_read(unsigned core, uint64_t sequence, struct ss_sched_trace_switch *out)
{
    const struct ss_sched_switch_history *in = &state[core].switch_history[(sequence - 1) % SS_SCHED_TRACE_HISTORY];
    uint64_t generation = atomic_load_explicit(&in->generation, memory_order_acquire);
    if (generation & 1) return false;
    out->begin = atomic_load_explicit(&in->begin, memory_order_relaxed);
    out->end = atomic_load_explicit(&in->end, memory_order_relaxed);
    out->selected_slot = atomic_load_explicit(&in->selected_slot, memory_order_relaxed);
    out->sequence = atomic_load_explicit(&in->sequence, memory_order_relaxed);
    SS_HISTORY_READ(core, 1, sequence);
    out->previous_end = atomic_load_explicit(&in->previous_end, memory_order_relaxed);
    out->irq_sequence = atomic_load_explicit(&in->irq_sequence, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    return generation == atomic_load_explicit(&in->generation, memory_order_relaxed) && out->sequence == sequence;
}
void ss_sched_trace_irq_end(unsigned core, unsigned result)
{
    if (!enabled(core) || !state[core].irq_active) return;
    struct ss_sched_trace_irq *irq = &state[core].irq;
    struct ss_sched_trace_core *row = &trace.core[core];
    irq->result = result;
    ++row->irq_count;
    if (irq->stamp[3] - irq->stamp[0] > row->irq.stamp[3] - row->irq.stamp[0]) row->irq = *irq;
    uint64_t sequence = atomic_load_explicit(&state[core].irq_sequence, memory_order_relaxed) + 1;
    atomic_store_explicit(&ss_sched_trace_switch_irq[core], sequence, memory_order_relaxed);
    if (!core) {
        dispatch_owner(6, irq->stamp[3], sequence);
        atomic_store_explicit(&cpu0_dispatch.owner, 0, memory_order_relaxed);
    }
    struct ss_sched_trace_irq_chain last = {.sequence = sequence,
        .previous_end = atomic_load_explicit(&state[core].irq_end, memory_order_relaxed),
        .passes = irq->passes, .result = result};
    last.stamp[0] = irq->stamp[0];
    last.stamp[1] = state[core].guard_end;
    for (unsigned i = 1; i < 4; ++i) last.stamp[i + 1] = irq->stamp[i];
    for (unsigned i = 0; i < 5; ++i) last.service[i] = irq->service[i];
    irq_history_store(&state[core].irq_history[(sequence - 1) % SS_SCHED_TRACE_HISTORY], &last);
    atomic_store_explicit(&state[core].irq_end, irq->stamp[3], memory_order_relaxed);
    atomic_store_explicit(&state[core].irq_sequence, sequence, memory_order_release);
    state[core].irq_active = 0;
}
void ss_sched_trace_switch_begin(unsigned core)
{
    const uint64_t now = ss_sched_trace_now();
    if (!enabled(core)) return;
    state[core].active_switch = (struct ss_sched_trace_switch){.begin = now,
        .previous_end = atomic_load_explicit(&state[core].switch_end, memory_order_relaxed),
        .irq_sequence = atomic_load_explicit(&ss_sched_trace_switch_irq[core], memory_order_relaxed),
        .sequence = atomic_load_explicit(&state[core].switch_sequence, memory_order_relaxed) + 1};
    if (state[core].direct_suspend_active) state[core].direct_suspend.stamp[5] = now;
}
void ss_sched_trace_switch_end(unsigned core, unsigned slot)
{
    const uint64_t now = ss_sched_trace_now();
    if (!enabled(core)) return;
    struct ss_sched_trace_switch *s = &state[core].active_switch;
    s->end = now;
    s->selected_slot = slot;
    switch_history_store(&state[core].switch_history[(s->sequence - 1) % SS_SCHED_TRACE_HISTORY], s);
    atomic_store_explicit(&state[core].switch_end, now, memory_order_relaxed);
    atomic_store_explicit(&state[core].switch_sequence, s->sequence, memory_order_release);
    if (state[core].direct_suspend_active) {
        state[core].direct_suspend.stamp[6] = now;
        ss_sched_trace_command(core, &state[core].direct_suspend);
        state[core].direct_suspend_active = 0;
    }
}
static void summarize(struct ss_sched_trace_summary *summary, uint64_t ticks)
{
    static const unsigned limits[5] = {10, 25, 50, 80, 100};
    uint64_t frequency = ss_latency_counter_frequency();
    ++summary->count;
    summary->sum += ticks;
    if (ticks > summary->maximum) summary->maximum = ticks;
    for (unsigned i = 0; i < 5; ++i)
        if (ticks > frequency * limits[i] / 1000000) ++summary->over_us[i];
}
static void cpu0_window(uint64_t ready, uint64_t running,
                        const struct ss_sched_trace_switch *selections, uint64_t last_switch, bool incomplete)
{
    struct ss_sched_trace_cpu0_window *out = &trace.cpu0_window;
    *out = (struct ss_sched_trace_cpu0_window){.capacity = SS_SCHED_TRACE_HISTORY};
    const uint64_t last_irq = atomic_load_explicit(&cpu0_dispatch.next, memory_order_relaxed);
    if (incomplete) out->flags |= SS_SCHED_WINDOW_INCOMPLETE;
    uint64_t first = last_irq > SS_SCHED_TRACE_HISTORY ? last_irq - SS_SCHED_TRACE_HISTORY + 1 : 1;
    for (uint64_t sequence = first; sequence <= last_irq; ++sequence) {
        const struct ss_sched_dispatch_entry *event = &cpu0_dispatch.irq[(sequence - 1) % SS_SCHED_TRACE_HISTORY];
        uint64_t generation = atomic_load_explicit(&event->generation, memory_order_acquire);
        uint64_t tag = atomic_load_explicit(&event->dispatch, memory_order_relaxed), words[7];
        for (unsigned i = 0; i < 7; ++i) words[i] = atomic_load_explicit(&event->word[i], memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if ((generation & 1) || tag != sequence ||
            generation != atomic_load_explicit(&event->generation, memory_order_relaxed)) {
            out->flags |= SS_SCHED_WINDOW_INCOMPLETE;
            continue;
        }
        if (words[2] > running) continue;
        if (words[3] > running || words[5] > running || words[6] > running) continue;
        if (!words[3] || (words[5] && !words[6])) {
            out->flags |= SS_SCHED_WINDOW_INCOMPLETE;
            continue;
        }
        struct ss_sched_trace_window_irq *row = &out->irq[out->irq_count++];
        row->sequence = words[0]; row->descriptor = words[1]; row->begin = words[2];
        row->end = words[3]; row->pc = words[4]; row->owner_begin = words[5]; row->owner_end = words[6];
    }
    if (last_irq > SS_SCHED_TRACE_HISTORY &&
        (!out->irq_count || out->irq[0].begin > ready)) out->flags |= SS_SCHED_WINDOW_IRQ_PREFIX_LOST;
    for (unsigned i = 0; i < SS_SCHED_TRACE_HISTORY; ++i) {
        const struct ss_sched_trace_switch *s = &selections[i];
        if (!s->sequence || !s->end || s->end > running) continue;
        out->selection[out->switch_count++] = (struct ss_sched_trace_window_switch){
            s->begin, s->end, s->irq_sequence, s->selected_slot};
    }
    if (last_switch > SS_SCHED_TRACE_HISTORY &&
        (!out->switch_count || out->selection[0].begin > ready)) out->flags |= SS_SCHED_WINDOW_SWITCH_PREFIX_LOST;
    const uint64_t latest = trace.history[0].latest_sequence;
    bool found = !latest;
    for (unsigned i = 0; i < out->irq_count; ++i) if (out->irq[i].sequence == latest) found = true;
    if (!found) out->flags |= SS_SCHED_WINDOW_INCOMPLETE;
}
/* The task observer runs unmasked. Each atomic history row is read once;
 * replacement during that read leaves the sample explicitly unattributed.
 * Completed events after the captured task timestamp remain excluded. */
void ss_sched_trace_wake_sample(unsigned core, uint64_t ready, uint64_t running, unsigned slot)
{
    if (!enabled(core) || !ready || running < ready) return;
    struct ss_sched_trace_irq_chain irq_snapshot[SS_SCHED_TRACE_HISTORY] = {0};
    struct ss_sched_trace_switch switch_snapshot[SS_SCHED_TRACE_HISTORY] = {0};
    const uint64_t irq_count = atomic_load_explicit(&state[core].irq_sequence, memory_order_acquire);
    const uint64_t switch_count = atomic_load_explicit(&state[core].switch_sequence, memory_order_acquire);
    bool incomplete = false;
    uint64_t first = irq_count > SS_SCHED_TRACE_HISTORY ? irq_count - SS_SCHED_TRACE_HISTORY + 1 : 1;
    for (uint64_t sequence = first; sequence <= irq_count; ++sequence)
        if (!irq_history_read(core, sequence, &irq_snapshot[sequence - first])) {
            irq_snapshot[sequence - first] = (struct ss_sched_trace_irq_chain){0};
            incomplete = true;
        }
    first = switch_count > SS_SCHED_TRACE_HISTORY ? switch_count - SS_SCHED_TRACE_HISTORY + 1 : 1;
    for (uint64_t sequence = first; sequence <= switch_count; ++sequence)
        if (!switch_history_read(core, sequence, &switch_snapshot[sequence - first])) {
            switch_snapshot[sequence - first] = (struct ss_sched_trace_switch){0};
            incomplete = true;
        }
    const struct ss_sched_trace_switch empty_switch = {0};
    const struct ss_sched_trace_irq_chain empty_irq = {0};
    const struct ss_sched_trace_switch *s = &empty_switch;
    const struct ss_sched_trace_irq_chain *irq = &empty_irq, *latest = &empty_irq;
    struct ss_sched_trace_wake_history history = {
        .irq_overwritten = irq_count > SS_SCHED_TRACE_HISTORY ? irq_count - SS_SCHED_TRACE_HISTORY : 0,
        .switch_overwritten = switch_count > SS_SCHED_TRACE_HISTORY ? switch_count - SS_SCHED_TRACE_HISTORY : 0
    };
    for (unsigned i = 0; i < SS_SCHED_TRACE_HISTORY; ++i) {
        const struct ss_sched_trace_switch *candidate = &switch_snapshot[i];
        if (candidate->end && candidate->end <= running && candidate->sequence > s->sequence) s = candidate;
        const struct ss_sched_trace_irq_chain *event = &irq_snapshot[i];
        if (!event->sequence) continue;
        if (event->stamp[4] > running) ++history.future_irqs;
        else if (event->sequence > latest->sequence) latest = event;
    }
    bool lost = !s->sequence && history.switch_overwritten;
    if (s->irq_sequence) {
        for (unsigned i = 0; i < SS_SCHED_TRACE_HISTORY; ++i) {
            const struct ss_sched_trace_irq_chain *candidate = &irq_snapshot[i];
            if (candidate->sequence == s->irq_sequence && candidate->stamp[4] <= running) irq = candidate;
        }
        if (!irq->sequence) lost = true;
    }
    if (!latest->sequence && history.irq_overwritten) lost = true;
    history.latest_start = latest->stamp[0];
    history.latest_end = latest->stamp[4];
    history.latest_work = latest->stamp[3] - latest->stamp[2];
    history.latest_result = latest->result;
    history.latest_sequence = latest->sequence;
    if (irq->sequence && latest->sequence >= irq->sequence) {
        history.trailing_count = latest->sequence - irq->sequence;
        for (unsigned i = 0; i < SS_SCHED_TRACE_HISTORY; ++i) {
            const struct ss_sched_trace_irq_chain *event = &irq_snapshot[i];
            if (event->sequence > irq->sequence && event->sequence <= latest->sequence)
                history.trailing_work += event->stamp[3] - event->stamp[2];
        }
        /* Exact sequence tags and a retained first record prove every later
         * record through latest is still in this circular history. */
    }
    uint64_t flags = 0;
    if (irq->sequence) flags |= SS_SCHED_WAKE_IRQ;
    if (s->sequence && s->end) flags |= SS_SCHED_WAKE_SWITCH;
    if (lost) flags |= SS_SCHED_WAKE_HISTORY_LOST;
    if (incomplete) flags |= SS_SCHED_WAKE_SNAPSHOT_INCOMPLETE;
    bool multiple = irq->previous_end > ready || s->previous_end > ready || history.trailing_count;
    if (multiple) flags |= SS_SCHED_WAKE_MULTIPLE;
    bool linked = !lost && !incomplete && !multiple && (flags & 3) == 3 &&
        irq->stamp[0] <= irq->stamp[1] && irq->stamp[1] <= irq->stamp[2] &&
        irq->stamp[2] <= irq->stamp[3] && irq->stamp[3] <= irq->stamp[4] &&
        ready <= irq->stamp[4] && irq->stamp[4] <= s->begin &&
        s->begin <= s->end && s->end <= running &&
        s->irq_sequence == irq->sequence && s->selected_slot == slot &&
        /* Result zero leaves the port's existing yield request intact. The
         * recorded selection establishes the switch independently of it. */
        (irq->result == 0 || irq->result == 1 || irq->result == 3);
    flags |= linked ? SS_SCHED_WAKE_LINKED : SS_SCHED_WAKE_UNMATCHED;
    unsigned kind = linked ? (irq->result == 3 ? 0 : 1) : 2;
    struct ss_sched_trace_wake_core *row = &trace.wake[core];
    uint64_t elapsed = running - ready;
    summarize(&row->summary[kind][0], elapsed);
    if (linked) summarize(&row->summary[kind][1], running - irq->stamp[0]);
    if (!row->maximum.ready || elapsed > row->maximum.running - row->maximum.ready) {
        row->maximum = (struct ss_sched_trace_wake){ .ready = ready, .running = running,
            .flags = flags, .running_slot = slot, .irq = *irq, .selection = *s };
        trace.history[core] = history;
        if (!core) cpu0_window(ready, running, switch_snapshot, switch_count, incomplete);
    }
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
/* The port calls this after its FIFO end timestamp. The sample belongs to
 * this CPU's next enclosing lock trace; its storage cost is not FIFO time. */
void ss_sched_trace_fifo_sample(unsigned core, uint64_t start, uint64_t end, uint32_t word)
{
    if (core >= SS_SCHED_TRACE_CORES) return;
    state[core].fifo.valid = 0;
    if (!enabled(core)) return;
    state[core].fifo.start = start;
    state[core].fifo.end = end;
    state[core].fifo.serving = word & UINT32_C(0xffff);
    state[core].fifo.ticket = word >> 16;
    state[core].fifo.valid = 1;
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
    if (core < SS_SCHED_TRACE_CORES) {
        /* Consume even a stale sample or one followed by a different lock.
         * Recursion and TAS paths never produce a fresh FIFO sample. */
        const unsigned valid = state[core].fifo.valid;
        state[core].fifo.valid = 0;
        if (valid && wait->lock == 0 &&
            wait->start <= state[core].fifo.start &&
            state[core].fifo.start <= state[core].fifo.end &&
            state[core].fifo.end <= wait->end) {
            wait->fifo_start = state[core].fifo.start;
            wait->fifo_end = state[core].fifo.end;
            wait->fifo_serving = state[core].fifo.serving;
            wait->fifo_ticket = state[core].fifo.ticket;
            wait->flags |= SS_SCHED_WAIT_ACTUAL_FIFO;
        }
    }
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
