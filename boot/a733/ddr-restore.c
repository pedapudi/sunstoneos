/* SRAM-only admission and PHY restore for the pinned boot0 experiment.
 * Addresses and call order: docs/designs/dram-training-restore.md.
 * No runtime library, allocation, retraining or storage access. */
#include <stdint.h>
#include "restore-profile.h"

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
#define RECORD ((const u8 *)0x87000)
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

u32 restore_admit(void)
{
    const u8 *r = RECORD, *p = r + 128;
    if (word(r) != 0x33375341 || word(r+4) != 0x4e525444 || word(r+8) != 1 || word(r+12) != 128)
        return GEOMETRY;
    if (crc(r, 124) != word(r+124)) return HEADER_CRC;
    u32 length = word(r+16);
    if (length > LIMIT-128) return GEOMETRY;
    if (crc(p, length) != word(r+20)) return PAYLOAD_CRC;
    for (u32 i = 100; i < 124; i += 4) if (word(r+i)) return GEOMETRY;
    for (u32 i = 0; i < 4; ++i)
        if (word(r+32+4*i) != ((volatile u32 *)0x03006200)[i]) return CHIP;
    if (word(r+48) != BOOT0_CHECKSUM || *(volatile u32 *)0x4700c != BOOT0_CHECKSUM) return BOOT0;
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
    /* No edit precedes complete admission. The first run contains 20 u32s. */
    for (u32 i = 0; i < 20; ++i) PARAM[32+i] = word(p+8+4*i);
    PARAM[30] &= ~4u;
    PARAM[27] = (PARAM[27] & ~0x3fff0000u) | 0x10000;
    PARAM[23] |= 0x80000000u;
    barrier();
    return 0;
}

static u32 restore_phy(u32 write)
{
    const u8 *r = RECORD, *p = r+128;
    u32 offset = 88, index = 0; /* Skip the admitted parameter run. */
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
                MAIL[11] = address; MAIL[12] = expected; MAIL[13] = actual;
                *MUX = 1; barrier();
                return READBACK;
            }
        }
        offset += (count*2+3)&~3u;
    }
    *MUX = 1;
    barrier();
    return 0;
}

u32 restore_write(void)
{
    if (MAIL[9]++) return HOOK;
    return restore_phy(1);
}

u32 restore_check(u32 size)
{
    if (MAIL[9] != 1 || MAIL[8] != PROFILE_COUNT) return HOOK;
    if (size != 12288) return SIZE;
    /* Cache-off entry is mandatory. Check again after opaque boot0 calls. */
    u32 control;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(control));
    if (control & 0x1005) return ENTRY;
    u32 failure = restore_phy(0);
    if (failure) return failure;
    /* Two 4 KiB pages, 32 walking-one passes and two address patterns.
     * 8 KiB distinct memory, 278528 bytes written and read each. */
    for (u32 pass = 0; pass < 34; ++pass) {
        for (u32 page = 0; page < 2; ++page) {
            volatile u32 *memory = (volatile u32 *)(0x41000000u+page*0x1000000u);
            for (u32 i = 0; i < 1024; ++i)
                memory[i] = pass < 32 ? 1u << pass : ((u32)&memory[i] ^ (pass == 32 ? 0xa55a5aa5u : 0x5aa5a55au));
            barrier();
            for (u32 i = 0; i < 1024; ++i) {
                u32 expected = pass < 32 ? 1u << pass : ((u32)&memory[i] ^ (pass == 32 ? 0xa55a5aa5u : 0x5aa5a55au));
                u32 actual = memory[i];
                MAIL[10] += 4;
                if (actual != expected) {
                    MAIL[11] = (u32)&memory[i]; MAIL[12] = expected; MAIL[13] = actual;
                    return MEMORY;
                }
            }
        }
    }
    return 0;
}
