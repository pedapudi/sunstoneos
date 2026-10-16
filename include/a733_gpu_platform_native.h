#ifndef SUNSTONE_A733_GPU_PLATFORM_NATIVE_H
#define SUNSTONE_A733_GPU_PLATFORM_NATIVE_H
#include "a733_gpu_platform.h"
#include "a733_gpu_resources_native.h"
struct a733_gpu_platform_native {
    struct a733_gpu_platform_ops operations;
    struct a733_gpu_resource_locks locks;
    unsigned initialized;
};
/* AArch64 EL1; zero storage first. Uses the same globally shared locks as the
 * native resource adapter. Init reads CNTFRQ only and requires 24 MHz. Callbacks
 * require identity Device mappings, qualified watchdog recovery, and a live
 * platform LOCAL lease. They are private to that platform, not raw user APIs. */
/* Counter callback for constructing shared locks before either native adapter.
 * Do not use it for device work until native init validates the counter rate. */
int a733_gpu_platform_native_now(void *, uint64_t *);
int a733_gpu_platform_native_init(struct a733_gpu_platform_native *,
                                 const struct a733_gpu_resource_locks *);
#endif
