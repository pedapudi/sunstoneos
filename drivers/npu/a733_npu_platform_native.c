#include "a733_npu_platform_native.h"
#include "a733_mmio.h"
#define CCU UINT32_C(0x02002000)
#define PCK UINT32_C(0x07064000)
#define LOCK UINT32_C(0x10000000)
static int readable(uintptr_t address)
{
    if (address==0x07090160 || address==PCK || address==PCK+8) return 1;
    if (address<CCU || address>=CCU+0x2000) return 0;
    switch(address-CCU) {
    case 0:case 0xa0:case 0xa8:case 0xac:case 0x2a0:case 0x2a4:case 0x2a8:case 0x2ac:case 0x2b0:
    case 0x500:case 0x580:case 0x588:case 0xa80:case 0xa88:case 0xa8c:case 0xb00:case 0xb04:
    case 0xc00:case 0xc0c:case 0x1908:case 0x1920:case 0x1988:case 0x19a0:
    case 0x1b10:case 0x1b14:case 0x1b1c:return 1;
    default:return 0;
    }
}
int a733_npu_platform_native_read(void *cookie,uintptr_t address,uint32_t *value)
{
    (void)cookie;
    if (!value || !readable(address)) return 0;
    *value=a733_mmio_read32_dsb_sy_both(address);return 1;
}
/* Writable: the NPU module and bus words, the PLL_NPU control word and its
 * gate control. PCK600 domain 4 belongs to the power-domain owner. */
int a733_npu_platform_native_write(void *cookie,uintptr_t address,uint32_t value)
{
    (void)cookie;
    uint32_t old=a733_mmio_read32_dsb_sy_both(address);
    switch (address) {
    case CCU+0xb00:
        if ((old^value)&~UINT32_C(0x8700001f)) return 0;
        if ((old&0x80000000) && value!=old && value!=(old&~UINT32_C(0x80000000))) return 0;
        if ((value&0x07000000) || ((value&31)!=0 && (value&31)!=1 && (value&31)!=3)) return 0;
        break;
    case CCU+0xb04:
        if ((old^value)&~UINT32_C(0x70001)) return 0;
        break;
    case CCU+0x1920:
        if (((old^value)&~UINT32_C(0x10001)) || (old&~value&0x10001)) return 0;
        break;
    case CCU+0x2a0: {
        uint32_t mask=UINT32_C(0xe870ff02); /* PLL/LDO/lock enable, output, factors. */
        if ((value&LOCK) || ((old^value)&~(mask|LOCK))) return 0;
        if (old&~value&UINT32_C(0xc0000000)) return 0; /* Never disable PLL/LDO. */
        if (((old^value)&0x0070ff02) &&
            ((old&0x08000000) || (value&0x0070ff02)!=0x00305100)) return 0;
        if ((value&0x08000000) && (old&0xf0000000)!=0xf0000000) return 0;
        break;
    }
    default:return 0;
    }
    a733_mmio_write32_dsb_sy_after(address,value);
    return 1;
}
