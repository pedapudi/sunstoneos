// Host arithmetic/segmentation reference. No wall-clock pacing or OS emulation.
#include "../apps/mimus/recorded.h"
#include "../apps/mimus/replay_runtime.h"
#include "boot.h"
#include "mimus/asr/platform.h"
#include "mimus/asr/scratch.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sys/mman.h>
#include <vector>

namespace mimus::asr::platform {
uint64_t MonotonicNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
extern "C" void platform_puts(const char *text) {
    std::fputs(text, stdout);
    RESULT->log_length = RESULT->log_length + std::strlen(text);
}
extern "C" size_t runtime_heap_peak() { return 0; }
bool mimus_replay_run(std::span<const int16_t> pcm, ReplayConsumer consumer, void *context) {
    sunstone::Replay replay(pcm);
    const uint64_t start = mimus::asr::platform::MonotonicNanos();
    unsigned consumed = 0;
    while (replay.state() == sunstone::Replay::State::Running) {
        replay.Pump(replay.next_due_us());
        while (auto segment = replay.Take()) {
            if (!consumer(context, pcm.subspan(segment->begin, segment->end - segment->begin), *segment, start))
                return false;
            ++consumed;
        }
    }
    char line[160];
    std::snprintf(line, sizeof(line), "REPLAY samples=%u segments=%u consumed=%u state=%u\n",
                  replay.samples(), replay.segments(), consumed, unsigned(replay.state()));
    platform_puts(line);
    return replay.state() == sunstone::Replay::State::Complete;
}
int main(int argc, char **argv) {
    if (argc != 4) {
        std::fputs("usage: mimus-replay-reference BASE OVERLAY WAV\n", stderr);
        return 2;
    }
    for (auto [base, size] : {std::pair<uintptr_t, size_t>{RESULT_BASE, RESULT_SIZE},
                              {kMimusArenaBase, kMimusArenaBytes}}) {
        if (mmap(reinterpret_cast<void *>(base), size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != reinterpret_cast<void *>(base))
            return 2;
    }
    std::vector<uint8_t> bytes[3];
    std::span<const uint8_t> inputs[3];
    for (int i = 0; i < 3; ++i) {
        std::ifstream file(argv[i + 1], std::ios::binary);
        if (!file) return 2;
        bytes[i] = {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        inputs[i] = bytes[i];
    }
    alignas(64) static uint8_t internal[97280];
    mimus::asr::SetInternalArena(internal);
    std::puts("REFERENCE unpaced host execution; timing and heap metrics do not represent the board.");
    return mimus_transcribe_paced(inputs) ? 0 : 1;
}
