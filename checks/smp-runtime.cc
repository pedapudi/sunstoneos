#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "runtime.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include <reent.h>

extern "C" void runtime_smp_init(void);
static StaticTask_t tasks[2];
static StackType_t stacks[2][4096];
static unsigned finished;
static unsigned progress[2];
static unsigned arrivals[2];
static struct _reent *task_state[2];
static std::string constructed(80, 's');
static char shared_output[2048];
static FILE *shared_stream;
struct alignas(64) Aligned { unsigned char bytes[128]; };

static void exercise(void *argument) {
    const unsigned id = reinterpret_cast<uintptr_t>(argument);
    task_state[id] = __getreent();
    configASSERT(constructed == std::string(80, 's'));
    for (unsigned round = 0; round < 128; ++round) {
        configASSERT(portGET_CORE_ID() == (id ^ (round & 1U)));
        __atomic_store_n(&arrivals[id], round + 1, __ATOMIC_RELEASE);
        while (__atomic_load_n(&arrivals[id ^ 1U], __ATOMIC_ACQUIRE) < round + 1) vTaskDelay(1);
        std::vector<unsigned> data(257 + round, id + round);
        auto *aligned = new (std::nothrow) Aligned;
        configASSERT(aligned && !(reinterpret_cast<uintptr_t>(aligned) & 63));
        std::memset(aligned->bytes, int(id), sizeof(aligned->bytes));
        void *block = std::calloc(17, 13);
        configASSERT(block);
        for (size_t i = 0; i < 221; ++i) configASSERT(static_cast<unsigned char *>(block)[i] == 0);
        std::memset(block, int(round), 221);
        block = std::realloc(block, 1024);
        configASSERT(block);
        char source[32], text[32];
        std::snprintf(source, sizeof(source), "task%u,round%u", id, round);
        char *first = std::strtok(source, ",");
        configASSERT(first && first[4] == char('0' + id));
        errno = int(100 + id);
        // Migrate with live allocations, errno and strtok's continuation state.
        vTaskCoreAffinitySet(nullptr, 1U << (portGET_CORE_ID() ^ 1U));
        vTaskDelay(1);
        configASSERT(portGET_CORE_ID() == (id ^ (round & 1U) ^ 1U));
        configASSERT(__getreent() == task_state[id]);
        configASSERT(errno == int(100 + id));
        std::snprintf(text, sizeof(text), "round%u", round);
        char *second = std::strtok(nullptr, ",");
        configASSERT(second && !std::strcmp(second, text));
        configASSERT(std::all_of(data.begin(), data.end(), [=](unsigned n) { return n == id + round; }));
        for (size_t i = 0; i < 221; ++i) configASSERT(static_cast<unsigned char *>(block)[i] == round);
        for (auto byte : aligned->bytes) configASSERT(byte == id);
        std::free(block);
        delete aligned;
        // Open and close a memory stream to exercise dynamic library locks.
        char buffer[32] = {};
        FILE *stream = fmemopen(buffer, sizeof(buffer), "w+");
        configASSERT(stream);
        configASSERT(std::fprintf(stream, "%u:%u", id, round) > 0);
        configASSERT(std::fclose(stream) == 0);
        std::snprintf(text, sizeof(text), "%u:%u", id, round);
        configASSERT(!std::strcmp(buffer, text));
        configASSERT(std::fprintf(shared_stream, "%u:%03u\n", id, round) == 6);
        __atomic_add_fetch(&progress[id], 1, __ATOMIC_RELEASE);
    }
    void *large = std::malloc(8 * 1024 * 1024);
    configASSERT(!large && errno == ENOMEM);
    void *small = std::malloc(64);
    configASSERT(small);
    std::free(small);
    if (__atomic_add_fetch(&finished, 1, __ATOMIC_ACQ_REL) == 2) {
        configASSERT(task_state[0] != task_state[1]);
        configASSERT(__atomic_load_n(&progress[0], __ATOMIC_ACQUIRE) == 128);
        configASSERT(__atomic_load_n(&progress[1], __ATOMIC_ACQUIRE) == 128);
        configASSERT(std::fclose(shared_stream) == 0);
        unsigned next[2] = {};
        for (unsigned offset = 0; offset < 1536; offset += 6) {
            const char *record = shared_output + offset;
            configASSERT((record[0] == '0' || record[0] == '1') && record[1] == ':' && record[5] == '\n');
            const unsigned writer = record[0] - '0';
            char expected[7];
            std::snprintf(expected, sizeof(expected), "%u:%03u\n", writer, next[writer]++);
            configASSERT(!std::memcmp(record, expected, 6));
        }
        configASSERT(next[0] == 128 && next[1] == 128 && shared_output[1536] == 0);
        platform_puts("PASS SMP runtime: concurrent allocation, task errno and strtok, shared memory stream, constructors and 256 migrations; rounds=128 per task\n");
        platform_exit(0);
    }
    vTaskSuspend(nullptr);
    platform_panic("finished task resumed", 0);
}
extern "C" void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task; platform_panic(name, 0);
}
extern "C" void kernel_main(void) {
    platform_init();
    runtime_smp_init();
    runtime_init();
    shared_stream = fmemopen(shared_output, sizeof(shared_output), "w+");
    configASSERT(shared_stream);
    for (uintptr_t id = 0; id < 2; ++id)
        configASSERT(xTaskCreateStaticAffinitySet(exercise, "runtime", 4096, reinterpret_cast<void *>(id),
                                                 3, stacks[id], &tasks[id], 1U << id));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
