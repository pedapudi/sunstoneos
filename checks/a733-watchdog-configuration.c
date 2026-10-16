#include <stdint.h>
#include "a733_rom_return.h"
#include "boot.h"

/* SRAM-only configuration trial. No instruction writes the watchdog mode or
 * feed register. A separate, protected expiry trial is still required.
 */
void watchdog_configuration(void)
{
    volatile uint32_t *result = (volatile uint32_t *)0x52800;
    if (result[0] != 0x57444346) return;
    result[1] = 0x57525031;
    result[2] = 2; // Refused until ownership is established.
    result[3] = 1;
    result[4] = mmio_read(0x02050014) & 0xffff;
    result[5] = mmio_read(0x02050010) & 0xffff;
    result[6] = mmio_read(0x02050018) & 0xffff;
    result[7] = mmio_read(0x070501c0);
    result[8] = mmio_read(0x070501c4);
    result[9] = mmio_read(0x070501d4);
    if ((result[4] & 1) || result[7] || result[9]) return;

    struct a733_rom_return context = {0};
    if (!a733_rom_return_prepare(&context)) {
        result[2] = 3;
        result[14] = context.owned;
        return;
    }
    result[3] = 2;
    result[10] = mmio_read(0x070501c0);
    result[11] = mmio_read(0x070501c4);
    uint32_t configuration = (result[5] & ~3U) | 1;
    mmio_write(0x02050018, 0x16aa0002);
    mmio_write(0x02050010, 0x16aa0000 | configuration);
    result[12] = mmio_read(0x02050010) & 0xffff;
    result[13] = mmio_read(0x02050018) & 0xffff;
    result[2] = result[12] == configuration && result[13] == 2 ? 0 : 4;

    mmio_write(0x02050010, 0x16aa0000 | result[5]);
    mmio_write(0x02050018, 0x16aa0000 | result[6]);
    if ((mmio_read(0x02050010) & 0xffff) != result[5] ||
        (mmio_read(0x02050018) & 0xffff) != result[6]) result[2] = 5;
    if (!a733_rom_return_restore(&context)) result[2] = 5;
    result[14] = context.owned;
    result[15] = mmio_read(0x02050014) & 0xffff;
    if (result[15] != result[4]) result[2] = 5;
    result[3] = 3;
}
