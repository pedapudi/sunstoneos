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
#include <optional>
#include <string>
#ifdef SUNSTONE_MIMUS_NATIVE
// The integrated OS image supplies its CPU clock record; other native images
// leave the weak reference null and print no clock line.
extern "C" __attribute__((weak)) int os_cpu_clock_line(char *line, size_t bytes);
#endif
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
#elif defined(SUNSTONE_MIMUS_NATIVE)
    char line[192];
    if (os_cpu_clock_line && os_cpu_clock_line(line, sizeof(line)) > 0) platform_puts(line);
#endif
}
static bool recognition_error(const char *message)
{
    platform_puts("ERROR "); platform_puts(message); platform_puts("\n");
    return false;
}
namespace {
// One loaded model and recognizer, owned by the single recognition task. The
// recognizer refers to the model, and the model to the caller's spans.
// crc_checked records whether the load ran the per-tensor CRC pass; an engine
// loaded without it serves only callers that pass kMimusVerified.
struct Engine {
    std::optional<mimus::asr::Model> model;
    std::optional<mimus::asr::Recognizer> recognizer;
    std::span<const uint8_t> base, overlay;
    bool crc_checked = false;
};
// The bare-metal images register no exit-time destructors.
[[clang::no_destroy]] Engine engine;
std::atomic<bool> engine_stale{false};
// Yield to equal-priority tasks at most once per quantum. Preemption and time
// slicing already apply; the supervised checkpoint still runs on every call.
constexpr uint64_t kYieldQuantumNs = 1000000;
constexpr uint64_t kYieldNow = UINT64_MAX;
std::atomic<uint64_t> yield_last{kYieldNow}, yields{0};
void yield_hook()
{
#ifdef SUNSTONE_MIMUS_SUPERVISED
    mimus_native_checkpoint();
#endif
    const uint64_t now = mimus::asr::platform::MonotonicNanos();
    const uint64_t last = yield_last.load(std::memory_order_relaxed);
    if (last != kYieldNow && now - last < kYieldQuantumNs) return;
    yield_last.store(now, std::memory_order_relaxed);
    yields.fetch_add(1, std::memory_order_relaxed);
    taskYIELD();
}
bool same(std::span<const uint8_t> a, std::span<const uint8_t> b)
{
    return a.data() == b.data() && a.size() == b.size();
}
}
void mimus_yield_install()
{
    yield_last.store(kYieldNow, std::memory_order_relaxed);
    yields.store(0, std::memory_order_relaxed);
    mimus::asr::SetYieldHook(yield_hook);
}
uint64_t mimus_yield_count() { return yields.load(std::memory_order_relaxed); }
void mimus_engine_release()
{
    engine.recognizer.reset();
    engine.model.reset();
    engine.base = engine.overlay = {};
    engine.crc_checked = false;
}
void mimus_engine_invalidate() { engine_stale.store(true, std::memory_order_release); }
const mimus::asr::Recognizer *mimus_engine_open(std::span<const uint8_t> inputs[3], unsigned options)
{
    using namespace mimus;
    if (engine_stale.exchange(false, std::memory_order_acq_rel)) mimus_engine_release();
    const bool verify = !(options & kMimusVerified);
    if (engine.recognizer && same(engine.base, inputs[0]) && same(engine.overlay, inputs[1]) &&
        (engine.crc_checked || !verify))
        return &*engine.recognizer;
    mimus_engine_release();
    const uint64_t start = asr::platform::MonotonicNanos();
    auto model = asr::Model::Load(inputs[0], inputs[1], verify, asr::kTargetOrangePi);
    if (!model) { recognition_error(asr::ErrorName(model.error())); return nullptr; }
    engine.model.emplace(std::move(*model));
    auto recognizer = asr::Recognizer::Create(*engine.model);
    if (!recognizer) {
        mimus_engine_release();
        recognition_error(asr::ErrorName(recognizer.error()));
        return nullptr;
    }
    engine.recognizer.emplace(std::move(*recognizer));
    engine.base = inputs[0];
    engine.overlay = inputs[1];
    engine.crc_checked = verify;
    char line[96];
    snprintf(line, sizeof(line), "LOAD load_us=%llu verify_crc=%u\n",
             static_cast<unsigned long long>((asr::platform::MonotonicNanos() - start) / 1000), unsigned(verify));
    platform_puts(line);
    return &*engine.recognizer;
}
static bool recognize(std::span<const uint8_t> inputs[3], std::span<uint8_t> internal_arena,
                      const std::atomic<bool> *stop, unsigned options)
{
    using namespace mimus;
    auto cancelled=[&]{return stop&&stop->load(std::memory_order_acquire);};
    if(cancelled())return false;
    const auto *recognizer = mimus_engine_open(inputs, options);
    if (!recognizer) return false;
    if(cancelled())return false;
    // PCM-to-formatted interval: WAV parsing through the formatting flush.
    const uint64_t total_start = asr::platform::MonotonicNanos();
    auto audio = asr::ParseWav(inputs[2]);
    if (!audio) return recognition_error(asr::ErrorName(audio.error()));
    if(cancelled())return false;
    asr::Arena arena({reinterpret_cast<uint8_t *>(kMimusArenaBase), kMimusArenaBytes});
    asr::SetInternalArena(internal_arena);
    mimus_yield_install();
    report_clock();
    asr::ResetProfile();
    uint64_t start = asr::platform::MonotonicNanos();
    auto result = recognizer->Recognize(*audio, arena, stop);
    uint64_t elapsed = (asr::platform::MonotonicNanos() - start) / 1000;
#ifdef SUNSTONE_MIMUS_NATIVE
    // A second record shows whether the clock changed during recognition.
    report_clock();
#endif
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
    const uint64_t total = (asr::platform::MonotonicNanos() - total_start) / 1000;
    platform_puts("FORMATTED "); platform_puts(typed.c_str()); platform_puts("\n");
    snprintf(metrics, sizeof(metrics), "TIMING total_us=%llu compute_us=%llu\n",
             static_cast<unsigned long long>(total), static_cast<unsigned long long>(elapsed));
    platform_puts(metrics);
    for (int i = 0; i < static_cast<int>(asr::Stage::kCount); ++i) {
        snprintf(metrics, sizeof(metrics), "PROFILE stage=%s us=%llu macs=%llu\n",
                 asr::StageName(static_cast<asr::Stage>(i)),
                 static_cast<unsigned long long>(asr::Profile()[i]),
                 static_cast<unsigned long long>(asr::ProfileMacs()[i]));
        platform_puts(metrics);
    }
    snprintf(metrics, sizeof(metrics), "MEMORY arena_peak=%zu heap_reserved_peak=%zu yields=%llu\n",
             arena.peak(), runtime_heap_peak(), static_cast<unsigned long long>(mimus_yield_count()));
    platform_puts(metrics);
    platform_puts("PASS mimus: recorded audio recognized locally\n");
    return true;
}
bool mimus_engine_finish(unsigned options, bool success)
{
    if (!(options & kMimusWarm)) mimus_engine_release();
    else if (!success) mimus_engine_invalidate();
    return success;
}
bool mimus_transcribe_recorded(std::span<const uint8_t> inputs[3],std::span<uint8_t> internal_arena,
                               const std::atomic<bool> *stop,unsigned options)
{
    return mimus_engine_finish(options, recognize(inputs, internal_arena, stop, options));
}
bool mimus_transcribe_recorded(std::span<const uint8_t> inputs[3],std::span<uint8_t> internal_arena,
                               const std::atomic<bool> *stop)
{
    return mimus_transcribe_recorded(inputs, internal_arena, stop, 0);
}
