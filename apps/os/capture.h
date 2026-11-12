#ifndef SUNSTONE_OS_CAPTURE_H
#define SUNSTONE_OS_CAPTURE_H
#include <stdint.h>
/* Application ABI declarations also proposed in sunstone_app.h. One application
 * task owns replay and ss_capture. The source is raw PCM in the manifest format;
 * offset skips any container header. */
int ss_capture_replay_open(uint32_t region,uint64_t offset,uint32_t frames,uint32_t paced);
/* Admission (os_large_prepare): take the capture buffer that the manifest declares from the ledger. 1 when taken or none is
 * declared, 0 when the ledger refuses it. */
int os_capture_memory(void);
#endif
