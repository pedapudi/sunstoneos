#ifndef SUNSTONE_A733_RADIO_PLATFORM_NATIVE_H
#define SUNSTONE_A733_RADIO_PLATFORM_NATIVE_H
#include "a733_radio_platform.h"
struct a733_platform_locks;
struct a733_radio_platform_native {
    struct a733_radio_platform_ops operations;
    struct a733_platform_locks *locks;
    void *owner_task,*admission_cookie;
    int (*admit)(void *,const struct a733_radio_platform_config *,const struct a733_clock_token *);
    unsigned initialized;
};
/* Pure address filtering. SDC0/SDC2/storage apertures, PM2, parents, supplies
 * and shared fabric gates cannot be written through this adapter. */
static inline int a733_radio_platform_native_address(uintptr_t a,unsigned writing)
{
    if(a&3)return 0;
    if(a>=UINT32_C(0x04021000)&&a<=UINT32_C(0x04021300)) {
        switch(a-UINT32_C(0x04021000)) {
        case 0:case 4:case 8:case 0xc:case 0x10:case 0x14:case 0x18:case 0x1c:
        case 0x30:case 0x38:case 0x5c:case 0x80:case 0x8c:case 0x104:
        case 0x140:case 0x144:case 0x200:return 1;
        case 0x20:case 0x24:case 0x28:case 0x2c:case 0x3c:case 0x300:return !writing;
        default:return 0;
        }
    }
    if(a==UINT32_C(0x02002d10)||a==UINT32_C(0x02002d1c))return 1;
    if(a>=UINT32_C(0x02002000)&&a<UINT32_C(0x02004000)) {
        switch(a-UINT32_C(0x02002000)) {
        case 0:case 0xa0:case 0xa8:case 0xac:case 0x500:case 0x580:case 0x588:
        case 0x59c:case 0x5c0:case 0x5e0:case 0x1908:case 0x1988:return !writing;
        default:return 0;
        }
    }
    if(a==UINT32_C(0x07090160))return !writing;
    if(a==UINT32_C(0x03400198))return 1; /* INTID194 disable word. */
    return !writing&&(a==UINT32_C(0x03400000)||a==UINT32_C(0x03400118)||
        a==UINT32_C(0x03400218)||a==UINT32_C(0x03400318));
}
/* No MMIO. owner_task is the real lifetime FreeRTOS TaskHandle_t. Same common
 * register lock as pin/clock/GIC writers; actual shared PMIC provider is separate. */
int a733_radio_platform_native_init(struct a733_radio_platform_native *,
    struct a733_platform_locks *,void *owner_task,
    int (*admit)(void *,const struct a733_radio_platform_config *,const struct a733_clock_token *),void *);
#endif
