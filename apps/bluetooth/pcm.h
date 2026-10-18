#ifndef SUNSTONE_BLUETOOTH_PCM_H
#define SUNSTONE_BLUETOOTH_PCM_H
#include <stddef.h>
#include <stdint.h>
#define BLUETOOTH_WAV_BYTES 96044u
/* Finite mono S16LE8k→16k. Each input emits itself and the adjacent average,
 * widened to int32 and divided toward zero; the last value is held. This is
 * linear interpolation, with its limited image rejection and narrowband input.
 * Exact output is44+4*samples bytes, no heap or channel/rate guessing. */
int bluetooth_pcm_wav(const uint8_t *,size_t bytes,uint8_t *,size_t capacity,size_t *written);
#endif
