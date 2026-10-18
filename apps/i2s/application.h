#ifndef SUNSTONE_I2S_APPLICATION_H
#define SUNSTONE_I2S_APPLICATION_H
#include "session.h"
#include "boot.h"
#include "a733_audio_leases.h"
struct i2s_application {
 struct i2s_session admitted,session;
 struct a733_native_usb shared;
 struct a733_dma_pool_session dma;
 struct a733_audio_leases leases;
 struct a733_audio_parent parent;
 struct a733_i2s_platform binding;
 struct dma_service_i2s_stream_profile stream;
 StaticTask_t tasks[3];StackType_t stacks[3][2048];TaskHandle_t handles[3];
 atomic_uint fatal,result,cancel,audio_ready;
 uint64_t deadline,pending,held_period;
 void *period_memory[I2S_PERIODS];
 struct dma_service_reply reply;
 unsigned stage,periods,primed,stopping;
 uint8_t output[I2S_PCM_BYTES];
};
extern struct i2s_application app;
extern const uint8_t i2s_recorded_pcm[I2S_PCM_BYTES],i2s_recorded_pcm_digest[32];
int i2s_live(void *);
void i2s_fail(void *);
void i2s_usb0_task(void *);
void i2s_client_task(void *);
int i2s_client_step(void);
int i2s_prepare_audio(void);
int i2s_output(unsigned,void *,unsigned);
#endif
