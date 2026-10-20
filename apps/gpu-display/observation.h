#ifndef SUNSTONE_GPU_DISPLAY_OBSERVATION_H
#define SUNSTONE_GPU_DISPLAY_OBSERVATION_H
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#define GPU_DISPLAY_OBSERVE_MAGIC UINT32_C(0x4f444747)
#define GPU_DISPLAY_OBSERVE_LIMIT 200u
struct gpu_display_sample {uint32_t rgba;uint16_t rgb565,reserved;};
struct gpu_display_frame_evidence {
    uint32_t sequence,rgba_bytes,flags,reserved0;
    uint8_t input_sha256[32],rgba_sha256[32],rgb565_sha256[32];
    struct gpu_display_sample samples[16];
    uint8_t reserved[16];
};
struct gpu_display_presentation {
    uint32_t slot,sequence;
    uint64_t rcq_done_us,frame_end_us,verified_source_address;
    uint32_t raw_status,flags;
    uint64_t observed_us,reserved[2];
};
struct gpu_display_observation {
    uint32_t magic,version,bytes,request_id;
    uint64_t epoch,generation;
    uint32_t state,flags,frames,presentations;
    uint64_t now_us,remaining_us;
    struct gpu_display_frame_evidence frame[2];
    struct gpu_display_presentation presentation[3];
    struct {
        uint32_t gpu_closed,hdmi_state,de_state,pmic_parked;
        uint32_t fatal,usb_ready,display_stage,flags;
        uint64_t stop_at,deadline;
        uint64_t lease;
        uint8_t build_id[32],session_sha256[32],reserved[136];
    } terminal;
};
struct gpu_display_observation_mailbox {
    /* USB posts1; display publishes2; USB owns3 through terminal TX/ZLP.
     * Only the USB owner returns the slot to0. No live owner state is lent. */
    atomic_uint state;
    uint32_t request_id,requests;
    uint64_t request_deadline;
    struct gpu_display_observation reply;
};
_Static_assert(sizeof(struct gpu_display_frame_evidence)==256,"frame observation ABI");
_Static_assert(sizeof(struct gpu_display_presentation)==64,"presentation observation ABI");
_Static_assert(sizeof(struct gpu_display_observation)==1024,"copied observation ABI");
int gpu_display_observation_request(const void *,size_t,uint64_t);
void gpu_display_observation_poll(void);
int gpu_display_evidence_begin(unsigned,const uint8_t[32]);
int gpu_display_evidence_chunk(unsigned,uint32_t,const void *,const void *,uint32_t);
int gpu_display_evidence_end(unsigned);
#endif
