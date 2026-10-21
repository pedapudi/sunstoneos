#ifndef SUNSTONE_JPEG_INPUT_H
#define SUNSTONE_JPEG_INPUT_H
#include "a733_jpeg_application.h"
#include "a733_video_jpeg.h"
#include <stdatomic.h>
/* Capacities fit the 2 MiB uncached pool with every guard page: a 512 KiB
 * JPEG, a padded luma plane of at most 640x480 bytes (any shape whose
 * 32-aligned stride times rows fits) and half that for chroma. */
#define JPEG_INPUT_CAPACITY 524288u
#define JPEG_PLANE_CAPACITY 307200u
#define JPEG_OUTPUT_CAPACITY 460800u
/* Fixed decoder IOVAs inside one 4 MiB table window. Unmapped pages separate
 * the largest input, luma and chroma spans from each other. */
#define JPEG_INPUT_IOVA UINT32_C(0x10001000)
#define JPEG_LUMA_IOVA UINT32_C(0x10100000)
#define JPEG_CHROMA_IOVA UINT32_C(0x10180000)
#define JPEG_RESULT_HEADER 144u
#define JPEG_UPLOAD_MAGIC UINT32_C(0x5547504a)
/* Sequence sessions (version 3) decode up to eight images in one lifetime. */
#define JPEG_SEQUENCE_CAPACITY 8u
#define JPEG_SEQUENCE_MAGIC UINT32_C(0x5347504a)
#define JPEG_IMAGE_MAGIC UINT32_C(0x4947504a)
#define JPEG_MANIFEST_ENTRY 80u
#define JPEG_MANIFEST_BYTES(count) (16u+JPEG_MANIFEST_ENTRY*(count))
#define JPEG_SEQUENCE_RESULT_HEADER 160u
/* A version 2 session takes one upload: LE32 magic/version 1/JPEG bytes/oracle
 * bytes and LE64 epoch/generation, then the JPEG and optional planar oracle.
 * A version 3 session takes an upload with version 2 whose third word is the
 * manifest length, then the manifest: LE32 magic/version 1/count/zero and one
 * entry per image of LE32 JPEG bytes/oracle bytes/width/height and the JPEG
 * and oracle SHA-256 digests. Each image then follows as its own frame:
 * LE32 image magic/index/JPEG bytes/oracle bytes, LE64 epoch/generation, the
 * JPEG and the optional oracle. Image k is accepted only after the result of
 * image k-1 has been delivered.
 * The producer owns all fields except published images: it releases each by
 * storing its count in committed, and writes the next image only after the
 * consumer finished with the previous one. Published bytes are never changed
 * after failure. */
struct jpeg_input {
    uint8_t header[32],bytes[JPEG_INPUT_CAPACITY],oracle[JPEG_OUTPUT_CAPACITY];
    uint8_t manifest[JPEG_MANIFEST_BYTES(JPEG_SEQUENCE_CAPACITY)];
    uint8_t fixture[32],reference[32]; /* Session digests latched at the first byte. */
    uint8_t input_hash[32],oracle_hash[32]; /* Digests of the current image. */
    uint64_t epoch,generation,authority,deadline,last_now;
    size_t header_used,input_bytes,oracle_bytes,received,manifest_bytes,manifest_used;
    unsigned failed,sequence,count,delivered;
    uint32_t width,height,stride,rows,luma_bytes,chroma_bytes,output_bytes;
    uint32_t mapped[3]; /* Page-rounded input, luma and chroma capacities. */
    atomic_uint committed; /* Images published. */
};
size_t jpeg_input_remaining(const struct jpeg_input *);
/* Bytes still owed by a frame or manifest that has started; zero between
 * frames, where a single zero byte is a result query. */
size_t jpeg_input_partial(const struct jpeg_input *);
/* Number of published images; the latest is the one being decoded. */
unsigned jpeg_input_ready(const struct jpeg_input *);
int jpeg_input_feed(struct jpeg_input *,const struct a733_jpeg_session *,
    int (*clock)(void *,uint64_t *),void *,uint64_t,const uint8_t *,size_t);
int jpeg_input_geometry(struct jpeg_input *,const struct a733_video_jpeg *);
/* Page-rounded mapped bytes shared by every job: 0 input, 1 luma, 2 chroma.
 * A single image maps its own sizes; a sequence maps its largest entries. */
uint32_t jpeg_input_mapped(const struct jpeg_input *,unsigned buffer);
#endif
