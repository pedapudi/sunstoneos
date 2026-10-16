#include "a733_watchdog.h"
#include "boot.h"

#define WDT 0x02050000UL
#define FEL_FLAG 0x07090108UL
#define KEY 0x16aa0000U

int a733_watchdog_arm(struct a733_watchdog *watchdog, unsigned seconds)
{
    unsigned interval;
    switch (seconds) {
    case 1: case 2: case 3: case 4: case 5: case 6:
        interval = seconds; break;
    case 8: interval = 7; break;
    case 10: interval = 8; break;
    case 12: interval = 9; break;
    case 14: interval = 10; break;
    case 16: interval = 11; break;
    default: return 0;
    }
    if (watchdog->armed) return 0;
    watchdog->mode = mmio_read(WDT + 0x14) & 0xffff;
    if (watchdog->mode & 1) return 0; // Do not take another owner's watchdog.
    watchdog->configuration = mmio_read(WDT + 0x10) & 0xffff;
    watchdog->output = mmio_read(WDT + 0x18) & 0xffff;
    watchdog->fel_flag = mmio_read(FEL_FLAG);
    mmio_write(FEL_FLAG, 0x5aa5a55a);
    if (mmio_read(FEL_FLAG) != 0x5aa5a55a) {
        mmio_write(FEL_FLAG, watchdog->fel_flag);
        return 0;
    }
    // The A733 bootloader uses this reset pulse setting to avoid resetting
    // the power controller. It still resets the SoC, including DDR control.
    mmio_write(WDT + 0x18, KEY | 2);
    mmio_write(WDT + 0x10, KEY | (watchdog->configuration & ~3U) | 1);
    uint32_t mode = (watchdog->mode & ~0xf1U) | (interval << 4) | 1;
    mmio_write(WDT + 0x14, KEY | mode);
    mmio_write(WDT + 0x0c, 0x14af);
    watchdog->armed = 1;
    if ((mmio_read(WDT + 0x14) & 0xf1) != (mode & 0xf1) ||
        (mmio_read(WDT + 0x10) & 3) != 1 ||
        (mmio_read(WDT + 0x18) & 0xffff) != 2) {
        (void)a733_watchdog_disarm(watchdog);
        return 0;
    }
    return 1;
}

int a733_watchdog_disarm(struct a733_watchdog *watchdog)
{
    if (!watchdog->armed) return 0;
    mmio_write(WDT + 0x14, KEY | watchdog->mode);
    if (mmio_read(WDT + 0x14) & 1) return 0;
    mmio_write(WDT + 0x10, KEY | watchdog->configuration);
    mmio_write(WDT + 0x18, KEY | watchdog->output);
    mmio_write(FEL_FLAG, watchdog->fel_flag);
    if ((mmio_read(WDT + 0x14) & 0xffff) != watchdog->mode ||
        (mmio_read(WDT + 0x10) & 0xffff) != watchdog->configuration ||
        (mmio_read(WDT + 0x18) & 0xffff) != watchdog->output ||
        mmio_read(FEL_FLAG) != watchdog->fel_flag) return 0;
    watchdog->armed = 0;
    return 1;
}

void a733_watchdog_feed(const struct a733_watchdog *watchdog)
{
    if (watchdog->armed) mmio_write(WDT + 0x0c, 0x14af);
}
