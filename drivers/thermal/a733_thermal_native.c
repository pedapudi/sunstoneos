#include "a733_thermal_native.h"
#define CCU UINT32_C(0x02002000)
#define THS UINT32_C(0x02522000)
static int readable(uintptr_t a)
{
    if (a==UINT32_C(0x07090160) || a==UINT32_C(0x03006244) ||
        a==UINT32_C(0x03006248) || a==UINT32_C(0x0300624c)) return 1;
    if (a>=CCU && a<CCU+0x2000) {
        switch (a-CCU) {
        case 0:case 0xa0:case 0xa8:case 0xac:case 0x518:case 0xfc0:case 0xfe4:
        case 0x1908:case 0x1988:return 1;
        default:return 0;
        }
    }
    if (a<THS || a>THS+0xd0 || (a&3)) return 0;
    switch (a-THS) {
    case 0:case 4:case 8:case 0x10:case 0x14:case 0x18:case 0x20:case 0x24:
    case 0x28:case 0x2c:case 0x30:case 0x40:case 0x44:case 0x48:case 0x4c:case 0x50:
    case 0x80:case 0x84:case 0x88:case 0xa0:case 0xa4:case 0xa8:
    case 0xc0:case 0xc4:case 0xc8:case 0xcc:case 0xd0:return 1;
    default:return 0;
    }
}
int a733_thermal_native_read(void *cookie,uintptr_t address,uint32_t *value)
{
    (void)cookie;
    if (!value || !readable(address)) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    *value=*(volatile const uint32_t *)address;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
int a733_thermal_native_write(void *cookie,uintptr_t address,uint32_t value)
{
    (void)cookie;
    uint32_t mask;unsigned w1c=0;
    if (address==CCU+0xfc0) mask=0x8700001f;
    else if (address==CCU+0xfe4) mask=0x10001;
    else {
        if (address<THS || address>=THS+0xac) return 0;
        switch (address-THS) {
        case 0:if (value!=0x01df002f) return 0;mask=UINT32_MAX;break;
        case 4:case 0x14:case 0x18:mask=31;break;
        case 8:mask=0xfffff000;break;
        case 0x20:case 0x24:case 0x28:case 0x2c:mask=31;w1c=1;break;
        case 0x30:mask=7;break;
        case 0x40:case 0x44:case 0x48:case 0x4c:case 0x50:
        case 0x80:case 0x84:case 0xa0:case 0xa4:mask=0x0fff0fff;break;
        case 0x88:case 0xa8:mask=4095;break;
        default:return 0;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (w1c) {if (value&~mask) return 0;}
    else {
        uint32_t old=*(volatile const uint32_t *)address;
        if ((old^value)&~mask) return 0;
        if (address==CCU+0xfc0 && (old&UINT32_C(0x80000000)) && ((old^value)&mask)) return 0;
        if (address==CCU+0xfe4 && (old&~value&mask)) return 0;
    }
    *(volatile uint32_t *)address=value;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
