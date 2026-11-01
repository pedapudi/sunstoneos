/* Real EL0 tasks exercise the fixed gateway and the production SMP port. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "pvr_gpu_call.h"
#include <stdint.h>

static StaticTask_t states[2];
static StackType_t stacks[2][4096];
static TaskHandle_t client;
static volatile unsigned entered, migrated, release_call, irq_count, irq_mode;
extern int gpu_call_abi(unsigned, void *);
extern void gpu_call_base_irq(uint64_t);

int a733_memory_local_prepare(void) { return 1; }
static unsigned caller_mode(void)
{
    register uint64_t result __asm__("x0");
    __asm__ volatile("svc #122" : "=r"(result) :: "memory");
    return (unsigned)result;
}
static int raw_enter(void)
{
    register int result __asm__("x0") = 0;
    __asm__ volatile("svc #120" : "+r"(result) :: "memory");
    return result;
}
static int raw_exit(void)
{
    register int result __asm__("x0") = 0;
    __asm__ volatile("svc #121" : "+r"(result) :: "memory");
    return result;
}
static __attribute__((unused)) uint64_t counter(void)
{ uint64_t n; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(n)); return n; }
static __attribute__((unused)) uint64_t frequency(void)
{ uint64_t n; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(n)); return n; }
static uint64_t privilege(void)
{ uint64_t n; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(n)); return n >> 56 & 1; }

#if !defined(GPU_CALL_NO_DISPATCH) && !defined(GPU_CALL_NO_GATEWAY)
int pvr_gpu_call_dispatch(unsigned operation, void *argument)
{
    uint64_t level, stack_select;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, SPSel" : "=r"(level), "=r"(stack_select));
    configASSERT(level == 4 && stack_select == 0 && privilege() == 1);
    configASSERT(xTaskGetCurrentTaskHandle() == client && argument == &entered);
    uintptr_t local = (uintptr_t)&level;
    configASSERT(local >= (uintptr_t)stacks[0] && local < (uintptr_t)(stacks[0] + 4096));
    configASSERT(raw_enter() == -1 && raw_exit() == -1 && caller_mode() == 4);
    if (operation == 2) return 0x1234;
    configASSERT(operation == 1);
    __atomic_store_n(&entered, 1, __ATOMIC_RELEASE);
    uint64_t end = counter() + frequency();
    while (portGET_CORE_ID() != 1) configASSERT(counter() < end);
    __atomic_store_n(&migrated, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&release_call, __ATOMIC_ACQUIRE)) configASSERT(counter() < end);
    configASSERT(privilege() == 1 && caller_mode() == 4);
    /* SGI1 uses an API-safe priority and interrupts the EL1t continuation. */
    uintptr_t local_gic = 0x080a0000 + 0x20000 + 0x10000;
    *(volatile uint8_t *)(local_gic + 0x401) = portLOWEST_USABLE_INTERRUPT_PRIORITY << portPRIORITY_SHIFT;
    *(volatile uint32_t *)(local_gic + 0x100) = 2;
    uint64_t target = (1u << 24) | 2u;
    __asm__ volatile("dsb sy\n msr ICC_SGI1R_EL1, %0\n isb" :: "r"(target) : "memory");
    while (!__atomic_load_n(&irq_count, __ATOMIC_ACQUIRE)) configASSERT(counter() < end);
    configASSERT(irq_mode == 4);
    return 0x1234;
}
#endif

void vApplicationIRQHandler(uint64_t interrupt)
{
    if (interrupt != 1) { gpu_call_base_irq(interrupt); return; }
    uint64_t origin;
    __asm__ volatile("mrs %0, SPSR_EL1" : "=r"(origin));
    irq_mode = origin & 15;
    __atomic_add_fetch(&irq_count, 1, __ATOMIC_RELEASE);
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveIndexedFromISR(client, 1, &woken);
    portYIELD_FROM_ISR(woken);
}
static void worker(void *unused)
{
    (void)unused;
    configASSERT(caller_mode() == 0 && privilege() == 0);
    configASSERT(raw_enter() == -1 && raw_exit() == -1 && caller_mode() == 0);
#if defined(GPU_CALL_NO_GATEWAY)
    platform_puts("PASS GPU call: absent gateway refuses entry\n");
#elif defined(GPU_CALL_NO_DISPATCH)
    configASSERT(pvr_gpu_call(1, (void *)&entered) == -1);
    configASSERT(caller_mode() == 0 && privilege() == 0);
    platform_puts("PASS GPU call: absent dispatcher returns to EL0\n");
#else
    configASSERT(gpu_call_abi(1, (void *)&entered) == 0x1234);
    configASSERT(caller_mode() == 0 && privilege() == 0 && portGET_CORE_ID() == 1);
    configASSERT(ulTaskNotifyTakeIndexed(1, pdTRUE, 0) == 1);
    configASSERT(gpu_call_abi(2, (void *)&entered) == 0x1234);
    configASSERT(caller_mode() == 0 && privilege() == 0);
    platform_puts("PASS GPU call: EL0 entry, guarded sites, EL1t migration, IRQ and ABI preserved\n");
#endif
    platform_exit(0);
}
static __attribute__((unused)) void controller(void *unused)
{
    (void)unused;
    TickType_t end = xTaskGetTickCount() + 1000;
    while (!__atomic_load_n(&entered, __ATOMIC_ACQUIRE)) {
        configASSERT(xTaskGetTickCount() < end); vTaskDelay(1);
    }
    /* A higher-priority task preempts the continuation before migrating it. */
    vTaskCoreAffinitySet(client, 2);
    while (!__atomic_load_n(&migrated, __ATOMIC_ACQUIRE)) {
        configASSERT(xTaskGetTickCount() < end); vTaskDelay(1);
    }
    __atomic_store_n(&release_call, 1, __ATOMIC_RELEASE);
    vTaskSuspend(NULL);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }
void kernel_main(void)
{
    platform_init();
    client = xTaskCreateStaticAffinitySet(worker, "gpu-call", 4096, NULL, 4, stacks[0], &states[0], 1);
    configASSERT(client);
#if !defined(GPU_CALL_NO_DISPATCH) && !defined(GPU_CALL_NO_GATEWAY)
    configASSERT(xTaskCreateStaticAffinitySet(controller, "control", 4096, NULL, 6, stacks[1], &states[1], 1));
#endif
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
