#ifndef SUNSTONE_BLUETOOTH_RECOGNITION_H
#define SUNSTONE_BLUETOOTH_RECOGNITION_H
#include <stddef.h>
#include <stdint.h>
#define BLUETOOTH_MODEL_BASE UINT64_C(0x44000000)
#define BLUETOOTH_MODEL_SLOT UINT64_C(0x800000)
#define BLUETOOTH_RECOGNITION_LOG 65536u
struct bluetooth_application;
struct bluetooth_recognition_profile {uint64_t bytes[2];uint8_t sha256[2][32];};
struct bluetooth_recognition_snapshot {
    uint64_t firmware,uart,acl,audio;
    uint32_t bytes,input_samples,output_samples,reserved;
    uint8_t pcm_sha256[32],wav_sha256[32],text_sha256[32];
};
#ifdef __cplusplus
extern "C" {
#endif
struct bluetooth_repeated_snapshot {
    struct bluetooth_recognition_snapshot result;
    uint64_t ticket,first_sample,first_acquired,last_acquired;
};
/* One serialized USB consumer; status/copy/ACK never touch HCI or raw buffers. */
int bluetooth_recognition_segment_copy(struct bluetooth_application *,uint64_t,uint32_t,size_t,
    uint8_t out[1024],size_t *,struct bluetooth_repeated_snapshot *);
int bluetooth_recognition_segment_ack(struct bluetooth_application *,uint64_t);
int bluetooth_recognition_finished(struct bluetooth_application *);
int bluetooth_playback_runtime_start(struct bluetooth_application *);
int bluetooth_recognition_start(struct bluetooth_application *,const struct bluetooth_recognition_profile *,uint64_t deadline);
int bluetooth_recognition_live(struct bluetooth_application *);
int bluetooth_recognition_output(struct bluetooth_application *,const void *,size_t);
void bluetooth_recognition_abort(void);
/* Only immutable final transcript bytes cross tasks. 0 pending,1 complete,
 * -1 retained failure. Failure never releases producer buffers or model bytes. */
int bluetooth_recognition_copy(struct bluetooth_application *,uint32_t offset,size_t bytes,uint8_t out[1024],
                               size_t *copied,struct bluetooth_recognition_snapshot *);
#ifdef __cplusplus
}
#endif
#endif
