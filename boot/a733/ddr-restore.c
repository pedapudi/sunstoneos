/* SRAM-only admission and PHY restore for the pinned boot0 experiment.
 * Addresses and call order: docs/designs/dram-training-restore.md.
 * No runtime library, allocation, retraining or storage access. */
#include <stdint.h>
#include "restore-profile.h"

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
#ifdef CARD_RESTORE
#define RECORD ((const u8 *)0xa0000)
#else
#define RECORD ((const u8 *)0x87000)
#endif
#define MAIL ((volatile u32 *)0x86000)
#define PARAM ((volatile u32 *)0x831f8)
#define MUX ((volatile u16 *)0x0aaa0000)
#define LIMIT 0x6000u
/* Codes are shared with the host through the build manifest. */
enum { GEOMETRY=1, HEADER_CRC, PAYLOAD_CRC, CHIP, BOOT0, STRAP,
       READBACK, SIZE, MEMORY, HOOK, ENTRY };

static u32 word(const u8 *p) { return *(const u32 *)p; }
static u32 crc(const u8 *p, u32 n)
{
    u32 c = ~0u;
    while (n--) {
        c ^= *p++;
        for (u32 b = 0; b != 8; ++b)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}
static void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }

#ifdef CARD_RESTORE
u32 card_admit(const u8 *r, u32 apply)
{
    const u8 *p = r + 128;
#else
u32 restore_admit(void)
{
    const u8 *r = RECORD, *p = r + 128;
#endif
    if (word(r) != 0x33375341 || word(r+4) != 0x4e525444 || word(r+8) != 1 || word(r+12) != 128)
        return GEOMETRY;
    if (crc(r, 124) != word(r+124)) return HEADER_CRC;
    u32 length = word(r+16);
    if (length > LIMIT-128) return GEOMETRY;
    if (crc(p, length) != word(r+20)) return PAYLOAD_CRC;
    for (u32 i = 100; i < 124; i += 4) if (word(r+i)) return GEOMETRY;
    for (u32 i = 0; i < 4; ++i)
        if (word(r+32+4*i) != ((volatile u32 *)0x03006200)[i]) return CHIP;
    if (word(r+48) != BOOT0_CHECKSUM) return BOOT0;
#ifndef CARD_RESTORE
    if (*(volatile u32 *)0x4700c != BOOT0_CHECKSUM) return BOOT0;
#endif
    if (word(r+52) != 5 || word(r+56) != STRAP_CRC || crc((const u8 *)PARAM,128) != STRAP_CRC)
        return STRAP;
    if (word(r+60) != 0x80000000 || word(r+64) || word(r+68) != 0x125f || word(r+72) != 12288)
        return GEOMETRY;
    u32 runs = word(r+80), mo = word(r+84), mc = word(r+88), end = word(r+92);
    if (word(r+76) || word(r+96) || mc != PROFILE_COUNT || mo > length || end != length
            || mc*8 != length-mo || !runs || runs > mo/12) return GEOMETRY;
    for (u32 i = 0; i < PROFILE_COUNT; ++i) {
        const u8 *m = p+mo+8*i;
        if (word(m) != restore_profile[i].address || word(m+4) != restore_profile[i].mask)
            return GEOMETRY;
    }
    u32 offset = 0, index = 0;
    for (u32 run = 0; run < runs; ++run) {
        if (offset > mo || mo-offset < 8) return GEOMETRY;
        u32 address = word(p+offset), count = word(p+offset+4);
        offset += 8;
        if (!count || count > (mo-offset)/2 || count > PROFILE_COUNT+40-index) return GEOMETRY;
        for (u32 i = 0; i < count; ++i, ++index) {
            u32 wanted = index < 40 ? 0x83278+2*index : restore_profile[index-40].address;
            if (address != wanted) return GEOMETRY;
            address += 2;
        }
        offset += (count*2+3)&~3u;
    }
    if (offset != mo || index != PROFILE_COUNT+40 || word(p) != 0x83278 || word(p+4) != 40)
        return GEOMETRY;
#ifdef CARD_RESTORE
    if (!apply) return 0;
#endif
    /* No edit precedes complete admission. The first run contains 20 u32s. */
    for (u32 i = 0; i < 20; ++i) PARAM[32+i] = word(p+8+4*i);
    PARAM[30] &= ~4u;
    PARAM[27] = (PARAM[27] & ~0x3fff0000u) | 0x10000;
    PARAM[23] |= 0x80000000u;
    barrier();
    return 0;
}

/* A mismatch does not stop the pass: every entry is written and read, so
 * one board run names every register that does not hold its value. Words
 * 11-13 keep the first mismatch, word 27 counts them and words 28-31 hold
 * the next four as (profile index << 16) | value read. Any mismatch fails
 * the restore, which then never resumes boot0. */
static u32 restore_phy(u32 write)
{
    const u8 *r = RECORD, *p = r+128;
    u32 offset = 88, index = 0, mismatches = 0; /* Skip the admitted parameter run. */
    *MUX = 0;
    barrier();
    for (u32 run = 1; run < word(r+80); ++run) {
        u32 address = word(p+offset), count = word(p+offset+4);
        offset += 8;
        for (u32 i = 0; i < count; ++i, ++index, address += 2) {
            volatile u16 *reg = (volatile u16 *)address;
            u16 mask = restore_profile[index].mask;
            u16 expected = *(const u16 *)(p+offset+2*i);
            if (write) {
                /* MMU/cache-off FEL accesses are ordered Device accesses.
                 * The same-register read below observes the preceding write;
                 * only mux ownership transitions need an explicit DSB. */
                *reg = (*reg & ~mask) | (expected & mask);
                MAIL[8] = index+1;
            }
            u16 actual = *reg;
            if ((actual ^ expected) & mask) {
                if (!mismatches) { MAIL[11] = address; MAIL[12] = expected; MAIL[13] = actual; }
                else if (mismatches < 5) MAIL[27+mismatches] = index << 16 | actual;
                MAIL[27] = ++mismatches;
            }
        }
        offset += (count*2+3)&~3u;
    }
    *MUX = 1;
    barrier();
    return mismatches ? READBACK : 0;
}

u32 restore_write(void)
{
    if (MAIL[9]++) return HOOK;
    return restore_phy(1);
}

/* Low word of the 24 MHz system counter, which boot0's own timer reads. */
static u32 ticks(void)
{
    u32 low, high;
    __asm__ volatile("isb\n\tmrrc p15, 0, %0, %1, c14" : "=r"(low), "=r"(high));
    (void)high;
    return low;
}

/* Quick memory check after init_DRAM. With the MMU off, AArch32 reaches
 * DRAM only from 0x40000000 to 0xffffffff, the first 3 GiB of 12 GiB.
 * Address lines: a unique word at DRAM and at DRAM + 2^k for k = 2..31, then
 * the complements; a stuck or shorted line aliases two words. Data: sixteen
 * 4 KiB windows 192 MiB apart, each pass written to all windows before any
 * is read back: a rotating one, a rotating zero, address ^ 0xa55a5aa5 and its
 * complement, so every data bit is driven both ways next to unequal
 * neighbours. 65,616 distinct bytes; 262,392 bytes written and read. */
#define DRAM 0x40000000u
#define LINES 31u
#define WINDOWS 16u
#define STRIDE 0x0c000000u
#define WORDS 1024u
#define SEED 0xa55a5aa5u
static volatile u32 *line(u32 n) { return (volatile u32 *)(DRAM + (n ? 1u << (n+1) : 0)); }
static u32 fail(u32 read, volatile u32 *at, u32 expected, u32 actual)
{
    MAIL[10] = read; MAIL[11] = (u32)at; MAIL[12] = expected; MAIL[13] = actual;
    return MEMORY;
}
static u32 memory_check(void)
{
    u32 read = 0;
    for (u32 flip = 0; flip != 2; ++flip) {
        for (u32 n = 0; n < LINES; ++n) *line(n) = 0x01010101u*(n+1) ^ (0u-flip);
        barrier();
        for (u32 n = 0; n < LINES; ++n) {
            u32 expected = 0x01010101u*(n+1) ^ (0u-flip), actual = *line(n);
            read += 4;
            if (actual != expected) return fail(read, line(n), expected, actual);
        }
    }
    for (u32 pass = 0; pass < 4; ++pass) {
        u32 flip = 0u - (pass & 1), rotate = pass < 2;
        for (u32 w = 0; w < WINDOWS; ++w) {
            volatile u32 *at = (volatile u32 *)(DRAM + w*STRIDE), *end = at + WORDS;
            for (u32 one = 1; at != end; ++at, one = one << 1 | one >> 31)
                *at = (rotate ? one : (u32)at ^ SEED) ^ flip;
        }
        barrier();
        for (u32 w = 0; w < WINDOWS; ++w) {
            volatile u32 *at = (volatile u32 *)(DRAM + w*STRIDE), *end = at + WORDS;
            for (u32 one = 1; at != end; ++at, one = one << 1 | one >> 31) {
                u32 expected = (rotate ? one : (u32)at ^ SEED) ^ flip, actual = *at;
                read += 4;
                if (actual != expected) return fail(read, at, expected, actual);
            }
        }
    }
    MAIL[10] = read;
    return 0;
}

u32 restore_check(u32 size)
{
    MAIL[25] = ticks();
    if (MAIL[9] != 1 || MAIL[8] != PROFILE_COUNT) return HOOK;
    if (size != 12288) return SIZE;
    /* Cache-off entry is mandatory. Check again after opaque boot0 calls. */
    u32 control;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(control));
    if (control & 0x1005) return ENTRY;
    u32 failure = restore_phy(0);
    if (!failure) failure = memory_check();
    MAIL[26] = ticks();
    return failure;
}
