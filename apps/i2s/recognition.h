#ifndef SUNSTONE_I2S_RECOGNITION_H
#define SUNSTONE_I2S_RECOGNITION_H
#include <stddef.h>
#include <stdint.h>
#define I2S_CAPTURE_MAX_SAMPLES 80000u
#define I2S_CAPTURE_MIN_SAMPLES 16000u
#define I2S_CAPTURE_MAX_BYTES (2u*I2S_CAPTURE_MAX_SAMPLES)
#define I2S_MODEL_BASE UINT64_C(0x44000000)
#define I2S_MODEL_SLOT UINT64_C(0x800000)
#define I2S_RECOGNITION_LOG 65536u
struct i2s_models {uint64_t bytes[2];uint8_t sha256[2][32];};
struct i2s_recognition_snapshot {
 uint32_t bytes,samples;uint8_t pcm_sha256[32],text_sha256[32];
};
#ifdef __cplusplus
extern "C" {
#endif
int i2s_models_valid(const struct i2s_models *);
int i2s_recognition_start(const struct i2s_models *);
void i2s_recognition_poll(void);
void i2s_recognition_abort(void);
int i2s_recognition_copy(unsigned,unsigned,void *,struct i2s_recognition_snapshot *);
/* C bridge retains DMA/application ownership; inference never polls hardware. */
int i2s_capture_valid(void);
unsigned i2s_capture_bytes(void);
int i2s_capture_copy(void *,unsigned);
int i2s_copy_target(const void *,size_t);
#ifdef __cplusplus
}
#endif
#endif
