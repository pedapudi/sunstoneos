#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"

/* No switch request may be lost. On cores 1 to 7 an API-level timer interrupt wakes a priority 6 task from the
 * interrupt (xTaskNotifyGiveFromISR and portYIELD_FROM_ISR) over a busy priority 1 task, while, in the storm build,
 * an interrupt above the API mask lands every 33 us, many of them in the interrupt-exit path of the wake interrupt.
 * A lost request shows as a wake that waits for the next interrupt exit, 200 us later or more. The check counts,
 * per core, the wakes raised in the interrupt (woken true) and the task runs that served them, the latency from the
 * interrupt to the task in counter ticks, and the wakes later than half a wake period; at the end no switch request
 * may be pending. */
#define CORES configNUMBER_OF_CORES
#define WAKE_PERIODS 300U
_Static_assert(CORES == 8, "the check needs the eight-core configuration");

extern uint64_t ss_wake_period;
volatile unsigned ss_wake_stop;
extern uint64_t ullPortYieldRequired[];
static StaticTask_t high_tasks[CORES], busy_tasks[CORES], control_task;
static StackType_t high_stacks[CORES][1024], busy_stacks[CORES][1024], control_stack[2048];
static TaskHandle_t high[CORES];
static struct { uint64_t raised, served, late, max, started, sum; } stat[CORES] __attribute__((aligned(64)));
static uint64_t counter(void) { uint64_t v; __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v) :: "memory"); return v; }

void ss_wake_isr(unsigned core) {
    BaseType_t woken = pdFALSE;
    stat[core].started = counter();
    if (high[core]) vTaskNotifyGiveFromISR(high[core], &woken);
    if (woken) ++stat[core].raised;
    portYIELD_FROM_ISR(woken);
}
static void high_task(void *argument) {
    const unsigned core = (unsigned)(uintptr_t)argument;
    for (;;) {
        if (!ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) continue;
        const uint64_t latency = counter() - stat[core].started;
        ++stat[core].served;
        stat[core].sum += latency;
        if (latency > stat[core].max) stat[core].max = latency;
        if (latency > ss_wake_period / 2) ++stat[core].late;
    }
}
static void busy_task(void *argument) {
    (void)argument;
    for (;;) __asm__ volatile("nop");
}
static void control(void *argument) {
    (void)argument;
    ss_wake_period = ({ uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); f; }) / 2500; /* 400 us: with the eight cores on one emulator thread a wake takes about 60 us of virtual time */
    /* A core arms its wake timer on its first interrupt; a yield request interrupts each core once. */
    for (unsigned c = 1; c < CORES; ++c) portYIELD_CORE(c);
    while (__atomic_load_n(&stat[1].served, __ATOMIC_RELAXED) < WAKE_PERIODS) vTaskDelay(1);
    ss_wake_stop = 1;
    vTaskDelay(5);
    uint64_t raised = 0, served = 0, late = 0, worst = 0;
    for (unsigned c = 1; c < CORES; ++c) {
        raised += stat[c].raised; served += stat[c].served; late += stat[c].late;
        if (stat[c].max > worst) worst = stat[c].max;
        platform_puts("wake cpu="); platform_hex(c); platform_puts(" raised="); platform_hex(stat[c].raised);
        platform_puts(" served="); platform_hex(stat[c].served); platform_puts(" late="); platform_hex(stat[c].late);
        platform_puts(" max-ticks="); platform_hex(stat[c].max);
        platform_puts(" mean-ticks="); platform_hex(stat[c].served ? stat[c].sum / stat[c].served : 0);
        platform_puts(" pending="); platform_hex(ullPortYieldRequired[c]); platform_puts("\n");
        configASSERT(stat[c].raised == stat[c].served);
        configASSERT(ullPortYieldRequired[c] == 0);
    }
    platform_puts("wake-period-ticks="); platform_hex(ss_wake_period); platform_puts(" worst-ticks="); platform_hex(worst); platform_puts("\n");
#ifdef SUNSTONE_OWNER_COUNTERS
    {
        extern uint64_t ulSunstoneOwnerCounters(unsigned cpu, unsigned which);
        uint64_t skipped = 0, pending = 0;
        for (unsigned c = 0; c < CORES; ++c) {
            skipped += ulSunstoneOwnerCounters(c, 1); pending += ulSunstoneOwnerCounters(c, 9);
        }
        platform_puts("exit-skipped="); platform_hex(skipped); platform_puts(" skipped-with-request-pending="); platform_hex(pending); platform_puts("\n");
    }
#endif
    configASSERT(raised > WAKE_PERIODS && raised == served);
    configASSERT(late == 0);
    platform_puts("PASS yield: every wake raised in an interrupt ran its task within half a wake period, none left pending\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task; platform_panic(name, 0);
}
void kernel_main(void) {
    platform_init();
    for (unsigned c = 1; c < CORES; ++c) {
        high[c] = xTaskCreateStaticAffinitySet(high_task, "high", 1024, (void *)(uintptr_t)c, 6, high_stacks[c], &high_tasks[c], 1U << c);
        configASSERT(high[c]);
        configASSERT(xTaskCreateStaticAffinitySet(busy_task, "busy", 1024, 0, 1, busy_stacks[c], &busy_tasks[c], 1U << c));
    }
    configASSERT(xTaskCreateStaticAffinitySet(control, "control", 2048, 0, 5, control_stack, &control_task, 1));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
