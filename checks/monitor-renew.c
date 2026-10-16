#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
static StaticTask_t state;
static StackType_t stack[1024];
static void exercise(void *argument)
{
    (void)argument;
    uint64_t start = counter();
    while (counter() - start < 3 * COUNTER_HZ) {
        register uint64_t operation __asm__("x0") = 0x734;
        __asm__ volatile("smc #0" : "+r"(operation) :: "memory");
        configASSERT(operation == 0);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    platform_puts("PASS monitor renewal: task survives initial deadline; stop renewal to exercise recovery\n");
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    for (;;) __asm__ volatile("wfi");
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    platform_panic(name, 0);
}
void kernel_main(void)
{
    platform_init();
    configASSERT(xTaskCreateStatic(exercise, "renew", 1024, NULL, 3, stack, &state));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
