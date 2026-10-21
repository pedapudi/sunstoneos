#ifndef SUNSTONE_HEVC_INPUT_H
#define SUNSTONE_HEVC_INPUT_H
#include "a733_hevc_application.h"
#include "a733_video_hevc.h"
#include <stdatomic.h>
/* One upload per lifetime. Header 32 bytes, little endian: magic U625,
 * version 1, clip bytes, picture count 2, session epoch64, generation64. The
 * clip follows. Every fragment is bound to the same admitted session and
 * deadline; completion hashes and parses the owned bytes before publication.
 * The upload buffer is the storage's write-back extent and is never reused. */
#define HEVC_UPLOAD_MAGIC UINT32_C(0x55363235)
#define HEVC_RESULT_MAGIC UINT32_C(0x44363235)
#define HEVC_RESULT_HEADER 160u
struct hevc_input {
 uint8_t header[32],hash[32];uint8_t *bytes;void *workspace;
 size_t capacity,workspace_bytes,header_used,received,total;
 uint64_t epoch,generation,authority,deadline,last_now,ranges[4];
 struct a733_hevc_plan plan;unsigned failed;atomic_uint committed;
};
int hevc_input_bind(struct hevc_input *,uint8_t *,size_t,void *,size_t);
int hevc_input_ready(const struct hevc_input *);
size_t hevc_input_remaining(const struct hevc_input *);
int hevc_input_feed(struct hevc_input *,const struct a733_hevc_session *,
 int (*)(void *,uint64_t *),void *,uint64_t,const uint8_t *,size_t);
#endif
