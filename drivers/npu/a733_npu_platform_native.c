#include "a733_npu_platform_native.h"
#include "a733_mmio.h"
#define CCU UINT32_C(0x02002000)
#define PCK UINT32_C(0x07064000)
static int readable(uintptr_t address)
{
    if (address==0x07090160 || address==0x070101ac) return 1;
    if (address>=CCU && address<CCU+0x2000) {
        switch(address-CCU) {
        case 0:case 0xa0:case 0xa8:case 0xac:case 0x2a0:case 0x2a8:case 0x2ac:
        case 0x500:case 0x580:case 0x588:case 0x5c0:case 0x5e0:case 0xb00:case 0xb04:
        case 0x1908:case 0x1920:case 0x1988:case 0x19a0:case 0x1b1c:return 1;
        default:return 0;
        }
    }
    if (address>=PCK && address<PCK+0x1000) {
        switch(address-PCK) {
        case 0:case 8:case 0x170:case 0x174:case 0xc00:case 0xc04:case 0xc10:return 1;
        default:return 0;
        }
    }
    return 0;
}
int a733_npu_platform_native_read(void *cookie,uintptr_t address,uint32_t *value)
{
    (void)cookie;
    if (!value || !readable(address)) return 0;
    *value=a733_mmio_read32_dsb_sy_both(address);return 1;
}
int a733_npu_platform_native_write(void *cookie,uintptr_t address,uint32_t value)
{
    (void)cookie;
    uint32_t mask,expected=0;
    if (address==CCU+0xb00) mask=0x8700001f;
    else if (address==CCU+0xb04) mask=0x70001;
    else if (address==PCK) {mask=0xf;expected=8;}
    else {
        mask=UINT32_MAX;
        switch(address) {
        case PCK+0x170:expected=0x1f1f1f;break;
        case PCK+0x174:expected=0x1f1f;break;
        case PCK+0xc00:expected=0x08080808;break;
        case PCK+0xc04:expected=0x0808;break;
        case PCK+0xc10:expected=8;break;
        default:return 0;
        }
    }
    uint32_t old=a733_mmio_read32_dsb_sy_both(address);
    if ((old^value)&~mask) return 0;

    if (address==CCU+0xb00) {
        if ((old&0x80000000) && value!=old && value!=(old&~UINT32_C(0x80000000))) return 0;
        if ((value&0x07000000) || ((value&31)!=0 && (value&31)!=1 && (value&31)!=3)) return 0;
    }
    if (address>=PCK && address<PCK+0x1000) {
        if ((value&mask)!=expected || (a733_mmio_read32_dsb_sy_both(PCK+8)&0xf)!=0) return 0;
    }
    a733_mmio_write32_dsb_sy_after(address,value);
    return 1;
}
