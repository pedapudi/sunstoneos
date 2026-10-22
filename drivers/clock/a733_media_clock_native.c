#include "a733_media_clock_native.h"
#include "a733_mmio.h"
#define CCU UINT32_C(0x02002000)
#define BIT(n) (UINT32_C(1)<<(n))
static int readable(uintptr_t address)
{
    if (address==UINT32_C(0x07090160) || address==UINT32_C(0x070101ac)) return 1;
    if (address>=UINT32_C(0x07060000) && address<=UINT32_C(0x07069008)) {
        unsigned offset=(unsigned)(address-UINT32_C(0x07060000));
        unsigned domain=offset/0x1000;
        return (domain<=3 || domain==8 || domain==9) && (offset%0x1000==0 || offset%0x1000==8);
    }
    if (address<CCU || address>=CCU+0x2000) return 0;
    switch (address-CCU) {
    case 0: case 0xa0: case 0xa8: case 0xac: case 0x120: case 0x128: case 0x12c:
    case 0x2e0: case 0x2e8: case 0x2ec: case 0x500: case 0x580: case 0x588:
    case 0x58c: case 0x5b4: case 0x5c0: case 0x5e0: case 0x5e4:
    case 0xa00: case 0xa04: case 0xa74: case 0x1500: case 0x1508: case 0x150c:
    case 0x1510: case 0x1580: case 0x1588: case 0x158c: case 0x15c0: case 0x15c4:
    case 0x16c4: case 0x16e4: case 0x1800: case 0x1804: case 0x1808:
    case 0x1840: case 0x1844: case 0x1860: case 0x1884: case 0x1908: case 0x1910:
    case 0x1928: case 0x1988: case 0x1990: case 0x19a8: return 1;
    default:return 0;
    }
}
int a733_media_clock_native_read(void *cookie,uintptr_t address,uint32_t *value)
{
    (void)cookie;
    if (!value || !readable(address)) return 0;
    *value=a733_mmio_read32_dsb_sy_both(address);return 1;
}
int a733_media_clock_native_write(void *cookie,uintptr_t address,uint32_t value)
{
    (void)cookie;
    if (address<CCU || address>=CCU+0x2000) return 0;
    uint32_t mask;unsigned module=0,mclk=0;
    switch (address-CCU) {
    case 0x1884: case 0xa74: case 0x16e4: mask=0x10000;break;
    case 0x1844: case 0xa04: case 0x16c4: case 0x150c: case 0x158c: mask=0x10001;break;
    case 0x58c: case 0x5b4:mask=0x10007;break;
    case 0x5e4:mask=0x300;break;
    case 0x1840: case 0x1860: case 0xa00: case 0x1588:mask=0x8700001f;module=1;break;
    case 0x1804: case 0x1808:mask=0x87001f1f;module=1;mclk=1;break;
    default:return 0;
    }
    a733_mmio_dsb_sy();
    uint32_t old=a733_mmio_read32(address);
    if (((value^old)&~mask) || (!module && (old&~value&mask))) return 0;
    if (module && (old&BIT(31)) && ((old^value)&mask)) return 0;
    if (mclk && (value&BIT(31))) return 0;
    if (address==CCU+0x5e4 && (old&0x40302)!=0x40302) return 0;
    a733_mmio_write32(address,value);
    a733_mmio_dsb_sy();return 1;
}
