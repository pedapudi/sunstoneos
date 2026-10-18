#ifndef SUNSTONE_MIMUS_RECORDED_H
#define SUNSTONE_MIMUS_RECORDED_H
#include <atomic>
#include <cstdint>
#include <span>
// Existing finite WAV/model recognition and formatting. One caller owns the
// model/WAV inputs and internal scratch through return and object destruction.
// Default callers retain the original uncancelled recognition behavior.
bool mimus_transcribe_recorded(std::span<const uint8_t> inputs[3],std::span<uint8_t> internal_arena,
                               const std::atomic<bool> *stop=nullptr);
#endif
