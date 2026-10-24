/* Two-core QEMU fixture that counts the instructions of FreeRTOS SMP port
 * operations from an EL1 task and from an EL0 task.
 *
 * checks/smp-port-cost.py builds this file on platform/qemu-smp with the SMP
 * port and task patches and runs it under -icount shift=0, where the
 * INST_RETIRED event counter is exact and the virtual counter advances by one
 * nanosecond per instruction retired on either core.
 *
 * Each core's EL1 setup task programs event counter 0 for INST_RETIRED at EL0
 * and EL1, lets EL0 read it and lets EL0 execute WFI. The build adds
 * configUSE_PASSIVE_IDLE_HOOK, and after setup the idle tasks wait in WFI, as
 * on the A733, so that an idle core retires almost no instructions. A region is "isb; mrs" before and after the
 * operation; the empty region is subtracted, and the least of ATTEMPTS
 * samples is kept so that a tick inside one sample does not count. Each
 * result line is "MEASURE name=0x...". */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"

extern _Noreturn void platform_panic(const char *, unsigned);
#define CHECK(x) do { if (!(x)) platform_panic(__FILE__, __LINE__); } while (0)
#define ATTEMPTS 16u
#define ROUND_TRIPS 256u
#define EL1 ((void *)1)

/* Tasks created with parameter EL1 start at EL1; the others at EL0. */
BaseType_t npu_service_task_privileged(TaskFunction_t code, void *parameters);
BaseType_t npu_service_task_privileged(TaskFunction_t code, void *parameters)
{
    (void)code;
    return parameters == EL1;
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name);
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    platform_panic(name, 0);
}

static inline uint64_t retired(void)
{
    uint64_t v;
    __asm__ volatile("isb\n mrs %0, pmevcntr0_el0" : "=r"(v) :: "memory");
    return v;
}
static inline uint64_t virtual_time(void)
{
    uint64_t v;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}

/* EL1 only: INST_RETIRED in counter 0, counted at EL0 and EL1, readable at
 * EL0, and WFI permitted at EL0 (SCTLR_EL1.nTWI). */
static void counter_setup(void)
{
    __asm__ volatile("msr pmevtyper0_el0, %0\n msr pmcntenset_el0, %1\n msr pmuserenr_el0, %2\n"
                     "mrs x9, pmcr_el0\n orr x9, x9, #3\n msr pmcr_el0, x9\n"
                     "mrs x9, sctlr_el1\n orr x9, x9, #(1 << 16)\n msr sctlr_el1, x9\n isb"
                     :: "r"(UINT64_C(0x08)), "r"(UINT64_C(1)), "r"(UINT64_C(1)) : "x9", "memory");
}

static volatile uint32_t idle_wait;
void vApplicationPassiveIdleHook(void);
void vApplicationPassiveIdleHook(void)
{
    if (__atomic_load_n(&idle_wait, __ATOMIC_ACQUIRE)) __asm__ volatile("wfi" ::: "memory");
}

static void say(const char *label, uint64_t value)
{
    platform_puts("MEASURE ");
    platform_puts(label);
    platform_puts("=");
    platform_hex(value);
    platform_puts("\n");
}

static TaskHandle_t driver_handle, el0_handle, waker_handle, remote_handle, setup1_handle, remote0_handle;
static StaticTask_t tasks[6];
static StackType_t stacks[6][4096];
static volatile uint64_t woken_at, remote_first, remote_last, remote0_first, remote0_last;

#define LEAST(result, body) do {                                   \
        uint64_t least_ = UINT64_MAX;                              \
        for (unsigned i_ = 0; i_ < ATTEMPTS; ++i_) {               \
            const uint64_t start_ = retired();                     \
            body;                                                  \
            const uint64_t cost_ = retired() - start_;             \
            if (cost_ < least_) least_ = cost_;                    \
        }                                                          \
        (result) = least_;                                         \
    } while (0)

/* Operations common to both privilege levels. */
static void common(const char *level_empty, const char *level_critical, const char *level_current)
{
    uint64_t empty, critical, current;
    LEAST(empty, (void)0);
    LEAST(critical, { taskENTER_CRITICAL(); taskEXIT_CRITICAL(); });
    LEAST(current, CHECK(xTaskGetCurrentTaskHandle() != NULL));
    say(level_empty, empty);
    say(level_critical, critical - empty);
    say(level_current, current - empty);
}

