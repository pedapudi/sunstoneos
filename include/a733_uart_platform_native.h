#ifndef SUNSTONE_A733_UART_PLATFORM_NATIVE_H
#define SUNSTONE_A733_UART_PLATFORM_NATIVE_H
#include "a733_uart_platform.h"
struct a733_platform_locks;
struct a733_uart_platform_native {
    struct a733_uart_platform_ops operations;
    struct a733_platform_locks *locks;
    void *owner_task,*admission_cookie;
    int (*admit)(void *,const struct a733_uart_platform_config *,
        const struct a733_clock_token *,enum a733_uart_admission);
    unsigned port,initialized;
};
/* Pure address admission for the selected controller, its clock/reset bits,
 * source observations and GIC mask/pending words. Parent writes are excluded. */
static inline int a733_uart_platform_native_address(unsigned port,uintptr_t a,unsigned writing)
{
    if (port>8 || (a&3)) return 0;
    uintptr_t uart=port<7 ? UINT32_C(0x02500000)+port*0x1000u : UINT32_C(0x07080000)+(port-7)*0x1000u;
    if (a>=uart && a<=uart+0xc0) {
        switch (a-uart) {
        case 0:case 4:case 8:case 0xc:case 0x10:case 0xa4:case 0x88:case 0x8c:return 1;
        case 0x14:case 0x18:case 0x7c:return !writing;
        case 0xc0:return port && port<7; /* RS485 control: RTS-capable ports. */
        default:return 0;
        }
    }
    if (a==UINT32_C(0x07090160)) return !writing;
    if (port<7) {
        if (a==UINT32_C(0x02002000) || a==UINT32_C(0x02002538)) return !writing;
        if (a==UINT32_C(0x02002e00)+port*4u) return 1;
    } else {
        if (a==UINT32_C(0x07010010)) return !writing;
        if (a==UINT32_C(0x0701018c)) return 1;
    }
    unsigned intid=port<7 ? 34+port : 233+port-7;
    uintptr_t bank=UINT32_C(0x03400000)+(intid/32)*4u;
    if (a==UINT32_C(0x03400000) || a==bank+0x100 || a==bank+0x200 || a==bank+0x300) return !writing;
    return a==bank+0x180 || a==bank+0x280;
}
/* No MMIO. owner_task is the real FreeRTOS TaskHandle_t for this lifetime.
 * Use the same lock object as all pin/clock/GIC writers. The admission callback
 * must authenticate physical/platform authority; this adapter cannot invent it.
 * All addresses require identity Device mappings and independent recovery. */
int a733_uart_platform_native_init(struct a733_uart_platform_native *,
    struct a733_platform_locks *,void *owner_task,unsigned port,
    int (*admit)(void *,const struct a733_uart_platform_config *,
        const struct a733_clock_token *,enum a733_uart_admission),void *admission_cookie);
#endif
