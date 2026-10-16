#include "replay_runtime.h"
#include "platform.h"
#include "boot.h"
#include "runtime.h"
#include "mimus/asr/recognizer.h"
#include "mimus/asr/parallel.h"
#include "mimus/asr/platform.h"
#include "mimus/asr/scratch.h"
#include "mimus/asr/wav.h"
#include "mimus/pipeline/formatter.h"
#include <cstdio>
#include <string>

namespace {
struct Recognition {
    mimus::asr::Recognizer &engine;
    mimus::asr::Arena &arena;
    mimus::pipeline::Formatter formatter;
    unsigned segments = 0;
};
bool error(const char *message) {
    platform_puts("ERROR "); platform_puts(message); platform_puts("\n");
    return false;
}
bool consume(void *context, std::span<const int16_t> pcm,
             const sunstone::ReplaySegment &segment, uint64_t start_ns) {
    using namespace mimus;
    auto &state = *static_cast<Recognition *>(context);
#ifndef SUNSTONE_SMP_NEWLIB
    if (RESULT->log_length > RESULT_SIZE - LOG_OFFSET - 2048)
        return error("paced result capacity exhausted");
#endif
    const uint64_t start = asr::platform::MonotonicNanos();
    state.arena.Reset();
    auto result = state.engine.Recognize(pcm, state.arena);
    if (!result) return error(asr::ErrorName(result.error()));
    const uint64_t compute_us = (asr::platform::MonotonicNanos() - start) / 1000;
    auto words = pipeline::SplitWords(result->text);
    if (words.size() != result->words.size()) return error("word alignment differs");
    std::string marks, capitals;
    for (size_t i = 0; i < words.size(); ++i) {
        const auto &word = result->words[i];
        words[i].scored = word.scored;
        words[i].mark = static_cast<uint8_t>(word.mark);
        words[i].capital = word.capital;
        marks += word.scored ? char('0' + unsigned(word.mark)) : '9';
        capitals += word.capital ? '1' : '0';
    }
    const pipeline::SegmentTiming timing{segment.speech_begin, segment.speech_end, 0};
    std::optional<pipeline::BoundaryDecision> boundary;
    std::string released;
    const auto held = state.formatter.held();
    if (held && held->speech_end) {
        const uint32_t pause = segment.speech_begin >= *held->speech_end
            ? (segment.speech_begin - *held->speech_end) / 16 : 0;
        if (pause >= 2500) released = state.formatter.ReleaseHeld();
        else if (!held->context.empty() && !result->punct_context.empty()) {
            const auto *head = state.engine.punct();
            if (head && head->has_boundary()) {
                auto decision = head->Boundary(held->context, result->punct_context, pause, state.arena);
                if (!decision) return error(asr::ErrorName(decision.error()));
                boundary = pipeline::BoundaryDecision{held->id, uint8_t(decision->mark), decision->capital};
            }
        }
    }
    std::string formatted = state.formatter.Format(words, timing, result->punct_context, boundary);
    if (!released.empty()) formatted = released + (formatted.empty() ? "" : " ") + formatted;
#ifndef SUNSTONE_SMP_NEWLIB
    // Reserve headers and the final replay/formatting summary before any part
    // of this segment is written. The platform log otherwise truncates writes.
    const size_t needed = result->text.size() + marks.size() + capitals.size() + formatted.size() + 1024;
    if (needed > RESULT_SIZE - LOG_OFFSET - 1 - RESULT->log_length)
        return error("paced result capacity exhausted");
#endif
    const uint64_t complete_us = (asr::platform::MonotonicNanos() - start_ns) / 1000;
    const uint64_t speech_end_us = uint64_t(segment.speech_end) * 1000000 / 16000;
    char line[320];
    snprintf(line, sizeof(line),
             "SEGMENT id=%u begin=%u end=%u speech_begin=%u speech_end=%u ready_us=%llu complete_us=%llu latency_us=%llu\n",
             ++state.segments, segment.begin, segment.end, segment.speech_begin, segment.speech_end,
             static_cast<unsigned long long>(segment.ready_us),
             static_cast<unsigned long long>(complete_us),
             static_cast<unsigned long long>(complete_us >= speech_end_us ? complete_us - speech_end_us : 0));
    platform_puts(line);
    snprintf(line, sizeof(line), "RESULT feat_crc=%08x lp_crc=%08x frames=%u samples=%u compute_us=%llu\n",
             result->feature_crc, result->log_prob_crc, unsigned(result->frames), unsigned(pcm.size()),
             static_cast<unsigned long long>(compute_us));
    platform_puts(line);
    platform_puts("TEXT "); platform_puts(result->text.c_str()); platform_puts("\nMARKS ");
    platform_puts(marks.c_str()); platform_puts("\nCAPS "); platform_puts(capitals.c_str());
    platform_puts("\nFORMATTED "); platform_puts(formatted.c_str()); platform_puts("\n");
    return true;
}
}

bool mimus_transcribe_paced(std::span<const uint8_t> inputs[3]) {
    using namespace mimus;
    auto model = asr::Model::Load(inputs[0], inputs[1], true, asr::kTargetOrangePi);
    if (!model) return error(asr::ErrorName(model.error()));
    auto engine = asr::Recognizer::Create(*model);
    if (!engine) return error(asr::ErrorName(engine.error()));
    auto audio = asr::ParseWav(inputs[2]);
    if (!audio) return error(asr::ErrorName(audio.error()));
    asr::Arena arena({reinterpret_cast<uint8_t *>(0x46000000), 16 * 1024 * 1024});
    Recognition state{*engine, arena, pipeline::Formatter{}, 0};
    if (!mimus_replay_run(*audio, consume, &state)) return false;
    const auto tail = state.formatter.ReleaseHeld();
    platform_puts("FLUSH "); platform_puts(tail.c_str()); platform_puts("\n");
    char line[128];
    snprintf(line, sizeof(line), "MEMORY arena_peak=%zu heap_reserved_peak=%zu\n",
             arena.peak(), runtime_heap_peak());
    platform_puts(line);
    platform_puts("PASS mimus: paced utterance replay completed\n");
    return true;
}
