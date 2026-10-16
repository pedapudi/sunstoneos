#ifndef SUNSTONE_AARCH64_DEVICE_MEMORY_H
#define SUNSTONE_AARCH64_DEVICE_MEMORY_H
#include <stddef.h>
#include <stdint.h>

struct aarch64_translation {
    uint64_t physical;
    unsigned attributes, shareability;
};
enum aarch64_cache_op {
    AARCH64_CACHE_CLEAN, AARCH64_CACHE_CLEAN_INVALIDATE, AARCH64_CACHE_INVALIDATE
};
/* CTR must justify exclusive ownership in 64-byte units. Unknown CWG is
 * insufficient. DminLine supplies the maintenance stride, not the lease size. */
static inline unsigned aarch64_cache_stride_from_ctr(uint64_t ctr)
{
    unsigned line = (unsigned)(ctr >> 16) & 15, granule = (unsigned)(ctr >> 24) & 15;
    if (!granule || granule > 4 || line > granule) return 0;
    return 4u << line;
}
/* EL1 only, stage-1 translation, no stage-2 remapping. Cache operations require
 * enabled MMU/data cache, pinned mappings and caller-owned 64-byte allocations.
 * They do not establish allocation ownership or validate physical RAM ranges.
 * AT preserves PAR_EL1 and DAIF. Cache maintenance completes to PoC with DSB SY.
 * Return zero on an invalid request; synchronous faults require platform
 * recovery. None of these operations can recover a stalled interconnect. */
unsigned aarch64_device_cache_stride(void);
int aarch64_device_translate(uintptr_t, int write, struct aarch64_translation *);
int aarch64_device_cache(enum aarch64_cache_op, uintptr_t, size_t);
#endif
