/* SPDX-License-Identifier: BSD-3-Clause */
/* Forced EL1 table walks on a pinned Cortex-A76 task in the OS image. */
#include "table_walks-app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "a733_cpu_coherence.h"
#include "a733_cpu_pmu.h"
#include "result.h"
#include <stdatomic.h>
#include <string.h>
static struct table_walk_result result;
static StaticTask_t worker_state;
static StackType_t worker_stack[1024];
static TaskHandle_t worker_task, main_task;
static atomic_uint complete;
void table_walks_worker(void *arg);

void table_walks_init(void)
{
    worker_task = xTaskCreateStaticAffinitySet(table_walks_worker, "table-walks", 1024, NULL, 3,
        worker_stack, &worker_state, 1u << 6);
    if (!worker_task) ss_fail(0x7a01);
}
void table_walks_worker(void *arg)
{
    (void)arg;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, tcr_el1\n mrs %2, midr_el1\n mrs %3, cntfrq_el0"
        : "=r"(el), "=r"(result.tcr), "=r"(result.midr), "=r"(result.counter_hz));
    result.core = portGET_CORE_ID();
    if (el != 4 || result.core != 6 || result.tcr != A733_CPU_COHERENCE_TCR ||
        ((result.midr >> 4) & 0xfff) != 0xd0b || result.counter_hz != 24000000) ss_fail(0x7a02);
    result.iterations = TABLE_WALKS_ITERATIONS;
    for (unsigned i = 0; i < TABLE_WALKS_SAMPLES; ++i) {
        struct a733_cpu_pmu_region region;
        struct a733_cpu_pmu_record counters;
        if (a733_cpu_pmu_begin(&region)) ss_fail(0x7a03);
        uint64_t daif;
        __asm__ volatile("mrs %0, daif\n msr daifset, #2" : "=r"(daif) :: "memory");
        table_walk_loop(&result.sample[i], TABLE_WALKS_ITERATIONS);
        __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
        if (a733_cpu_pmu_end(&region, &counters) || counters.core != 6 ||
            (counters.supported & 0x38) != 0x38 || counters.overflow) ss_fail(0x7a04);
        ss_progress();
    }
    atomic_store_explicit(&complete, 1, memory_order_release);
    xTaskNotifyGive(main_task);
    vTaskSuspend(NULL);
}
void table_walks_main(const struct ss_grant *g)
{
    if (!g || g->main_core != 6) ss_fail(0x7a05);
    main_task = xTaskGetCurrentTaskHandle();
    while (!ss_clock_ready() || ss_throttled()) {
        if (ss_live() < 0) ss_fail(0x7a06);
        ss_progress();
        vTaskDelay(1);
    }
    memcpy(result.magic, TABLE_WALKS_MAGIC, 8);
    if (ss_clock_info(result.clock_before, sizeof result.clock_before) <= 0) ss_fail(0x7a07);
    ss_activity_begin();
    xTaskNotifyGive(worker_task);
    while (!atomic_load_explicit(&complete, memory_order_acquire)) {
        if (ss_live() < 0) ss_fail(0x7a08);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    }
    if (!ss_clock_ready() || ss_throttled() ||
        ss_clock_info(result.clock_after, sizeof result.clock_after) <= 0) ss_fail(0x7a09);
    ss_activity_end(1);
    ss_finish(&result, sizeof result);
}
