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
StaticTask_t owner_state, services[2], observer_state;
StackType_t owner_stack[8192], service_stacks[2][1024], observer_stack[1024];
TaskHandle_t service_handles[2], observer_handle;
std::atomic<unsigned> entered, serviced, progress, failures;
std::atomic<bool> live{true}, throttle{true}, close_after_callback, returned;
struct Context { uint64_t magic; };
Context *borrowed;
constexpr uint64_t kMagic = 0x6f73382d6f776e65;
void service(void *argument) {
    unsigned core = unsigned(reinterpret_cast<uintptr_t>(argument));
    configASSERT(portGET_CORE_ID() == core && uxTaskPriorityGet(nullptr) == 3);
    configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);
    configASSERT(entered.load(std::memory_order_acquire) == 0xff);
    configASSERT(!returned.load(std::memory_order_acquire));
    serviced.fetch_or(1u << core, std::memory_order_release);
    vTaskSuspend(nullptr);
    platform_panic("service resumed", 0);
}
void observe(void *) {
    configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) >= 1);
    // Let the real failed() path suspend and let every acknowledged slot drain.
    vTaskDelay(3);
    configASSERT(failures.load() && !live.load() && !returned.load());
    configASSERT(borrowed && borrowed->magic == kMagic);
    platform_puts("PASS OS8 workers: owner6 workers7/0-5; services preempt A55 inference; pre-ack failure retains context\n");
    platform_exit(0);
}
void recognize(void *) {
    configASSERT(portGET_CORE_ID() == 6 && uxTaskPriorityGet(nullptr) == kMimusOwnerPriority);
    mimus_parallel_bind();
    const uint64_t until = counter() + COUNTER_HZ;
    mimus::asr::ParallelFor(8, [](void *opaque, size_t a, size_t b, int worker) {
        const auto until = *static_cast<uint64_t *>(opaque);
        const unsigned core = worker < 2 ? unsigned(worker + 6) : unsigned(worker - 2);
        configASSERT(a == unsigned(worker) && b == a + 1 && portGET_CORE_ID() == core);
        configASSERT(uxTaskPriorityGet(nullptr) == (core < 6 ? 2 : 3));
        entered.fetch_or(1u << worker, std::memory_order_release);
        if (!worker) {
            while (entered.load(std::memory_order_acquire) != 0xff) configASSERT(counter() < until);
            xTaskNotifyGive(service_handles[0]);
            xTaskNotifyGive(service_handles[1]);
        }
        while (serviced.load(std::memory_order_acquire) != 3) configASSERT(counter() < until);
    }, const_cast<uint64_t *>(&until));
    configASSERT(progress.load() >= 17 && live.load());
    throttle.store(false);
    mimus_parallel_report();
    Context context{kMagic};
    borrowed = &context;
    mimus::asr::ParallelFor(8, [](void *, size_t, size_t, int worker) {
        // Core 0's actual post-callback OS checkpoint closes admission. It
        // suspends through failed() before the adapter can send completion.
        if (worker == 2) close_after_callback.store(true, std::memory_order_release);
    }, &context);
    returned.store(true);
    platform_panic("owner returned without every acknowledgement", 0);
}
}
extern "C" int mimus_board_live(void) {
    if (portGET_CORE_ID() == 0 && close_after_callback.load(std::memory_order_acquire)) live.store(false);
    return live.load(std::memory_order_acquire);
}
extern "C" void mimus_board_fail(void) {
    live.store(false, std::memory_order_release);
    failures.fetch_add(1);
    xTaskNotifyGive(observer_handle);
}
extern "C" void mimus_board_progress(void) { progress.fetch_add(1); }
extern "C" int os_thermal_throttled(void) { return throttle.load(); }
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *name) { platform_panic(name, 0); }
extern "C" void kernel_main(void) {
    platform_init(); runtime_smp_init(); runtime_init(); mimus_parallel_init(6, 7);
    for (unsigned i = 0; i < 2; ++i) {
        service_handles[i] = xTaskCreateStaticAffinitySet(service, "service", 1024,
            reinterpret_cast<void *>(uintptr_t(i)), 3, service_stacks[i], &services[i], 1u << i);
        configASSERT(service_handles[i]);
    }
    observer_handle = xTaskCreateStaticAffinitySet(observe, "retention", 1024, nullptr, 4,
        observer_stack, &observer_state, 1);
    configASSERT(observer_handle);
    configASSERT(xTaskCreateStaticAffinitySet(recognize, "recognizer", 8192, nullptr,
        kMimusOwnerPriority, owner_stack, &owner_state, 1u << 6));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
