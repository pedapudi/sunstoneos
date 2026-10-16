#ifndef SUNSTONE_A733_MMC_PLATFORM_NATIVE_H
#define SUNSTONE_A733_MMC_PLATFORM_NATIVE_H
#include "a733_mmc_platform.h"
struct a733_platform_locks;
struct a733_mmc_platform_native {
    struct a733_mmc_platform_ops operations;
    struct a733_platform_locks *locks;
    void *owner_task,*admission_cookie;
    int (*admit)(void *,const struct a733_mmc_platform_config *,const struct a733_clock_token *);
    enum sunxi_mmc_kind kind;
    unsigned initialized;
};
/* Pure selected-aperture filter. Parent clocks, shared storage fabric, voltage,
 * PMIC, other storage controllers and card data writes are never writable. */
static inline int a733_mmc_platform_native_address(enum sunxi_mmc_kind kind,uintptr_t a,unsigned writing)
{
    if((kind!=SUNXI_MMC_SD&&kind!=SUNXI_MMC_EMMC)||(a&3))return 0;
    uintptr_t base=kind==SUNXI_MMC_SD?0x04020000:0x04022000;
    if(a>=base&&a<=base+0x300) {
        switch(a-base) {
        case 0:case 4:case 8:case 0xc:case 0x10:case 0x14:case 0x18:case 0x1c:
        case 0x30:case 0x38:case 0x54:case 0x58:case 0x5c:case 0x80:case 0x8c:
        case 0x104:case 0x10c:case 0x140:case 0x144:return 1;
        case 0x20:case 0x24:case 0x28:case 0x2c:case 0x3c:case 0x200:case 0x300:return !writing;
        default:return 0;
        }
    }
    uintptr_t module=kind==SUNXI_MMC_SD?0x02002d00:0x02002d20;
    if(a==module||a==module+0xc)return 1;
    if(a>=0x02002000&&a<0x02004000) {
        switch(a-0x02002000) {
        case 0:case 0xa0:case 0xa8:case 0xac:case 0x500:case 0x580:case 0x588:
        case 0x59c:case 0x5c0:case 0x5e0:case 0x1908:case 0x1988:return !writing;
        default:return 0;
        }
    }
    if(a==0x02000040||a==0x02000048||a==0x02000070||a==0x07090160)return !writing;
    if(a==0x03400198)return 1;
    return !writing&&(a==0x03400000||a==0x03400118||a==0x03400218||a==0x03400318);
}
/* Designated real FreeRTOS task and same common register mutex as other users.
 * kind is immutable and admission must authenticate this exact selection. */
int a733_mmc_platform_native_init(struct a733_mmc_platform_native *,
    struct a733_platform_locks *,void *owner_task,enum sunxi_mmc_kind,
    int (*admit)(void *,const struct a733_mmc_platform_config *,const struct a733_clock_token *),void *);
#endif
