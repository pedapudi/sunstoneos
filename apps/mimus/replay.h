#ifndef SUNSTONE_MIMUS_REPLAY_H
#define SUNSTONE_MIMUS_REPLAY_H
#include "mimus/pipeline/audio_work_queue.h"
#include "mimus/pipeline/segmenter.h"
#include <atomic>
#include <span>

namespace sunstone {
// Offsets refer to an immutable recording owned until both tasks finish.
struct ReplaySegment {
    uint32_t begin, end, speech_begin, speech_end;
    uint64_t ready_us;
};
class Replay {
public:
    enum class State { Running, Complete, Overflow, Cancelled, Invalid };
    explicit Replay(std::span<const int16_t> pcm);
    // Single producer. Processes at most one due 20 ms frame per call.
    // elapsed_us is time since capture began; calls must be monotonic.
    void Pump(uint64_t elapsed_us);
    uint64_t next_due_us() const;
    void Cancel() { cancel_.store(true, std::memory_order_relaxed); }
    State state() const { return state_.load(std::memory_order_acquire); }
    // Single consumer. Observe state() BEFORE Take() when checking for EOF,
    // so a completed producer's final publication cannot be missed.
    std::optional<ReplaySegment> Take() { return queue_.Take(); }
    // Read statistics only after the producer has finished using the object.
    uint32_t samples() const { return position_; }
    uint32_t segments() const { return segments_; }
    uint64_t max_lateness_us() const { return max_lateness_us_; }
private:
    bool Publish(uint64_t elapsed_us);
    std::span<const int16_t> pcm_;
    mimus::pipeline::Segmenter segmenter_;
    std::vector<mimus::pipeline::SegmentEvent> events_;
    mimus::pipeline::AudioWorkQueue<ReplaySegment, 4> queue_;
    std::atomic<State> state_{State::Running};
    std::atomic<bool> cancel_{false};
    uint32_t position_ = 0, begin_ = 0, speech_begin_ = 0, segments_ = 0;
    uint64_t max_lateness_us_ = 0, previous_us_ = 0;
};
}
#endif
