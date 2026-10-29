#ifndef SUNSTONE_HDMI_AUDIO_OBSERVATION_H
#define SUNSTONE_HDMI_AUDIO_OBSERVATION_H
#include <stdatomic.h>
#include <stddef.h>
#include "session.h"
#define HDMI_AUDIO_REQUEST_MAGIC UINT32_C(0x4f414748)
#define HDMI_AUDIO_OBSERVE_MAGIC UINT32_C(0x52414748)
#define HDMI_AUDIO_OBSERVE_LIMIT 200u
struct hdmi_audio_observation {
    uint32_t magic,version,bytes,request_id;
    uint64_t epoch,generation,lease;
    uint32_t state,flags;
    uint64_t now_us,remaining_us;
    uint8_t build_id[32],session_sha256[32];
    struct hdmi_audio_result result;
    uint8_t reserved[224];
};
struct hdmi_audio_observation_mailbox {
    /* USB posts1; output owner publishes2; USB owns3 through TX+terminalZLP.
     * Only USB returns the slot to0. Audio buffers are never released. */
    atomic_uint state;
    uint32_t request_id,requests;
    uint64_t request_deadline;
#if SUNSTONE_HDMI_AUDIO_STREAM
    uint64_t sequence;
    unsigned operation,payload_bytes;
#endif
    struct hdmi_audio_observation reply;
};
_Static_assert(sizeof(struct hdmi_audio_observation)==512&&
    offsetof(struct hdmi_audio_observation,result)==128&&
    offsetof(struct hdmi_audio_observation,session_sha256)==96,"audio copied reply ABI");
extern struct hdmi_audio_observation_mailbox observation;
int hdmi_audio_observation_request(const void *,size_t,uint64_t);
void hdmi_audio_observation_poll(void);
#if SUNSTONE_HDMI_AUDIO_STREAM
int hdmi_audio_observation_uploaded(void);
#endif
#endif
