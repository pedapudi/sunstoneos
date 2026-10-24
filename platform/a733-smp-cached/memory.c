#include "a733_cached_memory.h"
#include "a733_cpu_coherence.h"
#include "a733_cpu_start.h"
#include "a733_device_memory_layout.h"
/* This builder maps image RAM, and so every local pool byte, with 2 MiB
 * level-2 block descriptors; the QEMU golden comparison in
 * checks/device-memory-capacity.c fixes them. The local pool check of
 * a733_memory_critical.h therefore makes one translation per 2 MiB block in
 * every image that links this file. */
const uintptr_t a733_local_pool_stride = (uintptr_t)2 << 20;
#if defined(A733_GPU_OUTPUT_EL0_READ_ONLY) && defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
#error "The read-only GPU output view rewrites local-layout entries; the extended layout has none"
#endif
#define TABLE __attribute__((section(".bss.a733_cached_tables"),aligned(4096)))
#define BIT(n) (UINT64_C(1) << (n))
uint64_t a733_cached_el3_l1[512] TABLE, a733_cached_el3_l2[512] TABLE;
uint64_t a733_cached_el3_l3[512] TABLE;
uint64_t a733_cached_el1_l1[512] TABLE, a733_cached_el1_l2[512] TABLE;
/* Word 0 holds the EL1 table sum; the rest keeps the line to itself. Code
 * names it last so that it follows the five pages (checks/cpu-table-walks.py). */
uint64_t a733_cached_el1_table_sum[8] __attribute__((section(".bss.a733_cached_tables"),aligned(64)));
extern unsigned char __tables_start[], __tables_end[], __bss_end[], __heap_end[];
extern unsigned char __device_pool_start[], __device_pool_end[];
extern unsigned char __uncached_pool_start[], __uncached_pool_end[];
/* DRAM above the boot image that EL1 maps when this boot's decode check
 * passes: level-2 blocks below 0x80000000, level-1 blocks from there. The same
 * table is the list the check receives and drives the entry writes. The
 * extended layout already maps 0x48000000-0x79000000 with its pools. */
static const struct a733_dram_range dram_map[] = {
#if !defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
    {UINT64_C(0x48000000), UINT64_C(0x79000000) - UINT64_C(0x48000000)},
#endif
    {UINT64_C(0x80000000), A733_DRAM_MAP_END - UINT64_C(0x80000000)},
};
#define DRAM_MAP_RANGES (unsigned)(sizeof dram_map / sizeof dram_map[0])
/* No image links drivers/dram/a733_dram_decode.c yet, so this default is the
 * one definition in every image: no decode evidence, so no entry above the
 * current map. The driver's definition, which has the same declaration in
 * a733_dram.h, replaces it in an image that links the driver. */
__attribute__((weak)) int a733_dram_decode_check(const struct a733_dram_range *ranges, unsigned count)
{
    (void)ranges; (void)count;
    return 0;
}

int a733_cached_memory_prepare(void)
{
    uint64_t el, sctlr3, sctlr1, daif, mpidr; unsigned core;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    if (el != 12) return 0;
    __asm__ volatile("mrs %0, sctlr_el3\n mrs %1, sctlr_el1\n mrs %2, daif\n mrs %3, mpidr_el1"
        : "=r"(sctlr3), "=r"(sctlr1), "=r"(daif), "=r"(mpidr));
    if (((sctlr3 | sctlr1) & UINT64_C(0x02001005)) || (daif & 0x3c0) != 0x3c0 ||
        !a733_cpu_index(mpidr, &core) || core) return 0;
    uintptr_t wb = (uintptr_t)__device_pool_start, we = (uintptr_t)__device_pool_end;
    uintptr_t nc = (uintptr_t)__uncached_pool_start, ne = (uintptr_t)__uncached_pool_end;
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
    struct a733_device_memory_layout layout;
    if (!a733_device_memory_tables_admitted(&layout) ||
        (uintptr_t)__bss_end > (uintptr_t)__heap_end || (uintptr_t)__heap_end > 0x42000000 ||
        wb != layout.wb_start || we != layout.wb_end || nc != layout.nc_start || ne != layout.nc_end) return 0;
#else
#if defined(SUNSTONE_NPU_LARGE_GRAPH)
    if (!a733_device_memory_local_pool_valid(wb, we)) return 0;
#endif
    if ((uintptr_t)__bss_end > (uintptr_t)__heap_end || (uintptr_t)__heap_end > wb || we < wb ||
        (we != wb && ((wb | we) & 4095 || we - wb > A733_LOCAL_DEVICE_POOL_BYTES || we > 0x41f00000)) ||
        (ne != nc && (nc != 0x41c00000 || ne != 0x41e00000 || we > nc))) return 0;
#endif
    /* With the MMU off, EL3 addresses the Secure space. Remove any Secure line
     * over the table section before the first descriptor write, so no later
     * eviction or publication writes stale data over a descriptor. */
    uint64_t now;
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(now));
    if (a733_cpu_coherence_flush((uintptr_t)__tables_start,
            (uintptr_t)__tables_end - (uintptr_t)__tables_start, now + 240000) != 1) return 0;
    if (a733_cpu_coherence_tables(a733_cached_el3_l1, a733_cached_el3_l2,
            (uintptr_t)a733_cached_el3_l2, 3) != 1 ||
        a733_cpu_coherence_tables(a733_cached_el1_l1, a733_cached_el1_l2,
            (uintptr_t)a733_cached_el1_l2, 1) != 1) return 0;
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
    /* Both regimes use identical memory attributes. New EL1 blocks deny EL0. */
    for (uintptr_t a = wb; a < ne; a += 0x200000) {
        unsigned i = (unsigned)((a - 0x40000000) >> 21);
        uint64_t common = a | BIT(54) | BIT(10) | (UINT64_C(3) << 8) | 1 |
            (a < we ? UINT64_C(4) : UINT64_C(8));
        a733_cached_el3_l2[i] = common | BIT(5) | BIT(6);
        a733_cached_el1_l2[i] = common | BIT(53);
    }
