#ifndef SUNSTONE_A733_MEMORY_LOCAL_H
#define SUNSTONE_A733_MEMORY_LOCAL_H
/* The boot preparation of both local-layout pools: the write-back pool of
 * a733_memory_reservations.h and the non-cacheable pool of
 * a733_memory_uncached.h. Call it once on core 0 at EL1, after the identity
 * tables are installed and the MMU and data cache are on, and before the
 * scheduler starts: platform_init() of platform/a733/platform.c and the core-0
 * path of platform/a733-smp-cached/continue.c do. It returns 1 when both pools
 * are ready or absent and 0 after a refusal, which the caller treats as a
 * boot failure. Extended-layout images do not call it; there
 * a733_device_memory_prepare() prepares both pools.
 *
 * Only the boot file that calls it includes this header. Its weak
 * definitions stand for a pool source the image does not link and report an
 * absent pool; the strong definitions in platform/a733/memory_reservations.c
 * and memory_uncached.c replace them. A weak definition, unlike a weak
 * reference, needs no GOT entry in images whose linker scripts place none. */
__attribute__((weak)) int a733_memory_reservations_prepare(void) { return 1; }
__attribute__((weak)) int a733_memory_uncached_prepare(void) { return 1; }
static inline int a733_memory_local_prepare(void)
{
    int ok = a733_memory_reservations_prepare();
    return a733_memory_uncached_prepare() && ok;
}
#endif
