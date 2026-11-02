/* SPDX-License-Identifier: GPL-2.0-only */
#include "result.h"
/* Plain 128-byte loads, no software prefetch: the rate a streaming reader sees
 * from the hardware prefetchers. Eight vector accumulators keep the loads
 * independent; the XOR fold makes the result depend on every loaded word. */
__attribute__((noinline)) uint64_t dq_read(const uint64_t *p, size_t bytes)
{
#ifdef __aarch64__
    uint64_t lo, hi;
    const uint64_t *end = p + bytes / 8;
    __asm__ volatile(
        ".arch_extension simd\n"
        "movi v16.16b,#0\n movi v17.16b,#0\n movi v18.16b,#0\n movi v19.16b,#0\n"
        "1:\n"
        "ld1 {v0.16b-v3.16b},[%[p]],#64\n"
        "ld1 {v4.16b-v7.16b},[%[p]],#64\n"
        "eor v16.16b,v16.16b,v0.16b\n eor v17.16b,v17.16b,v1.16b\n"
        "eor v18.16b,v18.16b,v2.16b\n eor v19.16b,v19.16b,v3.16b\n"
        "eor v16.16b,v16.16b,v4.16b\n eor v17.16b,v17.16b,v5.16b\n"
        "eor v18.16b,v18.16b,v6.16b\n eor v19.16b,v19.16b,v7.16b\n"
        "cmp %[p],%[end]\n b.lo 1b\n"
        "eor v16.16b,v16.16b,v17.16b\n eor v18.16b,v18.16b,v19.16b\n eor v16.16b,v16.16b,v18.16b\n"
        "mov %[lo],v16.d[0]\n mov %[hi],v16.d[1]\n"
        : [p] "+r"(p), [lo] "=r"(lo), [hi] "=r"(hi)
        : [end] "r"(end)
        : "v0","v1","v2","v3","v4","v5","v6","v7","v16","v17","v18","v19","cc","memory");
    return lo ^ hi;
#else
    uint64_t x = 0;
    for (size_t i = 0; i < bytes / 8; ++i) x ^= p[i];
    return x;
#endif
}
