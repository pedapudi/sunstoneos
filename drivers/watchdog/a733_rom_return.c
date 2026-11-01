#include "a733_rom_return.h"
#include "a733_mmio.h"

#define REDIRECT_FLAG 0x070501c0UL
#define REDIRECT_ENTRY 0x070501c4UL
#define ALTERNATE_FLAG 0x070501d4UL
#define WATCHDOG_MODE 0x02050014UL
#define REDIRECT_MAGIC 0xfa50392fU

int a733_rom_return_restore(struct a733_rom_return *context)
{
    if (!context->owned || (a733_mmio_read32_dsb_sy_both(WATCHDOG_MODE) & 1)) return 0;
    uint32_t flag = a733_mmio_read32_dsb_sy_both(REDIRECT_FLAG);
    if (flag != 0 && flag != REDIRECT_MAGIC) return 0;
    if (flag == REDIRECT_MAGIC && a733_mmio_read32_dsb_sy_both(REDIRECT_ENTRY) != 0x20) return 0;
    // Never replace the entry while the redirect flag remains active.
    a733_mmio_write32_dsb_sy_both(REDIRECT_FLAG, 0);
    if (a733_mmio_read32_dsb_sy_both(REDIRECT_FLAG) != 0) return 0;
    a733_mmio_write32_dsb_sy_both(REDIRECT_ENTRY, context->entry);
    if (a733_mmio_read32_dsb_sy_both(REDIRECT_ENTRY) != context->entry) return 0;
    context->owned = 0;
    return 1;
}

int a733_rom_return_prepare(struct a733_rom_return *context)
{
    if (context->owned || (a733_mmio_read32_dsb_sy_both(WATCHDOG_MODE) & 1) ||
        a733_mmio_read32_dsb_sy_both(REDIRECT_FLAG) != 0 || a733_mmio_read32_dsb_sy_both(ALTERNATE_FLAG) != 0) return 0;
    context->entry = a733_mmio_read32_dsb_sy_both(REDIRECT_ENTRY);
    context->owned = 1;
    a733_mmio_write32_dsb_sy_both(REDIRECT_ENTRY, 0x20);
    if (a733_mmio_read32_dsb_sy_both(REDIRECT_ENTRY) == 0x20) {
        a733_mmio_write32_dsb_sy_both(REDIRECT_FLAG, REDIRECT_MAGIC);
        if (a733_mmio_read32_dsb_sy_both(REDIRECT_FLAG) == REDIRECT_MAGIC) return 1;
    }
    (void)a733_rom_return_restore(context);
    return 0;
}
