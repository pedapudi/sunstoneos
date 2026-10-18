#ifndef SUNSTONE_I2S_SESSION_H
#define SUNSTONE_I2S_SESSION_H
#include "a733_native_usb.h"
#include "a733_dma_pool_session.h"
#define A733_STORAGE_SESSION_ADDRESS UINT64_C(0x41f00000)
#define I2S_SESSION_MAGIC UINT64_C(0x4153373341555331)
#define I2S_CONTRACT_MAGIC UINT64_C(0x4153373341554331)
#define I2S_SESSION_SECONDS 60u
#ifdef SUNSTONE_I2S_TRANSCRIBE
#include "recognition.h"
#define I2S_SESSION_VERSION 2u
#else
#define I2S_SESSION_VERSION 1u
#endif
#define I2S_PERIODS 4u
#define I2S_PERIOD_BYTES 4096u
#define I2S_PCM_BYTES (I2S_PERIODS*I2S_PERIOD_BYTES)
/* Immutable value-only launch record. Evidence binds real DMA prior drain,
 * remote exclusion, FIFO routes, bank voltage, audio clocks/device wiring,
 * memory coherence and externally supervised manual power-removal recovery.
 * Flags/digests identify externally admitted facts; they cannot establish them.
 * Absolute provider deadlines are derived from the live launch contract. */
struct i2s_session {
 uint64_t magic,version,bytes,epoch,generation,lease;
 uint8_t image[32],evidence[32],memory[32],recovery[32],pcm[32];
 uint32_t direction,audio_cold;
 struct a733_native_usb_config shared;
 struct a733_dma_handoff dma;
 struct a733_pin_config inherited[3];
#ifdef SUNSTONE_I2S_TRANSCRIBE
 _Alignas(8) uint32_t capture_samples;uint32_t reserved;
 struct i2s_models models;
#endif
};
#ifdef SUNSTONE_I2S_TRANSCRIBE
_Static_assert(sizeof(struct i2s_session)==640&&offsetof(struct i2s_session,capture_samples)==552&&
 offsetof(struct i2s_session,models)==560,"transcription wire geometry");
#endif
_Static_assert(sizeof(struct i2s_session)<=4096,"protected session page");
int i2s_session_valid(const struct i2s_session *);
#endif
