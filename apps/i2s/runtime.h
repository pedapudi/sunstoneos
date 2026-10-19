#ifndef SUNSTONE_I2S_RUNTIME_H
#define SUNSTONE_I2S_RUNTIME_H
#include <stdint.h>
#include <stdatomic.h>
#define I2S_RUNTIME_DIRECTION 6u
#define I2S_RUNTIME_SLOTS 4u
#define I2S_RUNTIME_FRAMES 2048u /* mono S16:4096bytes; two32-bit wire slots */
struct i2s_runtime {
 _Atomic uint64_t head,tail,committed,consumed;
 atomic_uint closed,drained,flags,file_mode,eof,file_done,stopped;
 uint64_t file_frames;
 uint8_t bytes[I2S_RUNTIME_SLOTS][4096];
 /* Audio-task state; USB owns only the unpublished upload range. */
 uint64_t held,uploaded;
 unsigned stage,primed,slot,started,valid_frames,sealed;
};
int i2s_runtime_begin(uint64_t);
int i2s_runtime_end(uint64_t);
unsigned i2s_runtime_upload_bytes(uint64_t);
int i2s_runtime_upload(uint64_t,unsigned,const void *,unsigned);
void i2s_runtime_upload_discard(void);
unsigned i2s_runtime_credits(void);
int i2s_runtime_step(void);
#endif
