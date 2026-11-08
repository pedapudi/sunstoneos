#include <stddef.h>
#include <stdint.h>

/* memcpy, memmove and memset for code that runs at EL1 or EL0 under Normal memory with SCTLR_EL1.A clear, where a Cortex-A55 or
 * A76 performs an unaligned access in hardware. Source and destination need not share an alignment: the copy stores whole
 * aligned 16-byte pairs (LDP and STP of general registers, 32 bytes a turn) and loads unaligned. The strict routines in
 * memory.c and memmove.c copy a byte at a time when the two alignments differ. Only general registers are used, so the routines
 * run in a unit built with -mgeneral-regs-only too. Do not call these from the EL3 monitor, with the MMU off, or on Device memory:
 * an unaligned access there faults. The units that may call them are the relaxed list of cmake/codegen-align.cmake, which
 * force-includes include/sunstone_relaxed_memory.h to rename the calls. */
#define RELAXED __attribute__((target("+nofp")))
#define LOAD64(p, v) __builtin_memcpy(&(v), (p), 8)
#define STORE64(p, v) __builtin_memcpy((p), &(v), 8)
#define LOAD32(p, v) __builtin_memcpy(&(v), (p), 4)
#define STORE32(p, v) __builtin_memcpy((p), &(v), 4)
#define LOAD16(p, v) __builtin_memcpy(&(v), (p), 2)
#define STORE16(p, v) __builtin_memcpy((p), &(v), 2)

/* Up to 32 bytes: every load happens before any store, so overlap in either direction is safe. */
RELAXED static inline void small(unsigned char *d, const unsigned char *s, size_t n)
{
    if (n >= 16) {
        uint64_t a, b, c, e;
        LOAD64(s, a); LOAD64(s + 8, b); LOAD64(s + n - 16, c); LOAD64(s + n - 8, e);
        STORE64(d, a); STORE64(d + 8, b); STORE64(d + n - 16, c); STORE64(d + n - 8, e);
    } else if (n >= 8) {
        uint64_t a, b;
        LOAD64(s, a); LOAD64(s + n - 8, b);
        STORE64(d, a); STORE64(d + n - 8, b);
    } else if (n >= 4) {
        uint32_t a, b;
        LOAD32(s, a); LOAD32(s + n - 4, b);
        STORE32(d, a); STORE32(d + n - 4, b);
    } else if (n >= 2) {
        uint16_t a, b;
        LOAD16(s, a); LOAD16(s + n - 2, b);
        STORE16(d, a); STORE16(d + n - 2, b);
    } else if (n) {
        *d = *s;
    }
}

