#ifndef SUNSTONE_HDMI_AUDIO_STREAM_H
#define SUNSTONE_HDMI_AUDIO_STREAM_H
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include "dma_service.h"
#define HDMI_STREAM_PERIOD_BYTES 4096u
#define HDMI_STREAM_PERIOD_FRAMES 1024u
#define HDMI_STREAM_PERIODS 4u
#define HDMI_STREAM_PROTOCOL_VERSION 2u
enum hdmi_stream_operation { HDMI_STREAM_STATUS, HDMI_STREAM_WRITE,
    HDMI_STREAM_END, HDMI_STREAM_CANCEL };
/* Sequence is the zero-based next period index; END uses the period count.
 * STATUS/CANCEL use the number of periods already accepted by USB. */
struct hdmi_stream_request {
    uint32_t magic,version,request_id,bytes;
    uint64_t epoch,generation;
    uint32_t operation,payload_bytes;
    uint64_t sequence;
};
struct hdmi_stream_observation {
    uint32_t credits,command_result;
    uint64_t accepted_frames,committed_frames,consumed_frames,sequence;
};
_Static_assert(sizeof(struct hdmi_stream_request)==48,"stream request ABI");
_Static_assert(sizeof(struct hdmi_stream_observation)==40,"stream observation suffix ABI");
#define HDMI_STREAM_MANIFEST_MAGIC UINT64_C(0x4153373348534d31)
struct hdmi_stream_manifest {
    uint64_t magic;
    uint32_t version,bytes;
    uint64_t frames;
    uint32_t frame_bytes,period_frames;
    uint8_t pcm_sha256[32];
};
_Static_assert(sizeof(struct hdmi_stream_manifest)==64,"stream manifest header");
struct hdmi_stream {
    _Atomic uint64_t head,tail,committed,consumed;
    atomic_uint closed,eof,cancel,failed;
    _Alignas(64) uint8_t staging[4][4096];
    /* USB is the sole unpublished-slot writer. Output owns service state. */
    unsigned uploaded;
    uint64_t command_sequence;
    unsigned command,command_result;
    uint64_t held,request_ticket,request_until;
    unsigned stage,slot,primed,started,sealed,final_frames,request_op;
    unsigned stopping,stop_failed;
    uint64_t stop_until;
    struct dma_service_reply reply;
};
extern struct hdmi_stream stream;
size_t hdmi_stream_manifest_bytes(void);
int hdmi_stream_manifest_valid(void);
unsigned hdmi_stream_upload_bytes(uint64_t);
void *hdmi_stream_upload_buffer(uint64_t,unsigned,unsigned);
int hdmi_stream_uploaded(uint64_t,unsigned);
unsigned hdmi_stream_credits(void);
int hdmi_stream_end(uint64_t);
int hdmi_stream_cancel(void);
int hdmi_stream_step(void);
#endif
