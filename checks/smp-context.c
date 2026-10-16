#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include <stddef.h>

// Offsets are shared with smp-context.S. General registers include SP at word 31.
typedef struct {
    uint64_t general[32];
    uint64_t vector[64];
    uint64_t nzcv, fpcr, fpsr, reserved;
} Context;
typedef struct {
    uint32_t ready, release, observed_core, complete;
} Gate;
_Static_assert(sizeof(Context) == 800, "assembly snapshot size");
_Static_assert(offsetof(Context, vector) == 256, "assembly vector offset");
_Static_assert(offsetof(Context, nzcv) == 768, "assembly status offset");
_Static_assert(offsetof(Gate, observed_core) == 8, "assembly core offset");
extern void check_migrating_context(Context *expected, Context *actual, Gate *gate);

static StaticTask_t states[3];
static StackType_t stacks[3][4096];
static TaskHandle_t workers[2];
static Gate gates[2];
static unsigned round_number, completed_migrations;

static void worker(void *argument) {
    const unsigned id = (uintptr_t)argument;
    for (;;) {
        configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
        const unsigned round = round_number;
        const unsigned source = id ^ (round & 1U), target = source ^ 1U;
        configASSERT(portGET_CORE_ID() == source);
        Context expected = {0}, actual = {0};
        for (unsigned i = 0; i < 31; ++i)
            expected.general[i] = 0xd1e2f30000000000UL | ((uint64_t)round << 24) | (id << 16) | (i * 257U);
        for (unsigned i = 0; i < 64; ++i)
            expected.vector[i] = 0xa1b2c30000000000UL | ((uint64_t)round << 24) | (id << 16) | (i * 259U);
        expected.nzcv = (uint64_t)((round + id) & 15U) << 28;
        expected.fpcr = (uint64_t)((round + id) & 3U) << 22;
        expected.fpsr = (round + id) & 31U;
        check_migrating_context(&expected, &actual, &gates[id]);
        configASSERT(portGET_CORE_ID() == target);
        const uint64_t *want = (const uint64_t *)&expected, *got = (const uint64_t *)&actual;
        for (unsigned i = 0; i < sizeof(Context) / sizeof(uint64_t); ++i) {
            if (want[i] != got[i]) {
                platform_puts("FAIL migrated context word="); platform_hex(i);
                platform_puts(" expected="); platform_hex(want[i]);
                platform_puts(" actual="); platform_hex(got[i]); platform_puts("\n");
                platform_exit(1);
            }
        }
        __atomic_store_n(&gates[id].complete, 1, __ATOMIC_RELEASE);
    }
}

static void wait_value(uint32_t *address, uint32_t value, TickType_t deadline) {
    while (__atomic_load_n(address, __ATOMIC_ACQUIRE) != value) {
        configASSERT(xTaskGetTickCount() < deadline);
        vTaskDelay(1);
    }
}
static void controller(void *unused) {
    (void)unused;
    for (unsigned round = 0; round < 32; ++round) {
        round_number = round;
        const TickType_t deadline = xTaskGetTickCount() + 1000;
        for (unsigned id = 0; id < 2; ++id) {
            gates[id] = (Gate){ .observed_core = UINT32_MAX };
            configASSERT(xTaskNotifyGive(workers[id]) == pdPASS);
        }
        for (unsigned id = 0; id < 2; ++id) wait_value(&gates[id].ready, 1, deadline);
        for (unsigned id = 0; id < 2; ++id)
            vTaskCoreAffinitySet(workers[id], 1U << (id ^ (round & 1U) ^ 1U));
        // The assembly loop reports its core while the full register patterns
        // are still live. Do not release it until migration is observed.
        for (unsigned id = 0; id < 2; ++id)
            wait_value(&gates[id].observed_core, id ^ (round & 1U) ^ 1U, deadline);
        for (unsigned id = 0; id < 2; ++id)
            __atomic_store_n(&gates[id].release, 1, __ATOMIC_RELEASE);
        for (unsigned id = 0; id < 2; ++id) {
            wait_value(&gates[id].complete, 1, deadline);
            ++completed_migrations;
        }
    }
    configASSERT(completed_migrations == 64);
    platform_puts("PASS SMP context: 64 task migrations preserve x0-x30, SP, q0-q31, NZCV, FPCR and FPSR\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task; platform_panic(name, 0);
}
void kernel_main(void) {
    platform_init();
    for (unsigned id = 0; id < 2; ++id) {
        workers[id] = xTaskCreateStaticAffinitySet(worker, "migrate", 4096, (void *)(uintptr_t)id,
                                                  4, stacks[id], &states[id], 1U << id);
        configASSERT(workers[id]);
    }
    configASSERT(xTaskCreateStaticAffinitySet(controller, "control", 4096, 0, 6,
                                             stacks[2], &states[2], 1));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