RELAXED void *ss_memmove_unaligned(void *dest, const void *src, size_t count)
{
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (count <= 32) { small(d, s, count); return dest; }
    if (count <= 64) {
        /* 33 to 64 bytes: the first and the last 32 bytes, all loaded before any is stored, overlap in the middle. */
        uint64_t a0, a1, a2, a3, b0, b1, b2, b3;
        LOAD64(s, a0); LOAD64(s + 8, a1); LOAD64(s + 16, a2); LOAD64(s + 24, a3);
        LOAD64(s + count - 32, b0); LOAD64(s + count - 24, b1); LOAD64(s + count - 16, b2); LOAD64(s + count - 8, b3);
        STORE64(d, a0); STORE64(d + 8, a1); STORE64(d + 16, a2); STORE64(d + 24, a3);
        STORE64(d + count - 32, b0); STORE64(d + count - 24, b1); STORE64(d + count - 16, b2); STORE64(d + count - 8, b3);
        return dest;
    }
    if ((uintptr_t)d <= (uintptr_t)s || (uintptr_t)d >= (uintptr_t)s + count) {
        /* Forward, and every store at a higher address than the one before it, none twice: a Cortex-A55 recognizes a stream of
         * stores that fill whole cache lines and then stops fetching the lines it is about to overwrite (write streaming). A
         * copy that stored its head and tail again, out of order, lost that: 176 rows of 382 bytes written into lines that were
         * not in the cache took 216 ns a row that way and 132 ns in address order (apps/memcpy-bench). Loads may be unaligned
         * and are always read before the store that could overwrite them (the destination is not above the source). */
        size_t skew = (size_t)(-(uintptr_t)d & 15);       /* bytes to the next 16-byte boundary of the destination */
        count -= skew;
        if (skew & 1) { *d++ = *s++; }
        if (skew & 2) { uint16_t v; LOAD16(s, v); STORE16(d, v); d += 2; s += 2; }
        if (skew & 4) { uint32_t v; LOAD32(s, v); STORE32(d, v); d += 4; s += 4; }
        if (skew & 8) { uint64_t v; LOAD64(s, v); STORE64(d, v); d += 8; s += 8; }
        /* 64 bytes a turn, the loads before the stores and the stores in order (an unrolled loop would interleave the stores of
         * two turns). */
        _Pragma("clang loop unroll(disable) interleave(disable)")
        for (; count >= 64; count -= 64, d += 64, s += 64) {
            uint64_t a0, a1, a2, a3, a4, a5, a6, a7;
            LOAD64(s, a0); LOAD64(s + 8, a1); LOAD64(s + 16, a2); LOAD64(s + 24, a3);
            LOAD64(s + 32, a4); LOAD64(s + 40, a5); LOAD64(s + 48, a6); LOAD64(s + 56, a7);
            STORE64(d, a0); STORE64(d + 8, a1); STORE64(d + 16, a2); STORE64(d + 24, a3);
            STORE64(d + 32, a4); STORE64(d + 40, a5); STORE64(d + 48, a6); STORE64(d + 56, a7);
        }
        if (count & 32) {
            uint64_t a, b, c, e;
            LOAD64(s, a); LOAD64(s + 8, b); LOAD64(s + 16, c); LOAD64(s + 24, e);
            STORE64(d, a); STORE64(d + 8, b); STORE64(d + 16, c); STORE64(d + 24, e);
            d += 32; s += 32;
        }
        if (count & 16) { uint64_t a, b; LOAD64(s, a); LOAD64(s + 8, b); STORE64(d, a); STORE64(d + 8, b); d += 16; s += 16; }
        if (count & 8) { uint64_t v; LOAD64(s, v); STORE64(d, v); d += 8; s += 8; }
        if (count & 4) { uint32_t v; LOAD32(s, v); STORE32(d, v); d += 4; s += 4; }
        if (count & 2) { uint16_t v; LOAD16(s, v); STORE16(d, v); d += 2; s += 2; }
        if (count & 1) { *d = *s; }
    } else {
        /* Backward, for a destination above an overlapping source. Both ends are read first and written last. */
        uint64_t t0, t1, t2, t3, h0, h1;
        LOAD64(s, t0); LOAD64(s + 8, t1); LOAD64(s + 16, t2); LOAD64(s + 24, t3);
        LOAD64(s + count - 16, h0); LOAD64(s + count - 8, h1);
        unsigned char *const begin = d;
        unsigned char *de = d + count;
        const unsigned char *se = s + count;
        unsigned char *const top = de;
        const size_t skew = (uintptr_t)de & 15;
        de -= skew; se -= skew; count -= skew;
        for (; count >= 32; count -= 32) {
            de -= 32; se -= 32;
            uint64_t a, b, c, e;
            LOAD64(se, a); LOAD64(se + 8, b); LOAD64(se + 16, c); LOAD64(se + 24, e);
            STORE64(de, a); STORE64(de + 8, b); STORE64(de + 16, c); STORE64(de + 24, e);
        }
        STORE64(top - 16, h0); STORE64(top - 8, h1);
        STORE64(begin, t0); STORE64(begin + 8, t1); STORE64(begin + 16, t2); STORE64(begin + 24, t3);
    }
    return dest;
}

RELAXED void *ss_memcpy_unaligned(void *restrict dest, const void *restrict src, size_t count)
{
    return ss_memmove_unaligned(dest, src, count);
}

RELAXED void *ss_memset_unaligned(void *dest, int value, size_t count)
{
    unsigned char *d = dest;
    const uint64_t w = (unsigned char)value * UINT64_C(0x0101010101010101);
    if (count <= 32) {
        if (count >= 16) { STORE64(d, w); STORE64(d + 8, w); STORE64(d + count - 16, w); STORE64(d + count - 8, w); }
        else if (count >= 8) { STORE64(d, w); STORE64(d + count - 8, w); }
        else if (count >= 4) { uint32_t v = (uint32_t)w; STORE32(d, v); STORE32(d + count - 4, v); }
        else if (count >= 2) { uint16_t v = (uint16_t)w; STORE16(d, v); STORE16(d + count - 2, v); }
        else if (count) *d = (unsigned char)value;
        return dest;
    }
    /* Stores in increasing order, none twice (see ss_memmove_unaligned). */
    size_t skew = (size_t)(-(uintptr_t)d & 15);
    count -= skew;
    if (skew & 1) { *d++ = (unsigned char)value; }
    if (skew & 2) { uint16_t v = (uint16_t)w; STORE16(d, v); d += 2; }
    if (skew & 4) { uint32_t v = (uint32_t)w; STORE32(d, v); d += 4; }
    if (skew & 8) { STORE64(d, w); d += 8; }
    _Pragma("clang loop unroll(disable) interleave(disable)")
    for (; count >= 64; count -= 64, d += 64) {
        STORE64(d, w); STORE64(d + 8, w); STORE64(d + 16, w); STORE64(d + 24, w);
        STORE64(d + 32, w); STORE64(d + 40, w); STORE64(d + 48, w); STORE64(d + 56, w);
    }
    if (count & 32) { STORE64(d, w); STORE64(d + 8, w); STORE64(d + 16, w); STORE64(d + 24, w); d += 32; }
    if (count & 16) { STORE64(d, w); STORE64(d + 8, w); d += 16; }
    if (count & 8) { STORE64(d, w); d += 8; }
    if (count & 4) { uint32_t v = (uint32_t)w; STORE32(d, v); d += 4; }
    if (count & 2) { uint16_t v = (uint16_t)w; STORE16(d, v); d += 2; }
    if (count & 1) { *d = (unsigned char)value; }
    return dest;
}
