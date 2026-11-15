#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "workload.h"
#include "sunstone_app.h"

/* The QEMU models have no OS and no address spaces; the kit's task creation is the direct FreeRTOS call. */
void *ss_task_create_static_affinity_set(void (*entry)(void *), const char *name, uint32_t stack_words, void *arg,
                                         unsigned long priority, void *stack, void *tcb, unsigned long affinity)
{
    return xTaskCreateStaticAffinitySet(entry, name, stack_words, arg, (UBaseType_t)priority, (StackType_t *)stack,
                                        (StaticTask_t *)tcb, (UBaseType_t)affinity);
}

#ifdef SUNSTONE_LATENCY_AXI_CAP
/* The board image's failure exit: the model prints the detail and value and stops. */
void ss_fail_value(uint32_t detail, uint32_t value)
{
    platform_puts("FAIL latency axi detail="); platform_hex(detail);
    platform_puts(" value="); platform_hex(value); platform_puts("\n");
    configASSERT(0);
    for (;;) {}
}
#endif
#ifdef SUNSTONE_LATENCY_CPU_ONLY
/* The CPU control has no DMA client; these hooks stand for the board image's absence of one. */
void latency_dma_model_tick(void) {}
void latency_dma_model_start(void) {}
unsigned latency_dma_model_stop(void) { return 0; }
#endif
static struct latency_result result;
static StaticTask_t controller_state;
static StackType_t controller_stack[2048];
static const char *const metric_names[SS_LAT_METRICS] = {
    "timer_irq", "irq_masked", "lock0_wait", "lock0_hold", "lock1_wait", "lock1_hold",
    "wake_idle", "wake_busy", "DMA_completion", "GPU_completion", "NPU_completion"
};
static void number(const char *name, uint64_t value)
{ platform_puts(name); platform_hex(value); }
static void controller(void *argument)
{
    (void)argument;
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    /* Owner notifications do not take the compatibility task lock. Exercise
     * that lock explicitly so the FIFO timing hook has a real acquisition. */
    taskENTER_CRITICAL();
    taskEXIT_CRITICAL();
#endif
#ifdef SUNSTONE_SCHED_PMU
    ss_sched_pmu_arm();
#endif
#ifdef SUNSTONE_LATENCY_CAUSE
    ss_latency_cause_arm();
#endif
    latency_workload_run(&result);
#ifdef SUNSTONE_LATENCY_CAUSE
    configASSERT(result.cause.magic == SS_LATENCY_CAUSE_MAGIC);
    configASSERT(!(result.cause.flags & SS_CAUSE_SNAPSHOT_INCOMPLETE));
    configASSERT(result.cause.bench.done && result.cause.bench.status == SS_CAUSE_OK);
    for (unsigned sample = 0; sample < SS_CAUSE_CASES; ++sample)
        configASSERT(result.cause.bench.count[sample] == SS_LATENCY_CAUSE_SAMPLES);
    if (result.ping.status != LATENCY_CAUSE_PING_OK) {
        /* One line that checks/latency-cause-model.py reads: a pair that timed out waiting for its peer (status 4) says the host did not
         * schedule the cores, not that the ping is broken. */
        number("latency cause ping result: status=", result.ping.status);
        for (unsigned pair = 0; pair < LATENCY_CAUSE_PING_PAIRS; ++pair) { number(" pair=", result.ping.pair[pair].status); }
        platform_puts("\n");
    }
    configASSERT(result.ping.status == LATENCY_CAUSE_PING_OK);
    platform_puts("PASS latency cause model: complete barrier samples and same/cross-type line exchanges; functional evidence only\n");
#endif
#ifdef SUNSTONE_LATENCY_SERVICE_CYCLES
    configASSERT(result.service.magic == SS_SERVICE_CYCLES_MAGIC && result.service.complete);
    configASSERT(result.service_footer[0] == SS_SERVICE_CYCLES_FOOTER_MAGIC);
    /* Each kind ran in both settings in both phases, and each cell's bins hold its uninterrupted count. */
    for (unsigned p = 0; p < SS_SERVICE_CYCLES_PHASES; ++p)
        for (unsigned kind = 0; kind < SS_SERVICE_CYCLES_KINDS; ++kind)
            for (unsigned state = 0; state < SS_SERVICE_CYCLES_STATES; ++state) {
                uint64_t count = 0;
                for (unsigned core = 0; core < SS_SERVICE_CYCLES_CORES; ++core) {
                    const struct ss_service_cycles_cell *cell = &result.service.cell[p][core][kind][state];
                    uint64_t binned = 0;
                    for (unsigned b = 0; b < SS_SERVICE_CYCLES_BINS; ++b) binned += cell->bins[b];
                    configASSERT(binned == cell->count && cell->max * cell->count >= cell->sum);
                    count += cell->count;
                }
                number(p ? "service_cycles busy kind=" : "service_cycles idle kind=", kind);
                number(" state=", state);
                number(" count=", count);
                platform_puts("\n");
                configASSERT(count);
            }
    platform_puts("PASS service cycle model: both drain settings timed in both phases; functional evidence only\n");
#endif
#ifdef SUNSTONE_LATENCY_WAKE_SEGMENTS
#ifndef SUNSTONE_LATENCY_RELEASE_MASK
#define SUNSTONE_LATENCY_RELEASE_MASK 0xffu
#endif
    configASSERT(result.wake_segments.magic == SS_WAKE_SEG_MAGIC && result.wake_segments.complete);
    configASSERT(result.wake_footer[0] == SS_WAKE_SEG_FOOTER_MAGIC);
    /* Every core linked busy wakes from interrupt entry to its running task, and CPU0 filed busy ticks. */
    for (unsigned core = 0; core < LATENCY_CORES; ++core) {
        const struct ss_wake_seg_core *w = &result.wake_segments.core[core];
        const uint64_t filed = w->cell[0].count + w->cell[1].count;
        number("wake_segments core=", core);
        number(" linked=", w->linked);
        number(" unlinked=", w->unlinked);
        number(" filed=", filed);
        number(" first_state=", w->first_state);
        number(" arms=", w->arms);
        platform_puts("\n");
        /* A wake whose interrupt chain a later interrupt replaced before the woken task finished its bookkeeping (a stall of
         * that core, long on a loaded host) is counted unlinked, by design; every busy wake is one or the other, and a chain
         * that is not recorded at all links none. The first wake may be among the unlinked. */
        configASSERT(filed && (w->first_state >> 16 & 1u) && w->linked + w->unlinked == LATENCY_BUSY_SAMPLES && 2 * w->linked > LATENCY_BUSY_SAMPLES);
        /* Each setting's worst wakes, largest first from the deadline; a released core's deadline is CPU0's. */
        for (unsigned setting = 0; setting < 2; ++setting)
            for (unsigned i = 0; i < SS_WAKE_WORST; ++i) {
                const struct ss_wake_seg_sample *x = &w->worst[setting][i];
                configASSERT(!x->running == (i >= w->cell[setting].count));
                if (!x->running) continue;
                configASSERT(x->deadline <= x->release && x->deadline <= x->entry_tick && x->release <= x->running);
                if (core && !(SUNSTONE_LATENCY_RELEASE_MASK >> core & 1u)) configASSERT(x->deadline == x->release);
                if (i) configASSERT(x->running - x->deadline <= w->worst[setting][i - 1].running - w->worst[setting][i - 1].deadline);
            }
#if configSS_LOCAL_TIMERS
        /* CPUs 1-7 that wait on their own deadlines armed their timers; those CPU0 releases had nothing to arm. */
        /* The cores of the cause ping wait on one another's turns after the measured phases, with timeouts, and arm their timers
         * for them while the recorder is still armed; every other core must arm none for a wake CPU0 released. */
        bool pings = false;
        for (unsigned pair = 0; pair < LATENCY_CAUSE_PING_PAIRS; ++pair) pings |= result.ping.pair[pair].target_core == core;
        if (core) configASSERT((SUNSTONE_LATENCY_RELEASE_MASK >> core & 1u) ? (pings || !w->arms) : w->arms != 0);
        /* Their timer samples, kept until the woken task ran, were recorded. */
        if (core && !(SUNSTONE_LATENCY_RELEASE_MASK >> core & 1u)) configASSERT(result.core[core].metric[SS_LAT_IRQ].count);
#endif
    }
    configASSERT(result.wake_segments.release_mask == SUNSTONE_LATENCY_RELEASE_MASK);
#if configSS_LOCAL_TIMERS
    configASSERT(result.wake_segments.scan_mask == 1u);
#else
    configASSERT(result.wake_segments.scan_mask == 0xffu);
#endif
    /* A tick is complete only when no core still held its previous wake, so a stalled core on a loaded host leaves few: the
     * first and second complete ticks each exist at most once, and at least one was filed. */
    configASSERT(result.wake_segments.tick[0].count + result.wake_segments.tick[1].count &&
                 result.wake_segments.first.count == 1 &&
                 result.wake_segments.second.count == (result.wake_segments.tick[0].count + result.wake_segments.tick[1].count > 1));
    platform_puts("PASS wake segment model: linked busy wake chains on every core and filed busy ticks; functional evidence only\n");
#endif
#ifdef SUNSTONE_LATENCY_AXI_CAP
    /* Stage 3a: the warm-up and the U B B U sub-phases ran in order, every operation succeeded, and each sub-phase ended at
     * its quota of busy wakes on every core. */
    configASSERT(result.axi_cap.magic == LATENCY_AXI_MAGIC && result.axi_cap.complete && result.axi_footer[0] == LATENCY_AXI_FOOTER_MAGIC);
    configASSERT(result.axi_cap.switch_count == 8);
    for (unsigned i = 0; i < result.axi_cap.switch_count; ++i) configASSERT(result.axi_cap.switches[i].status == LATENCY_AXI_OK);
    for (unsigned k = 0; k < LATENCY_AXI_SUBPHASES; ++k) {
        const struct latency_axi_window *w = &result.axi_cap.sub[k];
        number("axi subphase=", k);
        number(" ticks=", w->end - w->start);
        platform_puts("\n");
        configASSERT(w->start < w->end && (!k || result.axi_cap.sub[k - 1].end <= w->start));
    }
    platform_puts("PASS axi cap model: warm-up, U B B U sub-phases and every cap operation completed; functional evidence only\n");
#endif
#ifdef SUNSTONE_SCHED_PMU
    configASSERT(result.pmu_windows.magic == SS_SCHED_PMU_MAGIC);
    configASSERT(!(result.pmu_windows.flags & 8));
    for (unsigned core = 0; core < LATENCY_CORES; ++core) {
        const struct ss_sched_pmu_core *pmu = &result.pmu_windows.core[core];
        configASSERT(pmu->status == SS_SCHED_PMU_OK && pmu->completed);
        uint64_t total = 0;
        for (unsigned kind = 0; kind < SS_SCHED_PMU_KINDS; ++kind)
            for (unsigned bank = 0; bank < SS_SCHED_PMU_BANKS; ++bank)
                total += pmu->row[kind][bank].count;
        configASSERT(total == pmu->completed);
    }
    platform_puts("PASS scheduler PMU model: eight frozen writers and coherent sample totals; functional evidence only\n");
#endif
#ifdef SUNSTONE_LATENCY_CONTROL
    configASSERT(result.control.magic == LATENCY_CONTROL_MAGIC);
    configASSERT(result.control.mode == SUNSTONE_LATENCY_CONTROL_MODE);
    configASSERT(result.control.requested_mask == 0xfe && !(result.control.ready_mask & ~0xfeu));
    configASSERT(result.control.quiescent_mask == 0xfe && result.control.consumer_parked);
    configASSERT(ulSunstoneControlTasksSuspended());
    const unsigned control_complete = !result.control.errors && result.control.complete_mask == 0xfe;
    configASSERT(control_complete || result.control.errors);
    if (control_complete) configASSERT(result.control.ready_mask == 0xfe);
    number("control_errors=", result.control.errors);
    number(" control_complete_mask=", result.control.complete_mask);
    platform_puts("\n");
    for (unsigned c = 0; c < LATENCY_CONTROL_COUNT; ++c) {
        const struct latency_control_core *row = &result.control.core[c];
        if (result.core[c + 1].metric[SS_LAT_IRQ].count)
            platform_panic("control PPI reached base tick recorder", c + 1);
        configASSERT(row->core == c + 1);
        if (control_complete) configASSERT(row->published_control > LATENCY_CONTROL_MIN_SAMPLES);
        number("control_core=", row->core);
        number(" rate_hz=", row->rate_hz);
        number(" published_control=", row->published_control);
        number(" published_task=", row->published_task);
        number(" errors=", row->error_flags);
        platform_puts("\n");
        configASSERT(row->published_control == row->stats[0].count);
        uint64_t sums[LATENCY_CONTROL_HISTOGRAMS] = {0};
        for (unsigned h = 0; h < LATENCY_CONTROL_HISTOGRAMS; ++h)
            for (unsigned b = 0; b < SS_CONTROL_BINS; ++b) sums[h] += row->histogram[h][b];
        configASSERT(sums[LATENCY_CONTROL_IRQ_JITTER] == (row->stats[0].count ? row->stats[0].count - 1 : 0));
        if (result.control.mode == LATENCY_CONTROL_BUSY) {
            configASSERT(row->paths == 3);
            if (control_complete) configASSERT(row->published_task >= LATENCY_CONTROL_MIN_SAMPLES);
            configASSERT(row->published_task == row->stats[1].count);
            configASSERT(sums[LATENCY_CONTROL_TASK_LATENCY] == row->published_task);
            configASSERT(sums[LATENCY_CONTROL_TASK_GAP] == row->published_task);
        } else {
            configASSERT(row->paths == 1 && !row->published_task);
            configASSERT(!sums[LATENCY_CONTROL_TASK_LATENCY] && !sums[LATENCY_CONTROL_TASK_GAP]);
        }
    }
    if (control_complete)
        platform_puts("PASS control qualification model: seven owned timer paths, declared counts, histogram totals and suspended writers; functional evidence only\n");
    else
        platform_puts("PASS control error shutdown model: published runtime error, coherent histograms and suspended writers; sample qualification incomplete\n");
#endif
#ifdef SUNSTONE_SCHED_LATENCY_TRACE
    configASSERT(result.scheduler.magic == SS_SCHED_TRACE_MAGIC);
    configASSERT(result.scheduler.version == SS_SCHED_TRACE_VERSION);
    configASSERT(result.scheduler.history_capacity == SS_SCHED_TRACE_HISTORY);
    configASSERT(result.scheduler.tick_count && result.scheduler.tick_count <= SS_SCHED_TRACE_TICKS);
    configASSERT(!result.scheduler.tick_dropped || result.scheduler.tick_count == SS_SCHED_TRACE_TICKS);
    unsigned actual_fifo_cores = 0, linked_wakes = 0;
    for (unsigned c = 0; c < LATENCY_CORES; ++c) {
        const struct ss_sched_trace_core *trace = &result.scheduler.core[c];
#ifdef SUNSTONE_LATENCY_RESTORE_SPANS
        extern uint64_t ss_latency_first_return_seen[8];
        configASSERT(ss_latency_first_return_seen[c] == 1);
        configASSERT(result.scheduler.startup[c][1] >= result.scheduler.startup[c][0]);
#endif
        configASSERT(trace->command_count && trace->drain_count && trace->irq_count);
        configASSERT(trace->self_suspend.command == 2);
        configASSERT((trace->self_suspend.flags & 5) == 5);
#ifdef SS_SCHED_TRACE_TEST_EXIT_SUSPEND
        /* CPU1's memory task is parked at an SGI exit (workload.c). The kept
         * row is the longest self-suspension, so check the exit's own record. */
        if (c == 1) {
            const struct ss_sched_trace_command *parked = &ss_sched_trace_test_exit_parked[1];
            configASSERT(parked->command == 2 && parked->flags == 197);
            for (unsigned i = 1; i < 7; ++i) configASSERT(parked->stamp[i] >= parked->stamp[i - 1]);
            configASSERT(parked->stamp[3] == parked->stamp[4] && parked->stamp[6]);
        }
#endif
        configASSERT(trace->self_suspend.stamp[0] && trace->self_suspend.stamp[6]);
        for (unsigned i = 1; i < 7; ++i)
            configASSERT(trace->self_suspend.stamp[i] >= trace->self_suspend.stamp[i - 1]);
        configASSERT(!trace->drain.dropped);
        for (unsigned i = 0; i < trace->drain.count; ++i) {
            configASSERT(trace->drain.record[i].stamp[0] >= trace->drain.start);
            configASSERT(trace->drain.record[i].stamp[4] <= trace->drain.end);
        }
        bool actual = false;
        for (unsigned i = 0; i <= trace->wait_count; ++i) {
            const struct ss_sched_trace_wait *wait = i ? &trace->wait[i - 1] : &trace->wait_max;
            if (!(wait->flags & SS_SCHED_WAIT_ACTUAL_FIFO)) continue;
            actual = true;
            configASSERT(wait->lock == 0 && wait->fifo_start);
            configASSERT(wait->start <= wait->fifo_start && wait->fifo_start <= wait->fifo_end && wait->fifo_end <= wait->end);
            configASSERT(wait->fifo_serving <= 0xffff && wait->fifo_ticket <= 0xffff);
        }
        actual_fifo_cores += actual;
        const struct ss_sched_trace_wake_core *row = &result.scheduler.wake[c];
        const struct ss_sched_trace_wake *w = &row->maximum;
        const struct ss_sched_trace_wake_history *history = &result.scheduler.history[c];
        unsigned count = 0;
        for (unsigned kind = 0; kind < 3; ++kind) {
            count += row->summary[kind][0].count;
            if (kind < 2) linked_wakes += row->summary[kind][0].count;
            configASSERT(row->summary[kind][1].count == (kind == 2 ? 0 : row->summary[kind][0].count));
        }
        configASSERT(count == LATENCY_BUSY_SAMPLES);
        number("wake-chain core=", c);
        number(" fast=", row->summary[0][0].count);
        number(" slow=", row->summary[1][0].count);
        number(" unmatched=", row->summary[2][0].count);
        number(" max_flags=", w->flags);
        number(" irq_result=", w->irq.result);
        number(" irq_seq=", w->irq.sequence);
        number(" switch_irq_seq=", w->selection.irq_sequence);
        number(" irq_end=", w->irq.stamp[4]);
        number(" switch_begin=", w->selection.begin);
        number(" latest_pre_running_irq=", history->latest_sequence);
        number(" trailing_irqs=", history->trailing_count);
        number(" trailing_work=", history->trailing_work);
        number(" future_irqs=", history->future_irqs);
        number(" irq_overwritten=", history->irq_overwritten);
        number(" switch_overwritten=", history->switch_overwritten);
        platform_puts("\n");
        configASSERT(w->running - w->ready == result.phases[SS_LAT_BUSY][c].metric[SS_LAT_SCHED_BUSY].maximum);
        configASSERT(w->ready >= result.phase_bounds[SS_LAT_BUSY][0] && w->running <= result.phase_bounds[SS_LAT_BUSY][1]);
        if (w->flags & SS_SCHED_WAKE_LINKED) {
            configASSERT((w->flags & (SS_SCHED_WAKE_IRQ | SS_SCHED_WAKE_SWITCH)) == 3);
            configASSERT(!(w->flags & (SS_SCHED_WAKE_MULTIPLE | SS_SCHED_WAKE_UNMATCHED)));
            configASSERT(w->irq.previous_end <= w->ready && w->selection.previous_end <= w->ready);
            for (unsigned i = 1; i < 5; ++i) configASSERT(w->irq.stamp[i - 1] <= w->irq.stamp[i]);
            configASSERT(w->ready <= w->irq.stamp[4] && w->irq.stamp[4] <= w->selection.begin);
            configASSERT(w->selection.begin <= w->selection.end && w->selection.end <= w->running);
            configASSERT(w->selection.selected_slot == w->running_slot && w->selection.irq_sequence == w->irq.sequence);
            configASSERT(!history->trailing_count && !(w->flags & SS_SCHED_WAKE_HISTORY_LOST));
        }
        if (c == 0 && !row->summary[0][0].count && !row->summary[1][0].count &&
            !(w->flags & SS_SCHED_WAKE_SNAPSHOT_INCOMPLETE)) {
            /* An entirely unmatched primary must still expose real measured
             * interference and its selecting exit, not observation-side loss. */
            configASSERT((w->flags & SS_SCHED_WAKE_MULTIPLE) && !(w->flags & SS_SCHED_WAKE_HISTORY_LOST));
            if (w->irq.sequence) configASSERT(w->irq.sequence == w->selection.irq_sequence);
            else configASSERT(!w->selection.irq_sequence && w->selection.previous_end > w->ready);
            configASSERT(history->latest_end <= w->running);
            configASSERT(history->trailing_count || w->irq.previous_end > w->ready || w->selection.previous_end > w->ready);
        }
    }
    const struct ss_sched_trace_cpu0_window *window = &result.scheduler.cpu0_window;
    configASSERT(window->capacity == SS_SCHED_TRACE_HISTORY);
    /* A late observer can find every retained row replaced by later dispatches;
     * the window then says so instead of holding rows. */
    configASSERT(window->irq_count || (window->flags & SS_SCHED_WINDOW_IRQ_OVERRUN));
    configASSERT(window->irq_count <= window->capacity && window->switch_count <= window->capacity);
    configASSERT(!(window->flags & SS_SCHED_WINDOW_IRQ_OVERRUN) || window->irq_count < window->capacity);
    configASSERT(!(window->flags & ~(SS_SCHED_WINDOW_IRQ_PREFIX_LOST | SS_SCHED_WINDOW_SWITCH_PREFIX_LOST |
        SS_SCHED_WINDOW_INCOMPLETE | SS_SCHED_WINDOW_IRQ_OVERRUN)));
    number("cpu0_window_irqs=", window->irq_count);
    number(" selections=", window->switch_count);
    number(" flags=", window->flags);
    platform_puts("\n");
    for (unsigned i = 0; i < window->irq_count; ++i) {
        const struct ss_sched_trace_window_irq *event = &window->irq[i];
        configASSERT(event->begin && event->begin <= event->end && event->end <= result.scheduler.wake[0].maximum.running);
        configASSERT((event->descriptor & 0xffff) < 1020 && event->pc);
        if (event->sequence) configASSERT(event->end <= event->owner_begin && event->owner_begin <= event->owner_end);
        else configASSERT(!event->owner_begin && !event->owner_end);
    }
    number("actual_fifo_cores=", actual_fifo_cores);
    number(" linked_wakes=", linked_wakes);
    platform_puts("\n");
    configASSERT(actual_fifo_cores && linked_wakes);
    platform_puts("PASS scheduler trace: actual FIFO intervals, busy wake chains, self-suspends, drain records and timer intervals\n");
#endif
    platform_puts("MODEL latency: host QEMU timing; CPU0 timer IRQ releases eight pinned tasks; DMA hardware modeled\n");
    platform_puts("DMA model stimulus: continuous 4096-byte jobs; client priority1 and service priority4 on CPU0\n");
    number("frequency=", result.frequency);
    number(" elapsed_ticks=", result.elapsed_ticks);
    number(" modeled_DMA_jobs=", result.dma_jobs);
    platform_puts("\n");
    for (unsigned c = 0; c < LATENCY_CORES; ++c) {
        configASSERT(result.core[c].metric[SS_LAT_SCHED_IDLE].count == LATENCY_IDLE_SAMPLES);
        configASSERT(result.core[c].metric[SS_LAT_SCHED_BUSY].count == LATENCY_BUSY_SAMPLES);
        number("core=", c);
        number(" sweeps=", result.traffic[c].sweeps);
        number(" bytes_read=", result.traffic[c].bytes_read);
        number(" bytes_written=", result.traffic[c].bytes_written);
        number(" digest=", result.traffic[c].digest);
        platform_puts("\n");
        number("isr_acquisitions=", result.acquisitions[c][0]);
        number(" task_acquisitions=", result.acquisitions[c][1]);
        platform_puts("\n");
        for (unsigned i = 0; i < LATENCY_IDLE_SAMPLES + LATENCY_BUSY_SAMPLES; ++i) {
            const struct latency_wake_sample *w = &result.wakes[c][i];
            unsigned p = i < LATENCY_IDLE_SAMPLES ? SS_LAT_IDLE : SS_LAT_BUSY;
            configASSERT(w->metric == (p == SS_LAT_IDLE ? SS_LAT_SCHED_IDLE : SS_LAT_SCHED_BUSY));
            configASSERT(w->workload_flags == (p == SS_LAT_IDLE ? LATENCY_WAKE_IDLE : LATENCY_WAKE_BUSY));
            configASSERT(result.phase_bounds[p][0] <= w->ready && w->ready <= w->running);
            configASSERT(w->running <= result.phase_bounds[p][1]);
        }
        for (unsigned p = 0; p < SS_LAT_PHASES; ++p)
            for (unsigned m = 0; m < SS_LAT_METRICS; ++m) {
                const struct ss_latency_distribution *d = &result.phases[p][c].metric[m];
                number("phase=", p); number(" metric=", m);
                number(" count=", d->count);
                number(" p50_upper_ticks=", d->p50_upper);
                number(" p99_upper_ticks=", d->p99_upper);
                number(" max_ticks=", d->maximum);
                number(" max_start=", d->start); number(" max_site=", d->site);
                number(" overflow_bin_count=", d->overflow_bin_count);
                platform_puts("\n");
            }
        for (unsigned p = 0; p <= SS_LAT_PHASES; ++p)
            for (unsigned k = 0; k < SS_LAT_SITE_KINDS; ++k) {
                uint64_t previous = UINT64_MAX;
                for (unsigned i = 0; i < SS_LAT_SITE_SLOTS; ++i) {
                    const struct ss_latency_site *s = &result.sites[p][c][k][i];
                    configASSERT(s->ticks <= previous);
                    previous = s->ticks;
                    if (!s->ticks) {
                        configASSERT(!s->start && !s->enter_site && !s->exit_site);
                        continue;
                    }
                    configASSERT(s->enter_site && s->exit_site);
                    unsigned metric = k == 0 ? SS_LAT_MASKED : SS_LAT_LOCK_HOLD1;
                    configASSERT(s->ticks <= result.core[c].metric[metric].maximum);
                    if (p < SS_LAT_PHASES) {
                        configASSERT(result.phase_bounds[p][0] <= s->start);
                        configASSERT(s->start < result.phase_bounds[p][1]);
                        configASSERT(s->ticks <= result.phase_bounds[p][1] - s->start);
                    }
#ifdef SUNSTONE_LATENCY_PMU
                    if (k == 0 && (p == SS_LAT_BUSY || p == SS_LAT_PHASES)) {
                        const struct ss_latency_site_pmu *q = &result.site_pmu[p == SS_LAT_BUSY ? 0 : 1][c][i];
                        number("site_pmu instructions=", q->instructions); number(" cycles=", q->cycles);
                        number(" stall_backend=", q->stall_backend); number(" mem_access=", q->mem_access);
                        number(" bus_access=", q->bus_access); number(" l1d_refills=", q->l1d_refills);
                        platform_puts("\n");
                    }
#endif
                    number("site_phase=", p); number(" kind=", k); number(" slot=", i);
                    number(" ticks=", s->ticks); number(" start=", s->start);
                    number(" enter_site=", s->enter_site); number(" exit_site=", s->exit_site);
                    platform_puts("\n");
                }
            }
        for (unsigned m = 0; m < SS_LAT_METRICS; ++m) {
            const struct ss_latency_metric *sample = &result.core[c].metric[m];
            platform_puts(metric_names[m]);
            number(" count=", sample->count);
            number(" max_ticks=", sample->maximum);
            number(" max_start=", sample->start);
            number(" max_site=", sample->site);
            number(" first_over_start=", sample->first_over_start);
            number(" first_over_ticks=", sample->first_over_ticks);
            number(" first_over_site=", sample->first_over_site);
            platform_puts("\n");
        }
    }
    configASSERT(result.core[0].metric[SS_LAT_IRQ].count);
#ifndef SUNSTONE_LATENCY_CPU_ONLY
    configASSERT(result.core[0].metric[SS_LAT_DMA].count);
#endif
    platform_puts("PASS latency model: 64 idle and 128 loaded wake samples on each of eight cores; sequential 1 MiB/core traffic and real DMA service with modeled completion\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }
void kernel_main(void)
{
    platform_init();
    uint64_t frequency;
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(frequency));
    ss_latency_init(frequency);
    latency_workload_init();
#ifndef SUNSTONE_LATENCY_CPU_ONLY
    latency_dma_model_init();
#endif
    configASSERT(xTaskCreateStaticAffinitySet(controller, "latency-control", 2048, NULL, 5,
        controller_stack, &controller_state, 1));
    vTaskStartScheduler();
    platform_panic("latency scheduler returned", 0);
}
