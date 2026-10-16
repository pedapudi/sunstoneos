// Actual replay source and segmenter, with a controlled capture clock.
#include "../apps/mimus/replay.h"
#include <cassert>
#include <cstdio>
#include <vector>

using sunstone::Replay;
using sunstone::ReplaySegment;
static std::vector<int16_t> recording(unsigned bursts, unsigned tail = 0) {
    std::vector<int16_t> pcm(16000);
    for (unsigned burst = 0; burst < bursts; ++burst) {
        for (unsigned i = 0; i < 16000; ++i) pcm.push_back(i & 1 ? 12000 : -12000);
        if (burst + 1 < bursts) pcm.resize(pcm.size() + 16000);
    }
    for (unsigned i = 0; i < tail; ++i) pcm.push_back(i & 1 ? 12000 : -12000);
    return pcm;
}
static std::vector<ReplaySegment> run(std::span<const int16_t> pcm, bool jitter) {
    Replay replay(pcm);
    std::vector<ReplaySegment> segments;
    uint64_t now = 0;
    while (replay.state() == Replay::State::Running) {
        const uint64_t due = replay.next_due_us();
        const unsigned before = replay.samples();
        // Data must remain unavailable until the last sample of its frame arrives.
        if (due > now) {
            replay.Pump(due - 1);
            assert(replay.samples() == before);
        }
        now = due + (jitter ? 37000 : 0);
        replay.Pump(now);
        assert(uint64_t(replay.samples()) * 1000000 <= now * 16000);
        while (auto segment = replay.Take()) segments.push_back(*segment);
    }
    assert(replay.state() == Replay::State::Complete);
    assert(replay.samples() == pcm.size());
    assert(replay.max_lateness_us() == (jitter ? 37000 : 0));
    return segments;
}
int main() {
    auto pcm = recording(2, 117);
    auto on_time = run(pcm, false), late = run(pcm, true);
    assert(on_time.size() == 2 && late.size() == 2);
    assert(on_time[0].begin == 11200 && on_time[0].end == 40000);
    assert(on_time[0].speech_begin == 16000 && on_time[0].speech_end == 32000);
    assert(on_time[0].ready_us == 2800000);
    assert(on_time[1].begin == 43200 && on_time[1].end == pcm.size());
    assert(on_time[1].speech_begin == 48000 && on_time[1].speech_end == pcm.size());
    for (unsigned i = 0; i < on_time.size(); ++i) {
        assert(on_time[i].begin == late[i].begin && on_time[i].end == late[i].end);
        assert(late[i].ready_us == on_time[i].ready_us + 37000);
    }
    auto many = recording(6);
    Replay overloaded(many);
    while (overloaded.state() == Replay::State::Running) overloaded.Pump(overloaded.next_due_us());
    assert(overloaded.state() == Replay::State::Overflow && overloaded.segments() == 4);
    unsigned retained = 0;
    while (overloaded.Take()) ++retained;
    assert(retained == 4); // A failed publication cannot replace queued audio.
    Replay cancelled(pcm);
    cancelled.Cancel(); cancelled.Pump(0);
    assert(cancelled.state() == Replay::State::Cancelled && !cancelled.Take());
    Replay backwards(pcm);
    backwards.Pump(1000); backwards.Pump(999);
    assert(backwards.state() == Replay::State::Invalid);
    Replay empty({});
    assert(empty.state() == Replay::State::Invalid);
    std::puts("PASS paced sample deadlines, VAD boundaries, partial frame, late capture, bounded queue, cancellation and invalid clock.");
}
