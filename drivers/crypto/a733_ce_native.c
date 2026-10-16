#include "a733_ce_native.h"
#define CCU UINT32_C(0x02002000)
#define NS UINT32_C(0x04603000)
#define SEC UINT32_C(0x04603800)
static int readable(uintptr_t a)
{
    if (a==UINT32_C(0x07090160)) return 1;
    if (a>=CCU && a<CCU+0x2000) {
        switch (a-CCU) {
        case 0:case 0xa0:case 0xa8:case 0xac:case 0x500:case 0x580:case 0x588:
        case 0x5e4:case 0xac0:case 0xac4:case 0x1908:case 0x1988:return 1;
        default:return 0;
        }
    }
    if (a>=SEC && a<=SEC+0x9c) {
        switch (a-SEC) {
        case 8:case 12:case 0x10:case 0x18:case 0x90:case 0x94:case 0x98:case 0x9c:return 1;
        default:return 0;
        }
    }
    if (a<NS || a>NS+0xd0) return 0;
    switch (a-NS) {case 0:case 4:case 8:case 12:case 0x10:case 0x18:case 0xd0:return 1;default:return 0;}
}
int a733_ce_native_read(void *cookie,uintptr_t a,uint32_t *v)
{
    (void)cookie;
    if (!v || !readable(a)) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    *v=*(volatile const uint32_t *)a;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
int a733_ce_native_write(void *cookie,uintptr_t a,uint32_t v)
{
    (void)cookie;
    uint32_t mask=0;
    if (a<NS || a>NS+0xd0) return 0;
    switch (a-NS) {
    case 0:mask=UINT32_MAX;break;
    case 4:mask=255;break;
    case 8:mask=4;break;
    case 12:if (v!=0x30) return 0;break;
    case 0x10:if (v!=0x400) return 0;break;
    case 0xd0:mask=2;break;
    default:return 0;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (mask) {
        uint32_t old=*(volatile const uint32_t *)a;
        if ((old^v)&~mask) return 0;
        if (a==NS+0xd0 && ((old&1) || !(v&2))) return 0;
    }
    *(volatile uint32_t *)a=v;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
int a733_ce_native_sync(void *cookie,struct a733_ce_memory *m,enum a733_ce_sync mode)
{
    (void)cookie;
    if (!m || ((uintptr_t)m&63) || sizeof(*m)>UINTPTR_MAX-(uintptr_t)m ||
        (mode!=A733_CE_PUBLISH && mode!=A733_CE_ACQUIRE)) return 0;
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    if (((ctr>>16)&15)!=4) return 0; /* A733 DminLine: 4 * 2^4 = 64 bytes. */
    __asm__ volatile("dsb sy" ::: "memory");
    if (mode==A733_CE_PUBLISH) {
        for (unsigned i=0;i<256;i+=64) {
            uintptr_t a=(uintptr_t)m->descriptor+i;
            __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
        }
        __asm__ volatile("dc civac, %0" :: "r"((uintptr_t)m->output) : "memory");
    } else __asm__ volatile("dc ivac, %0" :: "r"((uintptr_t)m->output) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
