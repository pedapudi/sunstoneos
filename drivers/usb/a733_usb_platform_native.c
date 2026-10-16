#include "a733_usb_platform_native.h"
#define CCU UINT32_C(0x02002000)
#define USB UINT32_C(0x04100000)
#define BIT(n) (UINT32_C(1)<<(n))
static int readable(uintptr_t a)
{
    if (a==UINT32_C(0x07090160) || a==UINT32_C(0x070101ac)) return 1;
    if (a>=CCU && a<CCU+0x2000) switch (a-CCU) {
    case 0:case 0xa0:case 0xa8:case 0xac:case 0x500:case 0x580:case 0x588:
    case 0x5a4:case 0x5c0:case 0x5e0:case 0x1300:case 0x1304:case 0x1308:
    case 0x130c:case 0x1340:case 0x1908:case 0x1988:return 1;
    default:return 0;
    }
    if (a>=USB+0x540 && a<=USB+0x630 && !((a-USB-0x540)%16)) return 1;
    return a==USB+0x40 || a==USB+0x48 || a==USB+0x50 || a==USB+0x400 ||
        a==USB+0x410 || a==USB+0x418 || a==USB+0x420 || a==USB+0x500;
}
int a733_usb_platform_native_read(void *cookie,uintptr_t a,uint32_t *v)
{
    (void)cookie;if (!v || !readable(a)) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    *v=*(volatile const uint32_t *)a;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
int a733_usb_platform_native_write(void *cookie,uintptr_t a,uint32_t v)
{
    (void)cookie;uint32_t mask,w1c=0;
    if (a==CCU+0x1340) mask=0x87000000;
    else if (a==CCU+0x1300) mask=BIT(30);
    else if (a==CCU+0x1304) mask=BIT(24)|BIT(8);
    else if (a==USB+0x410) mask=0xa8;
    else if (a==USB+0x418) mask=0xf3f;
    else if (a==USB+0x420) mask=1;
    else if (a==USB+0x400) {mask=0xf000;w1c=0x70;}
    else return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t old=*(volatile const uint32_t *)a;
    if (((v^old)&~(mask|w1c)) || (v&w1c)) return 0;
    if ((a==CCU+0x1300 || a==CCU+0x1304 || a==USB+0x420) && (old&~v&mask)) return 0;
    if (a==CCU+0x1340 && (((old^v)&0x07000000) && ((old|v)&BIT(31)))) return 0;
    if (a==CCU+0x1340 && ((v>>24)&7)>1) return 0;
    if (a==USB+0x410 && (v&mask)!=0x20) return 0;
    if (a==USB+0x418 && (v&mask)!=0x806) return 0;
    if (a==USB+0x400 && (v&mask)!=0xf000) return 0;
    *(volatile uint32_t *)a=v;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
