#ifndef SUNSTONE_HDMI_AUDIO_SESSION_H
#define SUNSTONE_HDMI_AUDIO_SESSION_H
#include "a733_dma_startup.h"
#define HDMI_AUDIO_PCM_ADDRESS UINT64_C(0x41f10000)
#define HDMI_AUDIO_MAX_FRAMES UINT64_C(48000)
#define HDMI_AUDIO_RATE 48000u
#define HDMI_AUDIO_CHANNELS 2u
#define HDMI_AUDIO_FRAME_BYTES 4u
/* Authenticated immutable suffix after the 440-byte pattern HDMI session.
 * PCM is stereo signed16-bit little endian, interleaved left/right. Its exact
 * frames*4 bytes are supplied separately at PCM_ADDRESS, not in the session.
 * The private boot copy supplies the DMA provider's actual absolute deadline.
 * memory/electrical name reviewed records; neither IDs nor flags establish
 * DDR exclusion, quiet producers, clock readiness or physical sink operation. */
struct hdmi_audio_session {
    uint8_t pcm_sha256[32],memory[32],electrical[32];
    uint64_t authority,frames;
    struct a733_dma_handoff dma;
};
_Static_assert(sizeof(struct hdmi_audio_session)==216&&
    offsetof(struct hdmi_audio_session,dma)==112,"HDMI audio suffix ABI");
#define HDMI_AUDIO_RESULT_MAGIC UINT64_C(0x4153373348415231)
enum hdmi_audio_result_stage { HDMI_AUDIO_PENDING=0,HDMI_AUDIO_RUNNING=1,
    HDMI_AUDIO_INPUT_ENDED_RETAINED=2,HDMI_AUDIO_FAULT_RETAINED=3 };
/* Copied diagnostic observations. Source consumption/DMA drain/FIFO empty are
 * not serializer completion, transmitted packets or rendered sound. */
struct hdmi_audio_result {
    uint64_t magic,version,bytes,stage;
    uint64_t frames,source_bytes,source_consumed_bytes,dma_result;
    uint64_t fifo_empty,tmds_hz,rate_hz,wire_drained;
    uint8_t pcm_sha256[32],session_sha256[32];
};
_Static_assert(sizeof(struct hdmi_audio_result)==160,"HDMI audio result ABI");
#endif
