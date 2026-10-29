#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "timing.h"
#include "runtime.h"
#include "parallel_runtime.h"
#include "mimus/asr/parallel.h"
#include <atomic>
extern "C" void runtime_smp_init(void);
namespace {
StaticTask_t owner_state, blocker_state;
StackType_t owner_stack[8192], blocker_stack[1024];
std::atomic<unsigned> completed, blocked, progress;
uint64_t stop_deadline;
void blocker(void *) {
    blocked.store(1, std::memory_order_release);
    for (;;) __asm__ volatile("yield");
}
void recognize(void *) {
    configASSERT(portGET_CORE_ID() == 6);
    mimus_parallel_bind();
    mimus::asr::ParallelFor(8, [](void *, size_t begin, size_t end, int index) {
        configASSERT(begin == unsigned(index) && end == begin + 1);
        completed.fetch_or(1u << index, std::memory_order_release);
    }, nullptr);
    configASSERT(completed.load(std::memory_order_acquire) == 255 && progress.load() >= 16);
    mimus_parallel_report();
#ifdef OS_RETURN_BLOCK_WORKER
    configASSERT(xTaskCreateStaticAffinitySet(blocker, "block-worker", 1024, nullptr, 4,
        blocker_stack, &blocker_state, 1u << 2));
    while (!blocked.load(std::memory_order_acquire)) taskYIELD();
#endif
    stop_deadline = counter() + COUNTER_HZ / 4;
    mimus_parallel_stop();
#ifdef OS_RETURN_BLOCK_WORKER
    platform_panic("stop returned without worker acknowledgment", 0);
#else
    configASSERT(counter() < stop_deadline);
    platform_puts("PASS recorded CPU return: seven workers acknowledged stop after real parallel completion\n");
    platform_exit(0);
#endif
}
}
extern "C" uint64_t a733_smp_counter(void) { return counter(); }
extern "C" uint64_t os_cpu_return_cutoff(void) { return stop_deadline; }
extern "C" void mimus_native_checkpoint(void) { progress.fetch_add(1); }
extern "C" void mimus_native_inference_progress(void) { progress.fetch_add(1); }
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *name) { platform_panic(name, 0); }
extern "C" void kernel_main(void) {
    platform_init(); runtime_smp_init(); runtime_init(); mimus_parallel_init(6, 7);
    configASSERT(xTaskCreateStaticAffinitySet(recognize, "recognizer", 8192, nullptr,
        kMimusOwnerPriority, owner_stack, &owner_state, 1u << 6));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
