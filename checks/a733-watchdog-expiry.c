#include <stdint.h>
#include "a733_rom_return.h"
#include "a733_watchdog.h"
#include "boot.h"

#define MAILBOX ((volatile uint32_t *)0x52800)
#define INPUT 0x03000024UL
extern void watchdog_expiry_wait(void) __attribute__((noreturn));

/* Command 1 exercises enable/disarm; command 2 intentionally waits for reset.
 * Both require a protected physical boot input and exclusive register ownership.
 */
void watchdog_expiry(void)
{
    volatile uint32_t *r = MAILBOX;
    if (r[0] != 0x57445850 || (r[1] != 1 && r[1] != 2) ||
        !r[2] || r[3] != ~r[2]) return;
    uint32_t command = r[1];
    r[4] = 0x57525032;
    r[5] = 2; /* Refused before ownership. */
    r[6] = 1;
    if ((mmio_read(0x02050014) & 1) ||
        mmio_read(0x070501c0) || mmio_read(0x070501d4)) return;
    for (unsigned i = 0; i < 4; ++i)
        if (mmio_read(INPUT) & 0x100) return;

    struct a733_rom_return redirect;
    struct a733_watchdog watchdog;
    redirect.owned = 0;
    watchdog.armed = 0;
    r[5] = 3;
    if (!a733_rom_return_prepare(&redirect)) goto restore;
    r[6] = 2;
    if (!a733_watchdog_arm(&watchdog, 4)) goto restore;
    r[6] = 3;
    r[7] = mmio_read(0x02050014) & 0xffff;
    if (command == 2) {
        r[5] = 0;
        r[6] = 4;
        __asm__ volatile("dsb sy" ::: "memory");
        watchdog_expiry_wait();
    }
    r[5] = 0;
restore:
    if (watchdog.armed && !a733_watchdog_disarm(&watchdog)) r[5] = 4;
    /* Retain redirect while disabling the timer is uncertain. */
    if (!(mmio_read(0x02050014) & 1)) {
        if (redirect.owned && !a733_rom_return_restore(&redirect)) r[5] = 4;
    } else r[5] = 4;
    r[8] = redirect.owned;
    r[9] = watchdog.armed;
    r[6] = 5;
}
