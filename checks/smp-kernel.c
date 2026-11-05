#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "platform.h"

static StaticTask_t tasks[4];
static StackType_t stacks[4][2048];
static StaticQueue_t queue_state, mutex_state;
static uint8_t queue_storage[4 * 256];
static QueueHandle_t queue;
static SemaphoreHandle_t mutex;
static uint64_t progress[2], protected_count, protected_words[32];
extern uint64_t check_fp_context(uint64_t pattern);
// The port's two ticket locks (port.c): the low halfword of each word is the owner, the high halfword the next ticket.
extern struct { uint32_t word; volatile uint64_t recursion; } __attribute__((aligned(64))) xPortLocks[2];
static uint32_t contention_phase;

static unsigned waiting_tickets(void) {
    unsigned most = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t word = __atomic_load_n(&xPortLocks[i].word, __ATOMIC_ACQUIRE);
        const unsigned depth = (uint16_t)((word >> 16) - (word & 0xffff));
        if (depth > most) most = depth;
    }
    return most;
}

static void background(void *argument) {
    const unsigned id = (uintptr_t)argument;
    const uint64_t fp_pattern = 0xa5a5000000000000UL + id;
    __asm__ volatile("fmov d8, %0" :: "r"(fp_pattern) : "v8");
    for (;;) {
        uint64_t fp_value;
        __asm__ volatile("fmov %0, d8" : "=r"(fp_value));
        configASSERT(fp_value == fp_pattern);
        configASSERT(portGET_CORE_ID() == id);
        configASSERT(xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE);
        for (unsigned i = 0; i < 32; ++i) configASSERT(protected_words[i] == protected_count);
        ++protected_count;
        for (unsigned i = 0; i < 32; ++i) protected_words[i] = protected_count;
        configASSERT(xSemaphoreGive(mutex) == pdTRUE);
        __atomic_add_fetch(&progress[id], 1, __ATOMIC_RELAXED);
        // No voluntary delay: tick and interprocessor interrupts must preempt.
    }
}
static void producer(void *unused) {
    (void)unused;
    // Hold both locks until core 1 has taken a ticket behind this core. Core 1 then finds the lock held and runs the
    // monitored wait (SEVL, WFE, LDAXRH) whatever the relative speed of the two cores.
    taskENTER_CRITICAL();
    __atomic_store_n(&contention_phase, 1, __ATOMIC_RELEASE);
    while (waiting_tickets() < 2) {}
    taskEXIT_CRITICAL();
    for (uint64_t sequence = 1; sequence <= 256; ++sequence) {
        uint64_t payload[32];
        configASSERT(portGET_CORE_ID() == 0);
        for (unsigned i = 0; i < 32; ++i) payload[i] = sequence * 1000 + i;
        configASSERT(xQueueSend(queue, payload, portMAX_DELAY) == pdTRUE);
        configASSERT(check_fp_context(0x1122334455667788));
    }
    vTaskSuspend(0);
    platform_panic("producer resumed", 0);
}
static void consumer(void *unused) {
    (void)unused;
    while (!__atomic_load_n(&contention_phase, __ATOMIC_ACQUIRE)) {}
    taskENTER_CRITICAL();
    taskEXIT_CRITICAL();
    for (uint64_t sequence = 1; sequence <= 256; ++sequence) {
        uint64_t payload[32];
        configASSERT(portGET_CORE_ID() == 1);
        configASSERT(xQueueReceive(queue, payload, portMAX_DELAY) == pdTRUE);
        for (unsigned i = 0; i < 32; ++i) configASSERT(payload[i] == sequence * 1000 + i);
        configASSERT(check_fp_context(0x8877665544332211));
    }
    configASSERT(xTaskGetTickCount() >= 256);
    configASSERT(__atomic_load_n(&progress[0], __ATOMIC_RELAXED));
    configASSERT(__atomic_load_n(&progress[1], __ATOMIC_RELAXED));
    configASSERT(xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE);
    for (unsigned i = 0; i < 32; ++i) configASSERT(protected_words[i] == protected_count);
    platform_puts("PASS SMP: 256 cross-core queue exchanges, core affinity, FP context, mutex exclusion and preemption on both cores\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task; platform_panic(name, 0);
}
void kernel_main(void) {
    platform_init();
    queue = xQueueCreateStatic(4, 256, queue_storage, &queue_state);
    mutex = xSemaphoreCreateMutexStatic(&mutex_state);
    configASSERT(queue && mutex);
    configASSERT(xTaskCreateStaticAffinitySet(producer, "producer", 2048, 0, 4, stacks[0], &tasks[0], 1));
    configASSERT(xTaskCreateStaticAffinitySet(consumer, "consumer", 2048, 0, 5, stacks[1], &tasks[1], 2));
    configASSERT(xTaskCreateStaticAffinitySet(background, "busy0", 2048, 0, 1, stacks[2], &tasks[2], 1));
    configASSERT(xTaskCreateStaticAffinitySet(background, "busy1", 2048, (void *)1, 1, stacks[3], &tasks[3], 2));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
