#ifndef SUNSTONE_A733_MMC_PLATFORM_NATIVE_H
#define SUNSTONE_A733_MMC_PLATFORM_NATIVE_H
#include "a733_mmc_platform.h"
#include "a733_storage_update.h"
struct a733_platform_locks;
struct a733_mmc_platform_native {
    struct a733_mmc_platform_ops operations;
    struct a733_platform_locks *locks;
    void *owner_task,*admission_cookie;
    int (*admit)(void *,const struct a733_mmc_platform_config *,const struct a733_clock_token *);
    enum sunxi_mmc_kind kind;
    enum a733_mmc_emmc_host emmc_host;
    struct a733_storage_update *update;
    unsigned initialized;
    unsigned holding; /* Register mutex entered by this binding, until leave or retain. */
    enum sunxi_mmc_sd_profile sd_profile; /* Set by successful actual platform admission. */
};
/* Pure selected-aperture filter. Parent clocks, shared storage fabric, voltage,
 * PMIC, other storage controllers and card data writes are never writable. The
 * native adapter separately admits the UHS-I profile's one PF selector write. */
static inline int a733_mmc_platform_native_address_selected(enum sunxi_mmc_kind kind,
    enum a733_mmc_emmc_host host,uintptr_t a,unsigned writing)
{
    if((kind!=SUNXI_MMC_SD&&kind!=SUNXI_MMC_EMMC)||(a&3)||
       (unsigned)host>A733_MMC_EMMC_SDC3||(kind==SUNXI_MMC_SD&&host))return 0;
    unsigned index=a733_mmc_slot(kind,host);
    uintptr_t base=A733_MMC_BASE(index);
    if(a>=base&&a<=base+0x300) {
        switch(a-base) {
        case 0:case 4:case 8:case 0xc:case 0x10:case 0x14:case 0x18:case 0x1c:
        case 0x30:case 0x38:case 0x54:case 0x58:case 0x5c:case 0x80:case 0x8c:
        case 0x104:case 0x10c:case 0x140:case 0x144:return 1;
        /* IDMAC: FIFO watermarks, list base and status. The platform admits
         * their values only for a configuration with a descriptor area. */
        case 0x40:case 0x84:case 0x88:return 1;
        case 0x48:return !writing; /* Card byte count, read timing only. */
        case 0x100:return kind==SUNXI_MMC_SD; /* Read threshold: SD UHS-I only. */
        case 0x7c:return index==3&&!writing;
        case 0x20:case 0x24:case 0x28:case 0x2c:case 0x3c:case 0x200:case 0x300:return !writing;
        default:return 0;
        }
    }
    uintptr_t module=A733_MMC_MODULE(index);
    if(a==module||a==A733_MMC_BUS(index))return 1;
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
static inline int a733_mmc_platform_native_address(enum sunxi_mmc_kind kind,uintptr_t a,unsigned writing)
{return a733_mmc_platform_native_address_selected(kind,A733_MMC_EMMC_SDC2,a,writing);}
/* Designated real FreeRTOS task and same common register mutex as other users.
 * kind is immutable and admission must authenticate this exact selection. */
int a733_mmc_platform_native_init(struct a733_mmc_platform_native *,
    struct a733_platform_locks *,void *owner_task,enum sunxi_mmc_kind,
    int (*admit)(void *,const struct a733_mmc_platform_config *,const struct a733_clock_token *),void *);
/* Explicit eMMC host selection; the existing initializer retains SDC2 default. */
int a733_mmc_platform_native_init_selected(struct a733_mmc_platform_native *,
    struct a733_platform_locks *,void *owner_task,enum sunxi_mmc_kind,enum a733_mmc_emmc_host,
    int (*admit)(void *,const struct a733_mmc_platform_config *,const struct a733_clock_token *),void *);
/* Exact data-write boundary used by the native adapter after task/mutex
 * admission. It adds no write authority to the default aperture filter. */
static inline int a733_mmc_platform_native_transfer_allowed(enum sunxi_mmc_kind kind,
    enum a733_mmc_emmc_host host,const struct a733_storage_update *u,uintptr_t a,uint32_t v)
{
    return u&&u->platform&&u->platform->config.kind==kind&&u->platform->config.emmc_host==host&&
        a733_storage_update_mmio(u,a,v);
}
/* The same boundary for one FIFO write burst of count words. */
static inline int a733_mmc_platform_native_fifo_allowed(enum sunxi_mmc_kind kind,
    enum a733_mmc_emmc_host host,const struct a733_storage_update *u,uintptr_t a,unsigned count)
{
    return u&&u->platform&&u->platform->config.kind==kind&&u->platform->config.emmc_host==host&&
        a733_storage_update_fifo(u,a,count);
}
/* Bind once after the actual service owns this exact platform. Existing native
 * initialization and address helper remain read-only until this binding. */
int a733_mmc_platform_native_bind_update(struct a733_mmc_platform_native *,struct a733_storage_update *);
#endif
