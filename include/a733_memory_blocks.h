#ifndef SUNSTONE_A733_MEMORY_BLOCKS_H
#define SUNSTONE_A733_MEMORY_BLOCKS_H
#include "a733_mapping_check.h"

#define A733_MEMORY_BLOCK UINT64_C(0x200000)

/* One translation per 2 MiB block that holds a byte of the identity range
 * [start, end): at start, then at the base of each following block. Every pool
 * range lies in level-2 block descriptors, which the QEMU golden comparison of
 * the EL1 tables fixes, and one translation of any address in a block
 * descriptor shows the attributes, permissions and output address of the whole
 * block. Refuses an empty or wrapping range. */
static inline int a733_memory_check_blocks(a733_translate_fn translate, uintptr_t start, uintptr_t end,
                                           const struct a733_mapping_rule *rule)
{
    if (start >= end) return 0;
    for (uintptr_t a = start; a < end; a = (a & ~(uintptr_t)(A733_MEMORY_BLOCK - 1)) + A733_MEMORY_BLOCK) {
        if (!a733_mapping_check(translate, a, a, rule)) return 0;
        if (a > UINTPTR_MAX - A733_MEMORY_BLOCK) return 1;
    }
    return 1;
}
#endif
