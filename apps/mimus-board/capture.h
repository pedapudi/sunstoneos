#ifndef SUNSTONE_MIMUS_BOARD_CAPTURE_H
#define SUNSTONE_MIMUS_BOARD_CAPTURE_H
/* The mimus recognizer's microphone input: one finite 16 kHz mono 16-bit
 * capture of 1 to 5 s per recognition generation, framed as a WAV in place
 * over the OS capture stream (apps/os/audio-stream.h). The stream delivers
 * whole 4,096-byte periods into the WAV's data region, so the adapter copies
 * nothing; it checks that the periods arrive contiguous and in place. */
#include "../os/audio-stream.h"

#define MIMUS_CAPTURE_RATE_HZ 16000u
#define MIMUS_CAPTURE_MIN_SAMPLES 16000u
#define MIMUS_CAPTURE_MAX_SAMPLES 80000u
#define MIMUS_CAPTURE_PERIOD_BYTES 4096u
/* The longest capture rounded up to whole periods: 40 ring slots. */
#define MIMUS_CAPTURE_PERIODS \
    ((2u*MIMUS_CAPTURE_MAX_SAMPLES+MIMUS_CAPTURE_PERIOD_BYTES-1u)/MIMUS_CAPTURE_PERIOD_BYTES)

/* The recognizer's capture length, in samples: 1 to 5 s. */
static inline int mimus_capture_admit(uint32_t samples)
{return samples>=MIMUS_CAPTURE_MIN_SAMPLES&&samples<=MIMUS_CAPTURE_MAX_SAMPLES;}
/* Once, before the scheduler: the stream configuration that captures
 * `samples` per generation; NULL when refused or already configured. */
const struct os_audio_stream_config *mimus_capture_configure(uint32_t samples);
/* EL0-safe; the recognizer task only. Requests the capture for recognition
 * generation g, which also releases the buffer of every earlier generation.
 * Returns 1 with the complete 16 kHz mono WAV of generation g, 0 while it is
 * being captured and -1 once the capture or the stream has failed. */
int mimus_capture(unsigned g,const uint8_t **wav,size_t *bytes);
#endif
