#ifndef SUNSTONE_A733_WATCHDOG_H
#define SUNSTONE_A733_WATCHDOG_H
#include <stdint.h>

/* Exclusive early-boot ownership; zero-initialize the context before use.
 * Hardware reset and ROM return are untested.
 * Expiry resets the SoC and requests FEL through the volatile RTC flag.
 * Treat DDR as uninitialized after expiry; the normal RAM return contract
 * does not apply to a watchdog system reset.
 */
struct a733_watchdog {
    uint32_t mode, configuration, output, fel_flag;
    int armed;
};
int a733_watchdog_arm(struct a733_watchdog *watchdog, unsigned seconds);
int a733_watchdog_disarm(struct a733_watchdog *watchdog);
void a733_watchdog_feed(const struct a733_watchdog *watchdog);
#endif
