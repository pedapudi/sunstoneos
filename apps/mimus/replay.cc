#include "replay.h"
#include <algorithm>
#include <array>
#include <limits>

namespace sunstone {
namespace {
constexpr uint32_t frame_samples = 320, sample_rate = 16000;
uint64_t sample_time(uint32_t samples) {
    return (uint64_t(samples) * 1000000 + sample_rate - 1) / sample_rate;
}
}
Replay::Replay(std::span<const int16_t> pcm) : pcm_(pcm) {
    // Bound index arithmetic, including the final padded analysis frame.
    if (pcm.empty() || pcm.size() > std::numeric_limits<int32_t>::max() - frame_samples)
        state_.store(State::Invalid, std::memory_order_relaxed);
    events_.reserve(4);
}
uint64_t Replay::next_due_us() const {
    return sample_time(uint32_t(std::min<size_t>(pcm_.size(), position_ + frame_samples)));
}
bool Replay::Publish(uint64_t elapsed_us) {
    using mimus::pipeline::EventType;
    for (const auto &event : events_) {
        const uint32_t end = std::min<uint32_t>(event.index, pcm_.size());
        if (event.type == EventType::kStart) {
            begin_ = end;
            speech_begin_ = event.speech_start;
        } else if (event.type == EventType::kEnd || event.type == EventType::kSplit) {
            const uint32_t speech_end = event.type == EventType::kSplit
                ? end : std::min(event.speech_end, end);
            if (end > begin_) {
                if (!queue_.Publish({begin_, end, speech_begin_, speech_end, elapsed_us})) {
                    state_.store(State::Overflow, std::memory_order_release);
                    return false;
                }
                ++segments_;
            }
            if (event.type == EventType::kSplit) {
                begin_ = end;
                speech_begin_ = std::max(speech_begin_, end);
            }
        }
    }
    return true;
}
void Replay::Pump(uint64_t elapsed_us) {
    if (state() != State::Running) return;
    if (cancel_.load(std::memory_order_relaxed)) {
        state_.store(State::Cancelled, std::memory_order_release);
        return;
    }
    if (elapsed_us < previous_us_) {
        state_.store(State::Invalid, std::memory_order_release);
        return;
    }
    previous_us_ = elapsed_us;
    const uint64_t due = next_due_us();
    if (elapsed_us < due) return;
    max_lateness_us_ = std::max(max_lateness_us_, elapsed_us - due);
    const uint32_t count = std::min<uint32_t>(frame_samples, pcm_.size() - position_);
    const auto frame = pcm_.subspan(position_, count);
    events_.clear();
    if (count == frame_samples) segmenter_.ProcessFrame(frame, &events_);
    else {
        // Padding is for VAD only. Recognizer offsets never include it.
        std::array<int16_t, frame_samples> tail{};
        std::copy(frame.begin(), frame.end(), tail.begin());
        segmenter_.ProcessFrame(tail, &events_);
    }
    position_ += count;
    if (!Publish(elapsed_us)) return;
    if (position_ == pcm_.size()) {
        events_.clear();
        segmenter_.Stop(&events_);
        if (Publish(elapsed_us)) state_.store(State::Complete, std::memory_order_release);
    }
}
}
