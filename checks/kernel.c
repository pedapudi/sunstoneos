#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "platform.h"

enum { ROUNDS = 100, STACK_WORDS = 1024 };
static StaticTask_t tasks[3];
static StackType_t stacks[3][STACK_WORDS];
static StaticQueue_t queue_state;
static uint8_t queue_storage[4 * sizeof(uint64_t)];
static QueueHandle_t queue;
static volatile uint64_t busy_work;
extern int check_fp_context(uint64_t value);

static void producer(void *argument)
{
    (void)argument;
    for (uint64_t i = 1; i <= ROUNDS; ++i) {
        configASSERT(check_fp_context(0x1234000000000000UL + i));
        configASSERT(xQueueSend(queue, &i, portMAX_DELAY) == pdPASS);
    }
    vTaskSuspend(NULL);
    configASSERT(0);
}

static void consumer(void *argument)
{
    (void)argument;
    TickType_t start = xTaskGetTickCount();
    for (uint64_t expected = 1; expected <= ROUNDS; ++expected) {
        uint64_t received = 0;
        configASSERT(check_fp_context(0xabcd000000000000UL + expected));
        configASSERT(xQueueReceive(queue, &received, pdMS_TO_TICKS(20)) == pdPASS);
        configASSERT(received == expected);
    }
    configASSERT(xTaskGetTickCount() - start >= ROUNDS);
    configASSERT(busy_work != 0);
    platform_puts("PASS kernel: timer preemption, queue ordering, blocking wakeup, FP context; rounds=100 ticks=");
    platform_hex(xTaskGetTickCount() - start);
    platform_puts("\n");
    platform_exit(0);
}

static void busy(void *argument)
{
    (void)argument;
    // This task never yields. Timer preemption must release the other tasks.
    for (;;) ++busy_work;
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    platform_panic(name, 0);
}

void kernel_main(void)
{
    platform_init();
    platform_puts("sunstoneos FreeRTOS kernel check: AArch64 EL1, QEMU virt\n");
    queue = xQueueCreateStatic(4, sizeof(uint64_t), queue_storage, &queue_state);
    configASSERT(queue != NULL);
    configASSERT(xTaskCreateStatic(producer, "producer", STACK_WORDS, NULL, 3,
                                  stacks[0], &tasks[0]) != NULL);
    configASSERT(xTaskCreateStatic(consumer, "consumer", STACK_WORDS, NULL, 4,
                                  stacks[1], &tasks[1]) != NULL);
    configASSERT(xTaskCreateStatic(busy, "busy", STACK_WORDS, NULL, 1,
                                  stacks[2], &tasks[2]) != NULL);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
