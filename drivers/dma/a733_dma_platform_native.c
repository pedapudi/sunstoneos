#include "a733_dma_platform_native.h"
#define CCU UINT32_C(0x02002000)
#define DMA UINT32_C(0x04601000)
static int readable(uintptr_t address)
{
    if (address==UINT32_C(0x07090160)) return 1;
    if (address>=CCU && address<CCU+0x2000) {
        switch (address-CCU) {
        case 0: case 0xa0: case 0xa8: case 0xac: case 0x500: case 0x580:
        case 0x588: case 0x5e4: case 0x704: case 0x1908: case 0x1988: return 1;
        default:return 0;
        }
    }
    if (address==DMA+0x28 || address==DMA+0x30 || address==DMA+0x34 || address==DMA+0x38) return 1;
    if (address>=DMA+0x100 && address<DMA+0x500) {
        switch ((address-DMA-0x100)%0x40) {
        case 0: case 4: case 0x28: case 0x34: case 0x38: case 0x3c: return 1;
        default:return 0;
        }
    }
    return 0;
}
int a733_dma_platform_native_read(void *cookie,uintptr_t address,uint32_t *value)
{
    (void)cookie;
    if (!value || !readable(address)) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    *value=*(volatile const uint32_t *)address;
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
int a733_dma_platform_native_write(void *cookie,uintptr_t address,uint32_t value)
{
    (void)cookie;
    uint32_t mask;
    if (address==CCU+0x704) mask=0x10001;
    else if (address==CCU+0x5e4) mask=1;
    else if (address==DMA+0x34 || address==DMA+0x38) mask=0xffff;
    else if (address==DMA+0x28) mask=UINT32_MAX;
    else return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t old=*(volatile const uint32_t *)address;
    if ((value^old)&~mask) return 0;
    if ((address==CCU+0x704 || address==CCU+0x5e4) && (old&~value&mask)) return 0;
    if (address==CCU+0x5e4 && (old&0x40302)!=0x40302) return 0;
    if (address==DMA+0x28 && (value!=4 || (old && old!=4))) return 0;
    *(volatile uint32_t *)address=value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
