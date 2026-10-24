/* Fault capture on the boot core of the returning platform.
 *
 * One task loads from 0x4_0000_0000. Every returning image maps level-1
 * entries 0 and 1 only (platform/a733/platform.c), so the load takes a
 * level-1 translation fault before any bus access. The synchronous vector
 * reaches platform_exception(), which writes one FAIL exception line and
 * exits with monitor status 1, the bundle's one controlled outcome
 * (docs/designs/board-bundles-next.md). A load that returns exits with
 * status 3, which no outcome lists. The image replaces checks/kernel.c in the
 * a733-kernel-check link; every other object is unchanged. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"

#define FAULT_ADDRESS UINT64_C(0x400000000)

enum { STACK_WORDS = 1024 };
static StaticTask_t task;
static StackType_t stack[STACK_WORDS];

/* The controlled outcome requires ELR_EL1 inside this function. */
__attribute__((noinline, used)) uint64_t board_bundles_fault_load(void)
{
    return *(volatile uint64_t *)FAULT_ADDRESS;
}

static void fault_task(void *argument)
{
    (void)argument;
    platform_puts("CHECK fault capture: load 0x400000000\n");
    (void)board_bundles_fault_load();
    platform_puts("FAIL fault capture: the load returned\n");
    platform_exit(3);
}

void vApplicationStackOverflowHook(TaskHandle_t handle, char *name)
{
    (void)handle;
    platform_panic(name, 0);
}

void kernel_main(void)
{
    platform_init();
    platform_puts("sunstoneos fault capture: AArch64 EL1, secure monitor\n");
    configASSERT(xTaskCreateStatic(fault_task, "fault", STACK_WORDS, NULL, 2, stack, &task) != NULL);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
