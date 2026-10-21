#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "platform.h"

// Eight-core emulator soak: a token ring whose every hop wakes a task on the
// next core, mutex-protected shared words written from every core, migrating
// tasks that keep FP state while their affinity rotates across all cores, and
// non-yielding background tasks so the tick and SGIs must preempt each core.

#define CORES configNUMBER_OF_CORES
#define LAPS 64U
#define MIGRATIONS 32U
_Static_assert(CORES == 8, "smp8-soak requires the eight-core configuration");

static StaticTask_t ring_tasks[CORES], busy_tasks[CORES], mover_tasks[2], control_task;
static StackType_t ring_stacks[CORES][2048], busy_stacks[CORES][1024];
static StackType_t mover_stacks[2][2048], control_stack[2048];
static StaticQueue_t ring_state[CORES], mutex_state, done_state;
static uint8_t ring_storage[CORES][2 * sizeof(uint64_t)], done_storage[4 * sizeof(uint32_t)];
static QueueHandle_t ring[CORES], done;
static SemaphoreHandle_t mutex;
static TaskHandle_t movers[2], controller;
static uint32_t targets[2];
static uint64_t protected_count, protected_words[16];
static uint64_t busy_progress[CORES] __attribute__((aligned(64)));
static uint32_t visited[2];
extern uint64_t check_fp_context(uint64_t pattern);

static void protected_update(void) {
    configASSERT(xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE);
    for (unsigned i = 0; i < 16; ++i) configASSERT(protected_words[i] == protected_count);
    ++protected_count;
    for (unsigned i = 0; i < 16; ++i) protected_words[i] = protected_count;
    configASSERT(xSemaphoreGive(mutex) == pdTRUE);
}
// Task i receives from ring[i] and forwards to ring[i + 1]; core 0 injects
// the token and checks that each lap visited every core in order.
static void ring_task(void *argument) {
    const unsigned id = (uintptr_t)argument;
    if (id == 0) {
        uint64_t token = 0;
        configASSERT(xQueueSend(ring[1], &token, portMAX_DELAY) == pdTRUE);
    }
    for (unsigned lap = 0; lap < LAPS; ++lap) {
        uint64_t token;
        configASSERT(xQueueReceive(ring[id], &token, portMAX_DELAY) == pdTRUE);
        configASSERT(portGET_CORE_ID() == id);
        // The token counts hops; hop h arrives at core h % CORES.
        configASSERT(token % CORES == (id + CORES - 1) % CORES);
        ++token;
        protected_update();
        if (id == 0 && lap + 1 == LAPS) break;
        configASSERT(xQueueSend(ring[(id + 1) % CORES], &token, portMAX_DELAY) == pdTRUE);
    }
    const uint32_t finished = id;
    configASSERT(xQueueSend(done, &finished, portMAX_DELAY) == pdTRUE);
    vTaskSuspend(0);
}
static void busy_task(void *argument) {
    const unsigned id = (uintptr_t)argument;
    const uint64_t pattern = 0x5a5a000000000000UL + id;
    __asm__ volatile("fmov d9, %0" :: "r"(pattern) : "v9");
    for (;;) {
        uint64_t value;
        __asm__ volatile("fmov %0, d9" : "=r"(value));
        configASSERT(value == pattern);
        configASSERT(portGET_CORE_ID() == id);
        __atomic_add_fetch(&busy_progress[id], 1, __ATOMIC_RELAXED);
    }
}
// The control task moves each mover to a new core and waits until the mover
// reports running there; the mover holds a callee-saved FP value across a
// blocking delay on every core it visits.
static void mover_task(void *argument) {
    const unsigned id = (uintptr_t)argument;
    uint32_t acknowledged = UINT32_MAX;
    for (;;) {
        const unsigned core = portGET_CORE_ID();
        __atomic_or_fetch(&visited[id], 1U << core, __ATOMIC_RELAXED);
        const uint32_t target = __atomic_load_n(&targets[id], __ATOMIC_ACQUIRE);
        if (core == target % CORES && target != acknowledged) {
            acknowledged = target;
            xTaskNotifyGive(controller);
        }
        configASSERT(check_fp_context(0x0123456789abcdefUL ^ ((uint64_t)id << 60) ^ core));
        protected_update();
    }
}
static void control(void *unused) {
    (void)unused;
    for (unsigned round = 1; round <= MIGRATIONS; ++round) {
        for (unsigned id = 0; id < 2; ++id) {
            // Round r moves mover 0 to core r % 8 and mover 1 to (r + 4) % 8;
            // the round number in the target distinguishes revisits.
            const uint32_t target = round * CORES + (round + 4 * id) % CORES;
            __atomic_store_n(&targets[id], target, __ATOMIC_RELEASE);
            vTaskCoreAffinitySet(movers[id], 1U << (target % CORES));
        }
        // Each mover acknowledges once; both may arrive before the first take.
        for (unsigned acknowledged = 0; acknowledged < 2;) {
            const uint32_t count = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            configASSERT(count >= 1 && acknowledged + count <= 2);
            acknowledged += count;
        }
    }
    for (unsigned finished = 0; finished < CORES; ++finished) {
        uint32_t id;
        configASSERT(xQueueReceive(done, &id, portMAX_DELAY) == pdTRUE);
    }
    // Hold the mutex while stopping the movers so none is suspended inside it.
    configASSERT(xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE);
    for (unsigned id = 0; id < 2; ++id) vTaskSuspend(movers[id]);
    for (unsigned core = 0; core < CORES; ++core)
        configASSERT(__atomic_load_n(&busy_progress[core], __ATOMIC_RELAXED));
    for (unsigned id = 0; id < 2; ++id)
        configASSERT(__atomic_load_n(&visited[id], __ATOMIC_RELAXED) == (1U << CORES) - 1);
    for (unsigned i = 0; i < 16; ++i) configASSERT(protected_words[i] == protected_count);
    configASSERT(protected_count >= LAPS * CORES - 1);
    platform_puts("PASS SMP8 soak: 64-lap eight-core queue ring, mutex exclusion from every core, "
                  "two tasks migrated across all eight cores with FP context, preemption on every core\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task; platform_panic(name, 0);
}
void kernel_main(void) {
    platform_init();
    for (unsigned i = 0; i < CORES; ++i) {
        ring[i] = xQueueCreateStatic(2, sizeof(uint64_t), ring_storage[i], &ring_state[i]);
        configASSERT(ring[i]);
    }
    done = xQueueCreateStatic(4, sizeof(uint32_t), done_storage, &done_state);
    mutex = xSemaphoreCreateMutexStatic(&mutex_state);
    configASSERT(done && mutex);
    for (unsigned i = 0; i < CORES; ++i) {
        configASSERT(xTaskCreateStaticAffinitySet(ring_task, "ring", 2048, (void *)(uintptr_t)i, 4,
                                                  ring_stacks[i], &ring_tasks[i], 1U << i));
        configASSERT(xTaskCreateStaticAffinitySet(busy_task, "busy", 1024, (void *)(uintptr_t)i, 1,
                                                  busy_stacks[i], &busy_tasks[i], 1U << i));
    }
    for (unsigned id = 0; id < 2; ++id) {
        targets[id] = UINT32_MAX;
        movers[id] = xTaskCreateStaticAffinitySet(mover_task, "mover", 2048, (void *)(uintptr_t)id, 3,
                                                  mover_stacks[id], &mover_tasks[id], 1U << (4 * id));
        configASSERT(movers[id]);
    }
    controller = xTaskCreateStaticAffinitySet(control, "control", 2048, 0, 5, control_stack,
                                              &control_task, 1);
    configASSERT(controller);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
