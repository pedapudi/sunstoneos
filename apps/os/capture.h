#ifndef SUNSTONE_OS_CAPTURE_H
#define SUNSTONE_OS_CAPTURE_H
#include <stdint.h>
/* Application ABI declarations also proposed in sunstone_app.h. One application
 * task owns replay and ss_capture. The source is raw PCM in the manifest format;
 * offset skips any container header. The image reserves the capture buffer. */
int ss_capture_replay_open(uint32_t region,uint64_t offset,uint32_t frames,uint32_t paced);
#endif
