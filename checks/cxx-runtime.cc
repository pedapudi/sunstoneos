#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <new>
#include <string>
#include <vector>

extern "C" void runtime_init(void);
static StaticTask_t tasks[2];
static StackType_t stacks[2][4096];
static unsigned finished;
static std::string constructed(80, 's');
struct alignas(64) Aligned { unsigned char bytes[128]; };

static void exercise(void *argument)
{
    uintptr_t id = reinterpret_cast<uintptr_t>(argument);
    configASSERT(constructed == std::string(80, 's'));
    for (unsigned round = 0; round < 100; ++round) {
        std::vector<unsigned> data(257 + round, unsigned(id + round));
        auto *aligned = new (std::nothrow) Aligned;
        configASSERT(aligned && (reinterpret_cast<uintptr_t>(aligned) & 63) == 0);
        memset(aligned->bytes, int(id), sizeof(aligned->bytes));
        void *block = std::calloc(17, 13);
        configASSERT(block);
        for (size_t i = 0; i < 221; ++i) configASSERT(static_cast<unsigned char *>(block)[i] == 0);
        memset(block, int(round), 221);
        block = std::realloc(block, 1024);
        configASSERT(block);
        errno = int(100 + id);
        vTaskDelay(1);
        configASSERT(errno == int(100 + id));
        configASSERT(std::all_of(data.begin(), data.end(), [=](unsigned value) { return value == id + round; }));
        for (size_t i = 0; i < 221; ++i) configASSERT(static_cast<unsigned char *>(block)[i] == round);
        for (auto value : aligned->bytes) configASSERT(value == id);
        std::free(block);
        delete aligned;
    }
    std::expected<unsigned, int> value = 42;
    configASSERT(value && *value == 42);
    value = std::unexpected(7);
    configASSERT(!value && value.error() == 7);
    volatile float four = 4.0f;
    configASSERT(std::sqrt(four) == 2.0f);
    char text[32];
    configASSERT(std::snprintf(text, sizeof(text), "task %u", unsigned(id)) == 6);
    configASSERT(text[5] == char('0' + id));
    void *too_large = std::malloc(8 * 1024 * 1024);
    configASSERT(!too_large && errno == ENOMEM);
    auto *oversized = new (std::nothrow) Aligned[65536];
    configASSERT(!oversized);
    delete[] oversized;
    auto *after_failure = new (std::nothrow) Aligned;
    configASSERT(after_failure);
    delete after_failure;
    if (++finished == 2) {
        platform_puts("PASS C++ runtime: constructors, containers, aligned allocation, realloc, task errno, bounded heap; rounds=100 per task\n");
        platform_exit(0);
    }
    vTaskSuspend(nullptr);
    platform_panic("suspended task returned", 0);
}

extern "C" void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    platform_panic(name, 0);
}
extern "C" void kernel_main(void)
{
    platform_init();
    runtime_init();
    for (uintptr_t i = 0; i < 2; ++i)
        configASSERT(xTaskCreateStatic(exercise, "cxx", 4096, reinterpret_cast<void *>(i + 1),
                                      3, stacks[i], &tasks[i]));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
