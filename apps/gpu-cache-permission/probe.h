#ifndef GPU_CACHE_PERMISSION_PROBE_H
#define GPU_CACHE_PERMISSION_PROBE_H
#include <stdint.h>
#define OUTPUT_BASE UINT64_C(0x41800000)
#define OUTPUT_BYTES UINT64_C(0x400000)
#define TAG_BYTES UINT64_C(0x10000)
#define OUTPUT_ATTR UINT64_C(0x00600000000007c5)
#define PATTERN UINT64_C(0x733c1a55cafe0000)
#define COMPLETE "COMPLETE GPU output cache permission: EL1 RO, zero tags, CIVAC 4MiB, bytes preserved\n"
static inline uintptr_t sample_address(unsigned i)
{ return i == 0 ? OUTPUT_BASE + TAG_BYTES : i == 1 ? OUTPUT_BASE + 0x200000 : OUTPUT_BASE + OUTPUT_BYTES - 64; }
#endif
