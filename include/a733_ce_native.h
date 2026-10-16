#ifndef SUNSTONE_A733_CE_NATIVE_H
#define SUNSTONE_A733_CE_NATIVE_H
#include "a733_ce.h"
/* Direct ordered MMIO only after actual security/mapping authority. Secure
 * access is read-only and limited to health/queue status; no keys or SID reads. */
int a733_ce_native_read(void *,uintptr_t,uint32_t *);
int a733_ce_native_write(void *,uintptr_t,uint32_t);
/* Requires admitted complete private 64-byte cache lines and CPU ownership for
 * PUBLISH; successful device completion before ACQUIRE. Verifies native cache
 * line size before maintaining the supplied ranges. No allocation/mapping. */
int a733_ce_native_sync(void *,struct a733_ce_memory *,enum a733_ce_sync);
#endif