/* Core 1 at EL0: returns each notification of the EL0 round trips. */
static void remote0(void *unused)
{
    (void)unused;
    for (unsigned round = 1;; ++round) {
        CHECK(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
        const uint64_t now = retired();
        if (round == 1) remote0_first = now;
        if (round == ROUND_TRIPS + 1) remote0_last = now;
        xTaskNotifyGive(el0_handle);
    }
}

static void el0_task(void *unused)
{
    (void)unused;
    common("el0_empty", "el0_critical_pair", "el0_current_task");
    /* Cross-core round trips between two EL0 tasks: each send keeps the
     * supervisor call and adds the task-register and nesting test. */
    __atomic_store_n(&idle_wait, 1, __ATOMIC_RELEASE);
    uint64_t first = 0;
    for (unsigned i = 0; i <= ROUND_TRIPS; ++i) {
        xTaskNotifyGive(remote0_handle);
        CHECK(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
        if (!i) first = retired();
    }
    const uint64_t core0 = retired() - first;
    __atomic_store_n(&idle_wait, 0, __ATOMIC_RELEASE);
    say("el0_cross_core_core0_instructions", core0);
    say("el0_cross_core_core1_instructions", remote0_last - remote0_first);
    xTaskNotifyGive(driver_handle);
    for (;;) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

/* Higher-priority task on core 0: records when the notification wakes it. */
static void waker(void *unused)
{
    (void)unused;
    for (;;) {
        CHECK(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
        woken_at = retired();
    }
}

/* Core 1: returns each notification to the driver and records its counter
 * when the first and the last notification of the round trips wake it. */
static void remote(void *unused)
{
    (void)unused;
    for (unsigned round = 1;; ++round) {
        CHECK(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
        const uint64_t now = retired();
        if (round == 1) remote_first = now;
        if (round == ROUND_TRIPS + 1) remote_last = now;
        xTaskNotifyGive(driver_handle);
    }
}

static void setup1(void *unused)
{
    (void)unused;
    counter_setup();
    xTaskNotifyGive(driver_handle);
    vTaskDelete(NULL);
}

static void driver(void *unused)
{
    (void)unused;
    counter_setup();
    CHECK(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1); /* core 1 counter ready */
    common("el1_empty", "el1_critical_pair", "el1_current_task");

    uint64_t empty, notify;
    LEAST(empty, (void)0);
    LEAST(notify, { xTaskNotifyGive(driver_handle); CHECK(ulTaskNotifyTake(pdTRUE, 0) == 1); });
    say("el1_notify_give_take", notify - empty);

    /* Give to the blocked higher-priority task on this core; it preempts. */
    uint64_t switch_least = UINT64_MAX;
    for (unsigned i = 0; i < ATTEMPTS; ++i) {
        const uint64_t start = retired();
        xTaskNotifyGive(waker_handle);
        const uint64_t cost = woken_at - start;
        if (cost < switch_least) switch_least = cost;
    }
    say("el1_give_switch", switch_least - empty);

    /* Cross-core round trips. Each core's counter is read at the same point
     * of the first and of the last round trip, so the difference covers every
     * instruction that core retires per round trip: the task, the SGI
     * handler, the context switches and the idle task's WFI loop. */
    __atomic_store_n(&idle_wait, 1, __ATOMIC_RELEASE);
    uint64_t first = 0, time = 0;
    for (unsigned i = 0; i <= ROUND_TRIPS; ++i) {
        xTaskNotifyGive(remote_handle);
        CHECK(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
        if (!i) { first = retired(); time = virtual_time(); }
    }
    time = virtual_time() - time;
    const uint64_t core0 = retired() - first;
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __atomic_store_n(&idle_wait, 0, __ATOMIC_RELEASE);
    say("cross_core_round_trips", ROUND_TRIPS);
    say("cross_core_core0_instructions", core0);
    say("cross_core_core1_instructions", remote_last - remote_first);
    say("cross_core_virtual_ticks", time);
    say("counter_frequency", frequency);

    vTaskResume(el0_handle);
    CHECK(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
    platform_puts("PASS SMP port cost fixture\n");
    platform_exit(0);
}

void kernel_main(void);
void kernel_main(void)
{
    platform_init();
    driver_handle = xTaskCreateStaticAffinitySet(driver, "driver", 4096, EL1, 3, stacks[0], &tasks[0], 1);
    waker_handle = xTaskCreateStaticAffinitySet(waker, "waker", 4096, EL1, 4, stacks[1], &tasks[1], 1);
    remote_handle = xTaskCreateStaticAffinitySet(remote, "remote", 4096, EL1, 3, stacks[2], &tasks[2], 2);
    setup1_handle = xTaskCreateStaticAffinitySet(setup1, "setup1", 4096, EL1, 4, stacks[3], &tasks[3], 2);
    el0_handle = xTaskCreateStaticAffinitySet(el0_task, "el0", 4096, NULL, 3, stacks[4], &tasks[4], 1);
    remote0_handle = xTaskCreateStaticAffinitySet(remote0, "remote0", 4096, NULL, 3, stacks[5], &tasks[5], 2);
    CHECK(driver_handle && waker_handle && remote_handle && setup1_handle && el0_handle && remote0_handle);
    vTaskSuspend(el0_handle);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
