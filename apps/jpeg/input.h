#ifndef SUNSTONE_JPEG_INPUT_H
#define SUNSTONE_JPEG_INPUT_H
#include "a733_jpeg_application.h"
#include "a733_video_jpeg.h"
#include <stdatomic.h>
#define JPEG_INPUT_CAPACITY 4096u
#define JPEG_PLANE_CAPACITY 4096u
#define JPEG_OUTPUT_CAPACITY 6144u
#define JPEG_RESULT_HEADER 144u
#define JPEG_UPLOAD_MAGIC UINT32_C(0x5547504a)
/* One retained job. Header LE32 magic/version/JPEG bytes/oracle bytes followed
 * by LE64 epoch/generation; body is JPEG then optional planar pixel oracle.
 * Producer owns all fields until committed release; consumer acquires once.
 * Published bytes are never changed, reset or reused, including after failure. */
struct jpeg_input {
    uint8_t header[32],bytes[JPEG_INPUT_CAPACITY],oracle[JPEG_OUTPUT_CAPACITY];
    uint8_t input_hash[32],oracle_hash[32];
    uint64_t epoch,generation,authority,deadline,last_now;
    size_t header_used,input_bytes,oracle_bytes,received;
    unsigned failed;
    uint32_t width,height,stride,rows,luma_bytes,chroma_bytes,output_bytes;
    atomic_uint committed;
};
size_t jpeg_input_remaining(const struct jpeg_input *);
int jpeg_input_ready(const struct jpeg_input *);
int jpeg_input_feed(struct jpeg_input *,const struct a733_jpeg_session *,
    int (*clock)(void *,uint64_t *),void *,uint64_t,const uint8_t *,size_t);
int jpeg_input_geometry(struct jpeg_input *,const struct a733_video_jpeg *);
#endif
