#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "mimus/asr/model.h"
#include "mimus/asr/recognizer.h"
#include "mimus/asr/parallel.h"
#include "mimus/asr/platform.h"
#include "mimus/asr/profile.h"
#include "mimus/asr/wav.h"
#include "mimus/asr/scratch.h"
#include "mimus/pipeline/formatter.h"
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

extern "C" void runtime_init(void);
static StaticTask_t tasks[2];
static StackType_t recognition_stack[16384], recovery_stack[1024];
static uint64_t application_deadline;
alignas(64) static uint8_t internal_arena[97280];
constexpr uintptr_t kAssets = 0x43000000;
constexpr size_t kAssetsLimit = 32 * 1024 * 1024;
struct Assets {
    char magic[8];
    uint64_t sizes[3];
    uint32_t crc[3];
};
static void keep_recovery(void *)
{
    for (;;) {
        if (counter() > application_deadline) platform_panic("recognition deadline", 0);
        register uint64_t operation __asm__("x0") = 0x734;
        __asm__ volatile("smc #0" : "+r"(operation) :: "memory");
        configASSERT(operation == 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
static void recognize(void *)
{
    using namespace mimus;
    const auto &header = *reinterpret_cast<const Assets *>(kAssets);
    configASSERT(memcmp(header.magic, "SUNMIM01", 8) == 0);
    size_t offset = 128;
    std::span<const uint8_t> inputs[3];
    for (unsigned i = 0; i < 3; ++i) {
        configASSERT(header.sizes[i] && header.sizes[i] <= kAssetsLimit - offset);
        inputs[i] = {reinterpret_cast<const uint8_t *>(kAssets + offset), size_t(header.sizes[i])};
        configASSERT(asr::Crc32(inputs[i]) == header.crc[i]);
        offset = (offset + header.sizes[i] + 63) & ~size_t(63);
        configASSERT(offset <= kAssetsLimit);
    }
    auto model = asr::Model::Load(inputs[0], inputs[1], true, asr::kTargetOrangePi);
    if (!model) platform_panic(asr::ErrorName(model.error()), 0);
    auto engine = asr::Recognizer::Create(*model);
    if (!engine) platform_panic(asr::ErrorName(engine.error()), 0);
    auto audio = asr::ParseWav(inputs[2]);
    if (!audio) platform_panic(asr::ErrorName(audio.error()), 0);
    asr::Arena arena({reinterpret_cast<uint8_t *>(0x46000000), 16 * 1024 * 1024});
    asr::SetInternalArena(internal_arena);
    asr::SetYieldHook([] { taskYIELD(); });
    uint64_t start = asr::platform::MonotonicNanos();
    auto result = engine->Recognize(*audio, arena);
    uint64_t elapsed = (asr::platform::MonotonicNanos() - start) / 1000;
    if (!result) platform_panic(asr::ErrorName(result.error()), 0);
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
    platform_puts("PASS mimus: recorded audio recognized locally\n");
    platform_exit(0);
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
    application_deadline = counter() + 180 * COUNTER_HZ;
    configASSERT(xTaskCreateStatic(keep_recovery, "recovery", 1024, nullptr, 5, recovery_stack, &tasks[0]));
    configASSERT(xTaskCreateStatic(recognize, "mimus", 16384, nullptr, 3, recognition_stack, &tasks[1]));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
