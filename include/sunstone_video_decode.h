#ifndef SUNSTONE_VIDEO_DECODE_H
#define SUNSTONE_VIDEO_DECODE_H
/* Video decoder class (the VE decoder: JPEG, H.264 and HEVC). An application declares the video-decode device in its
 * manifest.json ("devices": {"required": ["video-decode"]}); the session grants it with SS_DEVICE_VIDEO_DECODE. One OS
 * owner task does every register access (apps/os/video_decode.c, docs/video-decode-os.md). Every call returns at once:
 * 1 done, 0 not yet (repeat the identical call after ss_video_decode_wait), -1 refused or failed. One request is in
 * flight.
 *
 * The service offers open and close only. Open powers PCK600 domain 2 through the shared power-domain owner, cold-starts
 * the decoder with PLL_VE0 at 624 MHz and reads its identity; close quiesces it again. No decode runs. */
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
/* 1 with the step and value of the check that refused or failed, 0 when none, -1 when the device is not granted. */
int ss_video_decode_fault(uint32_t *step, uint32_t *value);
/* Block until the request in flight is answered or wait_us elapses. */
void ss_video_decode_wait(uint32_t wait_us);
#ifdef __cplusplus
}
#endif
#endif
