#include "a733_i2s_platform_native.h"
#include "a733_mmio.h"
#define CCU UINT32_C(0x02002000)
#define I2S UINT32_C(0x02532000)
static int controller(uintptr_t a, unsigned write)
{
    if (a < I2S) return 0;
    switch (a-I2S) {
    case 0: case 4: case 8: case 20: case 28: case 36: case 44: case 48:
    case 40: case 52: case 68: case 72:
    case 100: case 104: case 108: case 112: case 116: return 1;
    case 24: return !write; /* FIFO occupancy is read-only. */
    case 12: return 1; /* Stream startup acknowledges only FIFO error bits. */
    default: return 0;
    }
}
static int readable(uintptr_t a)
{
    if (controller(a,0)) return 1;
    if (a == UINT32_C(0x07090160)) return 1;
    if (a < CCU) return 0;
    switch (a-CCU) {
    case 0: case 0x280: case 0x288: case 0x28c: case 0x191c: case 0x199c:
    case 0x1200: case 0x120c: case 0x1210: case 0x1220: case 0x1224:
    case 0x1230: case 0x1240: case 0x1280: case 0x12c0: return 1;
    default: return 0;
    }
}
int a733_audio_native_read(void *cookie, uintptr_t a, uint32_t *v)
{
    (void)cookie;
    if (!v || !readable(a)) return 0;
    *v=a733_mmio_read32_dsb_sy_both(a);
    return 1;
}
int a733_audio_native_write(void *cookie, uintptr_t a, uint32_t v)
{
    (void)cookie;
    if(a==I2S+12 && (v&~UINT32_C(0x66)))return 0;
    uint32_t mask=0;
    if (a == CCU+0x280) mask=UINT32_C(0xe807ff00); /* N, DIV5, control; LOCK is read-only. */
    else if (a == CCU+0x1200) mask=UINT32_C(0x8700001f);
    else if (a == CCU+0x120c) mask=UINT32_C(0x00010001);
    else if (!controller(a,1)) return 0;
    if (mask) {
        uint32_t old=a733_mmio_read32(a);
        uint32_t ignore=a == CCU+0x280 ? UINT32_C(0x10000000) : 0;
        if ((v ^ old) & ~(mask | ignore)) return 0;
        v=(old & ~mask) | (v & mask);
    }
    a733_mmio_write32_dsb_sy_both(a,v);
    return 1;
}
