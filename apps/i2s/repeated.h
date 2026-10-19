#ifndef SUNSTONE_I2S_REPEATED_H
#define SUNSTONE_I2S_REPEATED_H
#include <stdint.h>
#include <stdatomic.h>
#define I2S_REPEATED_SLOTS 4u
#define I2S_REPEATED_OVERRUN 1u
#define I2S_REPEATED_DEVICE_FAILED 2u
struct i2s_repeated_period {
 uint64_t ticket,first_frame;
 uint32_t frames,counter;
};
struct i2s_repeated_slot {
 struct i2s_repeated_period period;
 uint8_t bytes[4096];
};
/* One audio-task producer and one serialized USB-task consumer. Each copied
 * slot is immutable from head release until tail release after exact USB ACK.
 * Monotonic 64-bit sequences cannot wrap within an admitted session. */
struct i2s_repeated {
 struct i2s_repeated_slot slots[I2S_REPEATED_SLOTS];
 _Atomic uint64_t head,tail,rx_frames,tx_frames;
 atomic_uint drained,flags;
 unsigned stage,opened,direction,primed;
 uint64_t held[2];
 /* USB-task/callback-owned delivery proof; never written by audio producer. */
 uint64_t delivered_ticket;unsigned delivered_bytes;
};
int i2s_repeated_step(void);
int i2s_repeated_copy(unsigned,unsigned,void *,struct i2s_repeated_period *);
int i2s_repeated_ack(uint64_t);
int i2s_repeated_delivered(uint64_t,unsigned,unsigned);
#endif
