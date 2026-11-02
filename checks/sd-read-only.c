/* Funnel test of the read-only SD core: the translation unit of the driver is
 * included so that the static queue() is called directly. */
#include "../drivers/storage/sunxi_mmc.c"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    static const uint32_t flags[] = {0, RESP, RESP | CRC | DATA | WAIT, RESP | CRC | DATA | WRITE | WAIT, AUTO | RESP};
    unsigned allowed = 0, refused = 0;
    for (uint32_t index = 0; index < 64; ++index)
        for (unsigned f = 0; f < sizeof flags / sizeof flags[0]; ++f) {
            struct sunxi_mmc h = {0};
            h.command = 0x1234;
            h.state = SUNXI_MMC_READY;
            queue(&h, P_IDLE, index | flags[f], 0x55, 0, 0);
            int ok = (READ_ONLY_COMMANDS >> index) & 1;
            if (ok) {
                assert(h.command == ((index | flags[f]) | START) && h.state == SUNXI_MMC_READY);
                ++allowed;
            } else {
                /* A refused index leaves the previous command word and faults the core. */
                assert(h.command == 0x1234 && h.state != SUNXI_MMC_READY);
                ++refused;
            }
        }
    assert(allowed == 17 * 5 && refused == 47 * 5);
    for (unsigned forbidden = 0; forbidden < 3; ++forbidden) {
        uint32_t index = forbidden == 0 ? 24 : forbidden == 1 ? 25 : 38;
        assert(!((READ_ONLY_COMMANDS >> index) & 1));
    }
    puts("sd-read-only funnel passed");
    return 0;
}
