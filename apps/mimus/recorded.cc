#include "recorded.h"
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "timing.h"
// Named by path: the I2S image puts its unrelated apps/i2s/runtime.h first.
#include "../../include/runtime.h"
#ifdef SUNSTONE_MIMUS_NATIVE
#include "native.h"
#endif
#include "mimus/asr/model.h"
#include "mimus/asr/recognizer.h"
#include "mimus/asr/platform.h"
#include "mimus/asr/parallel.h"
#include "mimus/asr/profile.h"
#include "mimus/asr/wav.h"
#include "mimus/asr/scratch.h"
#include "mimus/pipeline/formatter.h"
#include <cstdio>
#include <string>
static void report_clock()
{
#if !defined(MONITOR_QEMU) && !defined(SUNSTONE_MIMUS_NATIVE)
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
static bool recognition_error(const char *message)
{
    platform_puts("ERROR "); platform_puts(message); platform_puts("\n");
    return false;
}
bool mimus_transcribe_recorded(std::span<const uint8_t> inputs[3],std::span<uint8_t> internal_arena,
                               const std::atomic<bool> *stop)
{
    using namespace mimus;
    auto cancelled=[&]{return stop&&stop->load(std::memory_order_acquire);};
    if(cancelled())return false;
    auto model = asr::Model::Load(inputs[0], inputs[1], true, asr::kTargetOrangePi);
    if (!model) return recognition_error(asr::ErrorName(model.error()));
    if(cancelled())return false;
    auto engine = asr::Recognizer::Create(*model);
    if (!engine) return recognition_error(asr::ErrorName(engine.error()));
    if(cancelled())return false;
    auto audio = asr::ParseWav(inputs[2]);
    if (!audio) return recognition_error(asr::ErrorName(audio.error()));
    if(cancelled())return false;
    asr::Arena arena({reinterpret_cast<uint8_t *>(0x46000000), 16 * 1024 * 1024});
    asr::SetInternalArena(internal_arena);
    asr::SetYieldHook([] {
#ifdef SUNSTONE_MIMUS_SUPERVISED
        mimus_native_checkpoint();
#endif
        taskYIELD(); });
    report_clock();
    asr::ResetProfile();
    uint64_t start = asr::platform::MonotonicNanos();
    auto result = engine->Recognize(*audio, arena, stop);
    uint64_t elapsed = (asr::platform::MonotonicNanos() - start) / 1000;
    if (!result) return recognition_error(asr::ErrorName(result.error()));
    if(cancelled())return false;
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
    if(cancelled())return false;
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
