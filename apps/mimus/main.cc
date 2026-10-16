#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "runtime.h"
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
static void report_clock()
{
#ifndef MONITOR_QEMU
    // Read-only decode of the pinned A733 kernel's RTC mux and CPU-L NKMP PLL.
    const uint32_t oscillator = *reinterpret_cast<volatile uint32_t *>(0x07090160);
    const uint32_t pll = *reinterpret_cast<volatile uint32_t *>(0x08871000);
    const uint32_t select = *reinterpret_cast<volatile uint32_t *>(0x0887101c);
    constexpr uint32_t references[] = {24000000, 19200000, 26000000, 24000000};
    const uint32_t reference = references[(oscillator >> 14) & 3];
    const unsigned source = (select >> 24) & 7;
    const unsigned multiplier = (pll >> 8) & 255;
    const unsigned divider = 1u << ((select >> 16) & 3);
    uint64_t configured_hz = 0;
    if (source <= 2) configured_hz = reference;
    else if (source == 3 && multiplier && (pll & 0xf8000000u) == 0xf8000000u)
        configured_hz = uint64_t(reference) * multiplier / divider;
    char line[192];
    snprintf(line, sizeof(line),
             "CLOCK rtc=%08x pll_l=%08x select_l=%08x reference_hz=%u configured_cpu_hz=%llu counter_hz=%llu\n",
             oscillator, pll, select, reference,
             static_cast<unsigned long long>(configured_hz),
             static_cast<unsigned long long>(COUNTER_HZ));
    platform_puts(line);
#endif
}
static void keep_recovery(void *)
{
    for (;;) {
        if (counter() > application_deadline.load(std::memory_order_relaxed)) platform_panic("recognition deadline", 0);
#ifdef SUNSTONE_MIMUS_USB
        // The recovery task must not keep a stalled USB task alive indefinitely.
        if (counter() - usb_runtime_last_progress() > 5 * COUNTER_HZ)
            platform_panic("USB task stalled", 0);
#endif
        register uint64_t operation __asm__("x0") = 0x734;
        __asm__ volatile("smc #0" : "+r"(operation) :: "memory");
        configASSERT(operation == 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
static bool recognition_error(const char *message)
{
    platform_puts("ERROR "); platform_puts(message); platform_puts("\n");
    return false;
}
static bool transcribe(std::span<const uint8_t> inputs[3])
{
    using namespace mimus;
    auto model = asr::Model::Load(inputs[0], inputs[1], true, asr::kTargetOrangePi);
    if (!model) return recognition_error(asr::ErrorName(model.error()));
    auto engine = asr::Recognizer::Create(*model);
    if (!engine) return recognition_error(asr::ErrorName(engine.error()));
    auto audio = asr::ParseWav(inputs[2]);
    if (!audio) return recognition_error(asr::ErrorName(audio.error()));
    asr::Arena arena({reinterpret_cast<uint8_t *>(0x46000000), 16 * 1024 * 1024});
    asr::SetInternalArena(internal_arena);
    asr::SetYieldHook([] { taskYIELD(); });
    report_clock();
    asr::ResetProfile();
    uint64_t start = asr::platform::MonotonicNanos();
    auto result = engine->Recognize(*audio, arena);
    uint64_t elapsed = (asr::platform::MonotonicNanos() - start) / 1000;
    if (!result) return recognition_error(asr::ErrorName(result.error()));
    char metrics[192];
    snprintf(metrics, sizeof(metrics), "RESULT feat_crc=%08x lp_crc=%08x frames=%u samples=%u compute_us=%llu\n",
             result->feature_crc, result->log_prob_crc, unsigned(result->frames), unsigned(audio->size()),
             static_cast<unsigned long long>(elapsed));
    platform_puts(metrics);
    platform_puts("TEXT "); platform_puts(result->text.c_str()); platform_puts("\n");
    std::string marks, capitals;
    auto words = pipeline::SplitWords(result->text);
    configASSERT(result->punct && words.size() == result->words.size());
    for (size_t i = 0; i < words.size(); ++i) {
        const auto &word = result->words[i];
        words[i].scored = word.scored;
        words[i].mark = static_cast<uint8_t>(word.mark);
        words[i].capital = word.capital;
        marks += word.scored ? char('0' + unsigned(word.mark)) : '9';
        capitals += word.capital ? '1' : '0';
    }
    platform_puts("MARKS "); platform_puts(marks.c_str()); platform_puts("\nCAPS ");
    platform_puts(capitals.c_str()); platform_puts("\n");
    pipeline::FormatSequence sequence;
    std::string typed = sequence.Add(words, audio->size(), result->punct_context, nullptr, nullptr);
    typed += sequence.Finish();
    platform_puts("FORMATTED "); platform_puts(typed.c_str()); platform_puts("\n");
    for (int i = 0; i < static_cast<int>(asr::Stage::kCount); ++i) {
        snprintf(metrics, sizeof(metrics), "PROFILE stage=%s us=%llu macs=%llu\n",
                 asr::StageName(static_cast<asr::Stage>(i)),
                 static_cast<unsigned long long>(asr::Profile()[i]),
                 static_cast<unsigned long long>(asr::ProfileMacs()[i]));
        platform_puts(metrics);
    }
    snprintf(metrics, sizeof(metrics), "MEMORY arena_peak=%zu heap_reserved_peak=%zu\n",
             arena.peak(), runtime_heap_peak());
    platform_puts(metrics);
    platform_puts("PASS mimus: recorded audio recognized locally\n");
    return true;
}
static void recognize(void *)
{
    std::span<const uint8_t> inputs[3];
#ifdef SUNSTONE_MIMUS_USB
    for (;;) {
        mimus_service_wait(inputs);
        application_deadline.store(counter() + 180 * COUNTER_HZ, std::memory_order_relaxed);
        bool success = transcribe(inputs);
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
    platform_exit(transcribe(inputs) ? 0 : 1);
#endif
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
#ifdef SUNSTONE_MIMUS_USB
    configASSERT(RESULT->mode == 4);
    mimus_service_init();
    usb_runtime_start(4);
#endif
    configASSERT(xTaskCreateStatic(keep_recovery, "recovery", 1024, nullptr, 5, recovery_stack, &tasks[0]));
    configASSERT(xTaskCreateStatic(recognize, "mimus", 16384, nullptr, 3, recognition_stack, &tasks[1]));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
