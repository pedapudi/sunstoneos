#ifndef SUNSTONE_MIMUS_REPLAY_RUNTIME_H
#define SUNSTONE_MIMUS_REPLAY_RUNTIME_H
#include "replay.h"
void mimus_replay_init();
using ReplayConsumer = bool (*)(void *, std::span<const int16_t>,
                               const sunstone::ReplaySegment &, uint64_t start_ns);
bool mimus_replay_run(std::span<const int16_t> pcm, ReplayConsumer consumer, void *context);
bool mimus_transcribe_paced(std::span<const uint8_t> inputs[3]);
#endif
