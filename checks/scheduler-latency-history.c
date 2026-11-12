/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Execute the recorder with constructed counter values. This checks event
 * association and retention, not hardware timing or scheduler behavior. */
#include "../kernel/scheduler/latency_trace.h"
#include <assert.h>
#include <stdio.h>
static uint64_t clock_value;
uint64_t ss_sched_trace_test_now(void) { return clock_value; }
uint64_t ss_latency_counter_frequency(void) { return 24000000; }
static void irq_result(unsigned core, uint64_t start, unsigned result)
{
    ss_sched_trace_irq_begin(core, start);
    clock_value = start + 1; ss_sched_trace_irq_guard(core);
    clock_value = start + 2; ss_sched_trace_irq_stamp(core, 1);
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
int main(int argc, char **argv)
{
    assert(argc == 2);
    /* A preexisting port yield request can select after owner result zero. */
    irq_result(0, 110, 0); select_task(0, 130, 3);
    for (unsigned i = 0; i < 3; ++i) irq(0, 160 + i * 20);
    ss_sched_trace_wake_sample(0, 100, 150, 3);

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
    for (unsigned core = 0; core < SS_SCHED_TRACE_CORES; ++core) ss_sched_trace_freeze(core);
    static struct ss_sched_trace_report report;
    ss_sched_trace_snapshot(&report);
    assert(report.version == 4 && report.history_capacity == SS_SCHED_TRACE_HISTORY);
    assert(report.wake[0].maximum.flags == 7 && report.wake[0].maximum.irq.sequence == 1);
    assert(report.wake[0].maximum.irq.result == 0 && report.wake[0].summary[1][0].count == 1);
    assert(report.history[0].latest_sequence == 1 && report.history[0].future_irqs == 3);
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
    FILE *file = fopen(argv[1], "wb"); assert(file);
    assert(fwrite(&report, sizeof(report), 1, file) == 1);
    static const unsigned char padding[64];
    size_t bytes = (64 - sizeof(report) % 64) % 64;
    assert(fwrite(padding, 1, bytes, file) == bytes);
    assert(fclose(file) == 0);
    puts("scheduler wake history: future events, trailing IRQs, exact sequence loss, slot identity and rotation passed");
    return 0;
}
