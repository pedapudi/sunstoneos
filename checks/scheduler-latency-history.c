/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Execute the recorder with constructed counter values. This checks event
 * association and retention, not hardware timing or scheduler behavior. */
#include "../kernel/scheduler/latency_trace.h"
#include <assert.h>
#include <stdio.h>
static uint64_t clock_value;
static unsigned inject_history, history_reads;
uint64_t ss_sched_trace_test_now(void) { return clock_value; }
uint64_t ss_latency_counter_frequency(void) { return 24000000; }
static void irq_result(unsigned core, uint64_t start, unsigned result)
{
    uint64_t dispatch = 0;
    if (!core) {
        clock_value = start - 10;
        dispatch = ss_sched_trace_dispatch_begin(0, 30, 0x41001000, 0x3c0);
        clock_value = start - 1; ss_sched_trace_dispatch_end(0, dispatch);
    }
    ss_sched_trace_irq_begin(core, start);
    clock_value = start + 1; ss_sched_trace_irq_guard(core);
    clock_value = start + 2; ss_sched_trace_irq_stamp(core, 1);
    if (!core && start == 110) {
        /* Owner work opens IRQ entry after the port nesting count reaches
         * zero. A guarded inner dispatch must not replace its owner record. */
        clock_value = start + 3;
        uint64_t inner = ss_sched_trace_dispatch_begin(0, 27, 0x41004000, 0x3c5);
        clock_value = start + 4; ss_sched_trace_dispatch_end(0, inner);
    }
    clock_value = start + 8; ss_sched_trace_irq_stamp(core, 2);
    clock_value = start + 10; ss_sched_trace_irq_stamp(core, 3);
    ss_sched_trace_irq_end(core, result);
}
static void irq(unsigned core, uint64_t start) { irq_result(core, start, 1); }
static void select_task(unsigned core, uint64_t begin, unsigned slot)
{
    clock_value = begin; ss_sched_trace_switch_begin(core);
    clock_value = begin + 5; ss_sched_trace_switch_end(core, slot);
}
void ss_sched_trace_test_history_read(unsigned core, unsigned kind, uint64_t sequence)
{
    (void)kind; (void)sequence;
    if (core) return;
    ++history_reads;
    unsigned mode = inject_history;
    inject_history = 0;
    if (mode) for (unsigned i = 0; i < (mode == 1 ? 1u : SS_SCHED_TRACE_HISTORY); ++i) {
        uint64_t start = 20000 + i * 40;
        irq(0, start); select_task(0, start + 20, 4);
    }
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    /* A preexisting port yield request can select after owner result zero. */
    irq_result(0, 110, 0); select_task(0, 130, 3);
    for (unsigned i = 0; i < 3; ++i) irq(0, 160 + i * 20);
    ss_sched_trace_wake_sample(0, 100, 150, 3);
    static struct ss_sched_trace_report first;
    ss_sched_trace_snapshot(&first);
    assert(first.cpu0_window.irq_count == 2 && first.cpu0_window.switch_count == 1);
    assert(first.cpu0_window.irq[0].sequence == 1 && first.cpu0_window.irq[0].descriptor == (30 | (UINT64_C(0x3c0) << 16)));
    assert(first.cpu0_window.irq[0].begin == 100 && first.cpu0_window.irq[0].end == 109);
    assert(first.cpu0_window.irq[0].owner_begin == 110 && first.cpu0_window.irq[0].owner_end == 120);
    assert(!first.cpu0_window.irq[1].sequence && !first.cpu0_window.irq[1].owner_begin);
    assert(first.cpu0_window.selection[0].irq_sequence == 1 && !first.cpu0_window.flags);

    irq(0, 260); select_task(0, 280, 3);
    clock_value = 300;
    uint64_t guarded = ss_sched_trace_dispatch_begin(0, 27, 0x41002000, 0x3c5);
    clock_value = 305; ss_sched_trace_dispatch_end(0, guarded);
    clock_value = 310;
    uint64_t sgi = ss_sched_trace_dispatch_begin(0, 0, 0x41003000, 0x3c0);
    clock_value = 319; ss_sched_trace_dispatch_end(0, sgi);
    ss_sched_trace_irq_begin(0, 320);
    clock_value = 321; ss_sched_trace_irq_guard(0);
    clock_value = 322; ss_sched_trace_irq_stamp(0, 1);
    clock_value = 328; ss_sched_trace_irq_stamp(0, 2);
    clock_value = 330; ss_sched_trace_irq_stamp(0, 3); ss_sched_trace_irq_end(0, 0);
    select_task(0, 350, 3);
    extern _Atomic uint64_t ss_sched_trace_switch_irq[SS_SCHED_TRACE_CORES];
    ss_sched_trace_switch_irq[0] = 0; /* SVC entry's assembly store. */
    select_task(0, 370, 3);
    for (unsigned i = 0; i < 3; ++i) irq(0, 410 + i * 20);
    ss_sched_trace_wake_sample(0, 250, 400, 3);

    irq_result(1, 1010, 0); select_task(1, 1030, 3); irq_result(1, 1040, 0);
    ss_sched_trace_wake_sample(1, 1000, 1060, 3);

    irq(2, 2010); select_task(2, 2030, 3);
    for (unsigned i = 0; i < SS_SCHED_TRACE_HISTORY; ++i) irq(2, 2060 + i * 20);
    ss_sched_trace_wake_sample(2, 2000, 2050, 3);

    irq(3, 3010); select_task(3, 3030, 3);
    irq(3, 3040); select_task(3, 3060, 4);
    ss_sched_trace_wake_sample(3, 3000, 3080, 3);

    for (unsigned i = 0; i <= SS_SCHED_TRACE_HISTORY; ++i) irq(4, 4000 + i * 20);
    ss_sched_trace_irq_begin(4, 4410);
    clock_value = 4411; ss_sched_trace_irq_guard(4); ss_sched_trace_irq_fast(4);
    select_task(4, 4430, 3);
    ss_sched_trace_wake_sample(4, 4400, 4450, 3);

    irq(5, 5010); select_task(5, 5030, 3);
    irq(5, 5060); select_task(5, 5080, 4);
    irq(5, 5100); select_task(5, 5120, 3);
    ss_sched_trace_wake_sample(5, 5000, 5050, 3);

    irq(6, 6010); select_task(6, 6030, 3);
    for (unsigned i = 0; i < SS_SCHED_TRACE_HISTORY; ++i) {
        irq(6, 6060 + i * 40); select_task(6, 6080 + i * 40, 4);
    }
    ss_sched_trace_wake_sample(6, 6000, 6050, 3);

    irq(7, 7010); irq(7, 7030); select_task(7, 7050, 3);
    ss_sched_trace_wake_sample(7, 7000, 7070, 3);
    struct ss_sched_trace_command direct = {.command = 2, .slot = 3, .flags = 5,
        .stamp = {8000, 8010, 8020, 8100}};
    ss_sched_trace_direct_suspend_begin(7, &direct);
    clock_value = 8110; ss_sched_trace_direct_suspend_mark(7);
    clock_value = 8130; ss_sched_trace_switch_begin(7);
    clock_value = 8150; ss_sched_trace_switch_end(7, 4);
    direct = (struct ss_sched_trace_command){.command = 2, .slot = 3, .flags = 5,
        .stamp = {9000, 9010, 9020, 9030}};
    ss_sched_trace_direct_suspend_begin(7, &direct);
    clock_value = 9040; ss_sched_trace_direct_suspend_mark(7);
    clock_value = 9060; ss_sched_trace_switch_begin(7);
    clock_value = 9090; ss_sched_trace_switch_end(7, 4);
    direct = (struct ss_sched_trace_command){.command = 2, .slot = 3, .flags = 5,
        .stamp = {9100, 9110, 9120, 10000}};
    ss_sched_trace_direct_suspend_begin(7, &direct);
    clock_value = 10010; ss_sched_trace_direct_suspend_mark(7);
    clock_value = 10020; ss_sched_trace_switch_begin(7);
    clock_value = 10040; ss_sched_trace_switch_end(7, 4);
    static struct ss_sched_trace_report report;
    ss_sched_trace_snapshot(&report);
    assert(report.version == 5 && report.history_capacity == SS_SCHED_TRACE_HISTORY);
    assert(first.wake[0].maximum.flags == 7 && first.wake[0].maximum.irq.sequence == 1);
    assert(first.wake[0].maximum.irq.result == 0 && first.wake[0].summary[1][0].count == 1);
    assert(first.history[0].latest_sequence == 1 && first.history[0].future_irqs == 3);
    assert(report.wake[0].maximum.flags == 26 && !report.wake[0].maximum.selection.irq_sequence);
    assert(report.cpu0_window.irq_count == 8 && report.cpu0_window.switch_count == 4 && !report.cpu0_window.flags);
    assert(report.cpu0_window.irq[6].descriptor == (27 | (UINT64_C(0x3c5) << 16)));
    assert(!report.cpu0_window.irq[6].sequence && !report.cpu0_window.irq[6].owner_begin);
    assert(report.cpu0_window.irq[7].descriptor == (UINT64_C(0x3c0) << 16));
    assert(report.cpu0_window.irq[7].sequence == 6 && report.cpu0_window.irq[7].owner_begin == 320);
    assert(report.cpu0_window.selection[3].begin == 370 && !report.cpu0_window.selection[3].irq_sequence);
    assert(report.wake[1].maximum.flags == 27 && report.wake[1].maximum.irq.sequence == 1);
    assert(report.wake[1].maximum.irq.result == 0 && report.wake[1].summary[2][0].count == 1);
    assert(report.history[1].latest_sequence == 2 && report.history[1].trailing_count == 1 && report.history[1].trailing_work == 6);
    assert(report.wake[2].maximum.flags == 50 && report.wake[2].maximum.selection.irq_sequence == 1);
    assert(report.history[2].irq_overwritten == 1 && report.history[2].future_irqs == SS_SCHED_TRACE_HISTORY);
    assert(report.wake[3].maximum.flags == 27 && report.wake[3].maximum.selection.selected_slot == 4);
    assert(report.wake[4].maximum.flags == 7 && report.history[4].irq_overwritten == 2);
    assert(report.wake[5].maximum.flags == 7 && report.wake[5].maximum.selection.sequence == 1);
    assert(report.wake[6].maximum.flags == 48 && report.history[6].switch_overwritten == 1);
    assert(report.wake[7].maximum.flags == 27 && !report.history[7].trailing_count);
    assert(report.core[7].self_suspend.flags == 69 && report.core[7].self_suspend.stamp[6] == 9090 &&
           "direct maximum must use masked C duration rather than unmasked delay");
    assert(report.core[7].self_suspend.stamp[4] == 9040 && report.core[7].self_suspend.stamp[5] == 9060);
    assert(report.core[7].command.stamp[6] == 10040);
    assert(report.core[7].command_stage_max[0] == 10 && report.core[7].command_stage_max[1] == 10);
    assert(!report.core[7].command_stage_max[2] && !report.core[7].command_stage_max[3]);
    FILE *file = fopen(argv[1], "wb"); assert(file);
    assert(fwrite(&report, sizeof(report), 1, file) == 1);
    static const unsigned char padding[64];
    size_t bytes = (64 - sizeof(report) % 64) % 64;
    assert(fwrite(padding, 1, bytes, file) == bytes);
    assert(fclose(file) == 0);
    inject_history = 1; history_reads = 0;
    ss_sched_trace_wake_sample(0, 250, 450, 3);
    static struct ss_sched_trace_report interrupted;
    ss_sched_trace_snapshot(&interrupted);
    assert(!(interrupted.wake[0].maximum.flags & SS_SCHED_WAKE_SNAPSHOT_INCOMPLETE));
    assert(interrupted.history[0].latest_end <= 450 && history_reads <= 2 * SS_SCHED_TRACE_HISTORY);
    inject_history = 2; history_reads = 0;
    ss_sched_trace_wake_sample(0, 300, 700, 3);
    ss_sched_trace_snapshot(&interrupted);
    assert(interrupted.wake[0].maximum.flags == (SS_SCHED_WAKE_UNMATCHED | SS_SCHED_WAKE_SNAPSHOT_INCOMPLETE));
    assert(interrupted.cpu0_window.flags & SS_SCHED_WINDOW_INCOMPLETE);
    assert(history_reads <= 2 * SS_SCHED_TRACE_HISTORY);
    file = fopen(argv[2], "wb"); assert(file);
    assert(fwrite(&interrupted, sizeof(interrupted), 1, file) == 1);
    assert(fwrite(padding, 1, bytes, file) == bytes);
    assert(fclose(file) == 0);
    for (unsigned core = 0; core < SS_SCHED_TRACE_CORES; ++core) ss_sched_trace_freeze(core);
    puts("scheduler wake history: future events, trailing IRQs, exact sequence loss, slot identity and rotation passed");
    return 0;
}
