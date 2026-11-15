#ifndef SUNSTONE_VIDEO_DECODE_H
#define SUNSTONE_VIDEO_DECODE_H
/* Video decoder class (the VE decoder: JPEG, H.264 and HEVC). An application declares the video-decode device in its
 * manifest.json ("devices": {"required": ["video-decode"]}); the session grants it with SS_DEVICE_VIDEO_DECODE. One OS
 * owner task does every register access (apps/os/video_decode.c, docs/video-decode-os.md). Every call returns at once:
 * 1 done, 0 not yet (repeat the identical call after ss_video_decode_wait), -1 refused or failed. One request is in
 * flight.
 *
 * Open powers PCK600 domain 2 through the shared power-domain owner, cold-starts the decoder with PLL_VE0 at 624 MHz and
 * reads its identity; a decode then runs one baseline JPEG picture; close quiesces the decoder again. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Open: power, cold start and identity. */
int ss_video_decode_open(void);
/* Close: the quiesce (module gate, bus gate, reset, then PLL_VE0 disabled; domain 2 stays ON). The decoder then stays
 * closed for the rest of the board's power lifetime: a later open is refused. After an open that failed past its first
 * write, close runs the same steps once and returns -1 with the fault kept (ss_video_decode_fault); the session can
 * then end normally. */
int ss_video_decode_close(void);
/* One baseline JPEG picture (8-bit YCbCr 4:2:0, one interleaved scan, at most 1920 by 1080), read from input_bytes
 * bytes at input_offset of the application's large region input_region, decoded into packed active YU12 (the width by
 * height Y plane, then the Cb plane and the Cr plane, each (width+1)/2 by (height+1)/2) written at output_offset of
 * the writable large region output_region, which must hold output_bytes >= the active bytes. The two extents must not
 * overlap. One decode per open. The owner copies the input into its own buffer before the decoder reads it, and
 * writes the output only after the decoder's completion interrupt, its drain and its fault scan. */
struct ss_video_decode_jpeg {
    uint32_t input_region, output_region;
    uint64_t input_offset, output_offset;
    uint32_t input_bytes, output_bytes;
};
/* The answer to a decode: the decoded size, the active bytes written, the SHA-256 of those bytes, the decoder's status
 * word at completion and the system counter (24 MHz) at the trigger, at the completion interrupt and when the output
 * was written. */
#define SS_VIDEO_DECODE_DONE_MAGIC UINT32_C(0x454e4456) /* "VDNE" */
struct ss_video_decode_done {
    uint32_t magic, bytes, width, height, output_bytes, status;
    uint8_t sha256[32];
    uint64_t trigger_ticks, irq_ticks, done_ticks;
};
int ss_video_decode_jpeg(const struct ss_video_decode_jpeg *job);
/* Copy the answer of the latest decode into out: 1 copied, 0 none yet, -1 not granted. */
int ss_video_decode_done(struct ss_video_decode_done *out);
/* 1 with the step and value of the check that refused or failed, 0 when none, -1 when the device is not granted. */
int ss_video_decode_fault(uint32_t *step, uint32_t *value);
/* Block until the request in flight is answered or wait_us elapses. */
void ss_video_decode_wait(uint32_t wait_us);
#ifdef __cplusplus
}
#endif
#endif