#else
    if (ne != nc) {
        /* Change both aliases before any cache adoption; preserve NS/AP/XN. */
        a733_cached_el3_l2[14] = (a733_cached_el3_l2[14] & ~UINT64_C(28)) | 8;
        a733_cached_el1_l2[14] = (a733_cached_el1_l2[14] & ~UINT64_C(28)) | 8;
    }
#if defined(A733_GPU_OUTPUT_EL0_READ_ONLY)
    /* GPU output region 0x46c00000-0x47000000: AP = 0b11, read-only at EL0
     * and EL1 as the text block is, UXN and PXN, still write-back inner
     * shareable. No EL1 or EL0 store can dirty a line of it. EL3 does not
     * map it; only the OS image's identity step stores to its tag area, with
     * translation off, before these tables take effect. */
    a733_cached_el1_l2[54] |= (UINT64_C(3) << 6) | BIT(53) | BIT(54);
    a733_cached_el1_l2[55] |= (UINT64_C(3) << 6) | BIT(53) | BIT(54);
#endif
#endif
    for (unsigned i = 0; i < 512; ++i) {
        uintptr_t a = 0x42000000 + (uintptr_t)i * 4096;
        uint64_t v = a | BIT(54) | BIT(10) | BIT(6) | 3;
        /* Whole table pages only: the checksum line after them keeps the
         * Device mapping of the remaining monitor pages. */
        if (a >= (uintptr_t)__tables_start && a + 4096 <= (uintptr_t)__tables_end) {
            v |= 8 | (UINT64_C(3) << 8);
            if (a == (uintptr_t)a733_cached_el1_l1 || a == (uintptr_t)a733_cached_el1_l2) v |= BIT(5);
        }
        a733_cached_el3_l3[i] = v;
    }
    a733_cached_el3_l2[16] = (uintptr_t)a733_cached_el3_l3 | 3;
    a733_cached_el1_l2[16] = 0; /* Secure monitor and tables remain inaccessible at EL1. */
    /* DRAM above the boot image, EL1 read/write, no EL0 access, never
     * executable. A mapped block receives speculative reads, so it is written
     * only when this boot's decode is linear there with no protected region.
     * EL3 entries stay invalid: EL3 does not access that range. */
    if (a733_dram_decode_check(dram_map, DRAM_MAP_RANGES) == 1)
        for (unsigned r = 0; r < DRAM_MAP_RANGES; ++r) {
            uint64_t base = dram_map[r].base, level1 = base >= UINT64_C(0x80000000);
            uint64_t step = level1 ? UINT64_C(1) << 30 : UINT64_C(1) << 21;
            uint64_t *entry = level1 ? &a733_cached_el1_l1[base >> 30] :
                &a733_cached_el1_l2[(base - UINT64_C(0x40000000)) >> 21];
            for (uint64_t v = A733_EL1_DRAM_BLOCK(base), end = v + dram_map[r].size; v < end; v += step)
                *entry++ = v;
        }
    /* Last table step: each core compares this with its own readback. */
    uint64_t sum = 0;
    for (unsigned i = 0; i < 512; ++i) sum += a733_cached_el1_l1[i] + a733_cached_el1_l2[i];
    a733_cached_el1_table_sum[0] = sum;
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
    return a733_device_memory_tables_complete();
#else
    return 1;
#endif
}
