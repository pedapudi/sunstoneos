/* SPDX-License-Identifier: Apache-2.0 */
/* Optional returning-image observation. Only the pinned main task calls these
 * wrappers. The IRQ owner configures the PMU before the first snapshot. */
#include "FreeRTOS.h"
#include "task.h"
#include "sunstone_app.h"
#include "../apps/os/application.h"
#include "pmu_result.h"
#include <string.h>

extern long ulSunstonePmuReady(unsigned core);
extern TaskHandle_t os_application_main_task(void);
extern void __real_ss_activity_begin(void);
extern void __real_ss_activity_end(int);
extern _Noreturn void __real_ss_finish(const void *, size_t);
static struct sunstone_pmu_result first, captured;
static unsigned active, complete;

static void require_main(void)
{
    TaskHandle_t main = os_application_main_task();
    if (!main || xTaskGetCurrentTaskHandle() != main) ss_fail(0x7b08);
}

static int snapshot(struct sunstone_pmu_result *out)
{
    uint64_t h0, lo, h1, cycles, data, ticks;
    __asm__ volatile("isb\n mrs %0, pmevcntr5_el0\n mrs %1, pmevcntr4_el0\n mrs %2, pmevcntr5_el0"
                     : "=r"(h0), "=r"(lo), "=r"(h1) :: "memory");
    if (h0 != h1) {
        /* One carry can cross the first sample. A second crossing refuses;
         * the read never waits indefinitely for a stable counter. */
        h0 = h1;
        __asm__ volatile("mrs %0, pmevcntr4_el0\n mrs %1, pmevcntr5_el0"
                         : "=r"(lo), "=r"(h1) :: "memory");
        if (h0 != h1) return 0;
    }
    __asm__ volatile("mrs %0, pmccntr_el0\n mrs %1, pmevcntr3_el0\n mrs %2, cntpct_el0"
                     : "=r"(cycles), "=r"(data), "=r"(ticks) :: "memory");
    *out = (struct sunstone_pmu_result){SUNSTONE_PMU_RESULT_MAGIC, ss_grant()->main_core,
        cycles, ((uint64_t)(uint32_t)h1 << 32) | (uint32_t)lo, (uint32_t)data, ticks};
    return 1;
}

static void ready(void)
{
    const struct ss_grant *g = ss_grant();
    if (!g || (g->main_core != 2 && g->main_core != 6) || portGET_CORE_ID() != g->main_core) ss_fail(0x7b01);
    for (;;) {
        long state = ulSunstonePmuReady(g->main_core);
        if (state == 1) return;
        if (state < 0 || ss_live() < 0) ss_fail(0x7b02);
        ss_progress();
        vTaskDelay(1);
    }
}

void __wrap_ss_activity_begin(void)
{
    require_main();
    if (active) ss_fail(0x7b03);
    ready();
    __real_ss_activity_begin();
    if (!snapshot(&first)) ss_fail(0x7b04);
    complete = 0;
    active = 1;
}

void __wrap_ss_activity_end(int success)
{
    require_main();
    if (!active || portGET_CORE_ID() != first.core || !snapshot(&captured)) ss_fail(0x7b05);
    captured.cycles -= first.cycles;
    captured.inst_retired -= first.inst_retired;
    captured.dtlb_walk = (uint32_t)(captured.dtlb_walk - first.dtlb_walk);
    captured.ticks -= first.ticks;
    active = 0;
    complete = 1;
    __real_ss_activity_end(success);
}

_Noreturn void __wrap_ss_finish(const void *data, size_t bytes)
{
    require_main();
    if (active) __wrap_ss_activity_end(1);
    if (!complete || !ss_application.manifest || (!data && bytes) ||
        ss_result_capacity < sizeof captured || bytes > ss_result_capacity - sizeof captured) ss_fail(0x7b06);
    unsigned char *result = pvPortMalloc(bytes + sizeof captured);
    if (!result) ss_fail(0x7b07);
    memcpy(result, &captured, sizeof captured);
    if (bytes) memcpy(result + sizeof captured, data, bytes);
    __real_ss_finish(result, bytes + sizeof captured);
}

