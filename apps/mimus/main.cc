#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "timing.h"
#include "runtime.h"
#include "recorded.h"
#include "mimus/asr/model.h"
#include "mimus/asr/recognizer.h"
#include "mimus/asr/parallel.h"
#include "mimus/asr/platform.h"
#include "mimus/asr/profile.h"
#include "mimus/asr/wav.h"
#include "mimus/asr/scratch.h"
#include "mimus/pipeline/formatter.h"
#include <atomic>
#ifdef SUNSTONE_MIMUS_USB
#include "service.h"
#include "usb_service.h"
#endif
#if defined(SUNSTONE_MIMUS_USB) || defined(SUNSTONE_MIMUS_PACED)
#include "replay_runtime.h"
#endif
#ifdef SUNSTONE_SMP_NEWLIB
extern "C" void runtime_smp_init(void);
#include "parallel_runtime.h"
#endif
#ifdef SUNSTONE_MIMUS_NATIVE
#include "native.h"
#endif
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

static StaticTask_t tasks[2];
static StackType_t recognition_stack[16384], recovery_stack[1024];
static std::atomic<uint64_t> application_deadline{UINT64_MAX};
alignas(64) static uint8_t internal_arena[97280];
#ifndef SUNSTONE_MIMUS_USB
constexpr uintptr_t kAssets = 0x43000000;
constexpr size_t kAssetsLimit = 32 * 1024 * 1024;
struct Assets {
    char magic[8];
    uint64_t sizes[3];
    uint32_t crc[3];
};
#endif
#if defined(SUNSTONE_MIMUS_USB) || defined(SUNSTONE_MIMUS_PACED)
// Paced replay with the engine storage shared with whole-recording recognition.
static bool paced_replay(std::span<const uint8_t> inputs[3], unsigned options)
{
    const auto *recognizer = mimus_engine_open(inputs, options);
    return mimus_engine_finish(options, recognizer && mimus_paced_run(*recognizer, inputs[2]));
}
#endif
static void keep_recovery(void *)
{
    for (;;) {
        if (counter() > application_deadline.load(std::memory_order_relaxed)) platform_panic("recognition deadline", 0);
#ifdef SUNSTONE_MIMUS_USB
        // The recovery task must not keep a stalled USB task alive indefinitely.
        if (counter() - usb_runtime_last_progress() > 5 * COUNTER_HZ)
            platform_panic("USB task stalled", 0);
#endif
#ifndef SUNSTONE_SMP_NEWLIB
        register uint64_t operation __asm__("x0") = 0x734;
        __asm__ volatile("smc #0" : "+r"(operation) :: "memory");
        configASSERT(operation == 0);
#endif
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
static void recognize(void *)
{
#ifdef SUNSTONE_SMP_NEWLIB
    mimus_parallel_bind();
#endif
    std::span<const uint8_t> inputs[3];
#ifdef SUNSTONE_MIMUS_USB
    for (;;) {
        const bool paced = mimus_service_wait(inputs);
        application_deadline.store(counter() + 180 * COUNTER_HZ, std::memory_order_relaxed);
        mimus::asr::SetInternalArena(internal_arena);
        mimus_yield_install();
        // The service invalidates the retained engine before model slots change.
        bool success = paced ? paced_replay(inputs, kMimusWarm)
                             : mimus_transcribe_recorded(inputs, internal_arena, nullptr, kMimusWarm);
        application_deadline.store(UINT64_MAX, std::memory_order_relaxed);
        mimus_service_finish(success);
    }
#else
    using namespace mimus;
    application_deadline.store(counter() + 180 * COUNTER_HZ, std::memory_order_relaxed);
    const auto &header = *reinterpret_cast<const Assets *>(kAssets);
    configASSERT(memcmp(header.magic, "SUNMIM01", 8) == 0);
    size_t offset = 128;
    for (unsigned i = 0; i < 3; ++i) {
        configASSERT(header.sizes[i] && header.sizes[i] <= kAssetsLimit - offset);
        inputs[i] = {reinterpret_cast<const uint8_t *>(kAssets + offset), size_t(header.sizes[i])};
        configASSERT(asr::Crc32(inputs[i]) == header.crc[i]);
        offset = (offset + header.sizes[i] + 63) & ~size_t(63);
        configASSERT(offset <= kAssetsLimit);
    }
#ifdef SUNSTONE_MIMUS_PACED
    asr::SetInternalArena(internal_arena);
    mimus_yield_install();
#ifdef SUNSTONE_MIMUS_NATIVE
    const bool success = mimus_native_paced() ? paced_replay(inputs, 0) : mimus_transcribe_recorded(inputs,internal_arena);
#else
    const bool success = paced_replay(inputs, 0);
#endif
#else
    const bool success = mimus_transcribe_recorded(inputs,internal_arena);
#endif
#ifdef SUNSTONE_SMP_NEWLIB
    mimus_parallel_report();
#endif
#ifdef SUNSTONE_MIMUS_NATIVE
    mimus_native_finished(success);
    vTaskSuspend(nullptr);
#else
    platform_exit(success ? 0 : 1);
#endif
#endif
}
extern "C" void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    platform_panic(name, 0);
}
extern "C" void mimus_application_start(void)
{
#ifdef SUNSTONE_SMP_NEWLIB
    runtime_smp_init();
#endif
    runtime_init();
#ifdef SUNSTONE_SMP_NEWLIB
    mimus_parallel_init();
#endif
#if defined(SUNSTONE_MIMUS_USB) || defined(SUNSTONE_MIMUS_PACED)
    mimus_replay_init();
#endif
#ifdef SUNSTONE_MIMUS_USB
    configASSERT(RESULT->mode == 4);
    mimus_service_init();
    usb_runtime_start(4);
#endif
    configASSERT(xTaskCreateStatic(keep_recovery, "recovery", 1024, nullptr, 5, recovery_stack, &tasks[0]));
#ifdef SUNSTONE_SMP_NEWLIB
    configASSERT(xTaskCreateStaticAffinitySet(recognize, "mimus", 16384, nullptr, 3,
                                              recognition_stack, &tasks[1], 1));
#else
    configASSERT(xTaskCreateStatic(recognize, "mimus", 16384, nullptr, 3, recognition_stack, &tasks[1]));
#endif
}
#ifndef SUNSTONE_MIMUS_NATIVE
extern "C" void kernel_main(void)
{
    platform_init();
    mimus_application_start();
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
#endif
