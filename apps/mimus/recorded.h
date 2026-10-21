#ifndef SUNSTONE_MIMUS_RECORDED_H
#define SUNSTONE_MIMUS_RECORDED_H
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
namespace mimus::asr { class Recognizer; }
// Fixed inference arena shared by recorded and paced recognition.
inline constexpr uintptr_t kMimusArenaBase = 0x46000000;
inline constexpr size_t kMimusArenaBytes = 16 * 1024 * 1024;
// kMimusWarm keeps the loaded model and recognizer in static storage owned by
// the single calling recognition task and reuses them for later calls on the
// same model spans. Without it, both are destroyed before return. Pass
// kMimusVerified only after the caller authenticated the exact model bytes;
// loading then skips the per-tensor CRC pass.
enum : unsigned { kMimusWarm = 1, kMimusVerified = 2 };
// Existing finite WAV/model recognition and formatting. One caller owns the
// model/WAV inputs and internal scratch through return and object destruction.
// Default callers retain the original uncancelled recognition behavior.
bool mimus_transcribe_recorded(std::span<const uint8_t> inputs[3],std::span<uint8_t> internal_arena,
                               const std::atomic<bool> *stop=nullptr);
// The same recognition with engine options.
bool mimus_transcribe_recorded(std::span<const uint8_t> inputs[3],std::span<uint8_t> internal_arena,
                               const std::atomic<bool> *stop,unsigned options);
// Paced replay of one WAV with an open recognizer. It is defined with the
// replay sources in paced.cc.
bool mimus_paced_run(const mimus::asr::Recognizer &recognizer,std::span<const uint8_t> wav);
// Returns the recognizer for inputs[0..1], loading it when absent, stale or
// bound to other spans, and reports LOAD once per load. Owner task only.
const mimus::asr::Recognizer *mimus_engine_open(std::span<const uint8_t> inputs[3],unsigned options);
// Destroys a retained engine. Owner task only.
void mimus_engine_release();
// Ends one request: destroys a cold engine, and marks a warm engine stale after
// a failure so that the owner's next request reloads it. Returns success.
bool mimus_engine_finish(unsigned options,bool success);
// Marks a retained engine stale before its model buffers change. Any task may
// call it; the owner destroys and reloads the engine at its next request.
void mimus_engine_invalidate();
// Installs the shared yield hook and restarts its quantum and yield count.
void mimus_yield_install();
uint64_t mimus_yield_count();
#endif
