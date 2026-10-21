#ifndef SUNSTONE_A733_CE_NATIVE_H
#define SUNSTONE_A733_CE_NATIVE_H
#include "a733_ce.h"
/* Direct ordered MMIO only after actual security/mapping authority. Secure
 * access is limited to status/health; health writes require both tests disabled
 * and authenticated cold provisioning. No direct key-register, SID or key-ladder
 * access. Cipher keys are read from the admitted private DMA reservation. */
int a733_ce_native_read(void *,uintptr_t,uint32_t *);
int a733_ce_native_write(void *,uintptr_t,uint32_t);
/* Requires admitted complete private 64-byte cache lines and exclusive controller
 * CPU access before PUBLISH; successful device completion before ACQUIRE.
 * Maintenance uses aarch64_device_cache, which refuses an unknown or
 * larger-than-64-byte writeback granule, a non-EL1 caller and a disabled MMU
 * or data cache. KEY_CLEAN requires a consumed successful AES task and cleans
 * the zeroed input, cipher output and key lines; it does not establish
 * internal key-register erasure. SCRUB cleans output, IV and input after a SHA
 * task; RANDOM_SCRUB cleans output after a TRNG task. No allocation/mapping. */
int a733_ce_native_sync(void *,struct a733_ce_memory *,enum a733_ce_sync);
#endif
