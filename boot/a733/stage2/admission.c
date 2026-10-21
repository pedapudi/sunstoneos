/* Cold SD admission for the stage-2 loader. No evidence exists yet: the
 * controller clock, pins, card supplies, presence and exclusive ownership
 * that boot0 leaves are unknown, and this monitor has no fixed-board subset
 * of the shared clock, pin, rail and PMIC providers. The refusal happens
 * before any controller access. A separate unit keeps the decision out of
 * the loader's optimization, so the instruction model can substitute
 * modeled evidence at these calls. */
#include <stdint.h>

int a733_stage2_card_admission(uint32_t module_hz);
int a733_stage2_card_present(void);
int a733_stage2_entry_admission(void);

int a733_stage2_card_admission(uint32_t module_hz)
{
    (void)module_hz;
    return 0;
}

/* The loader's retained() callback asks this before every SD core poll.
 * Presence must come from the active-low PF6 card-detect pin, read under the
 * same retained pin ownership that admission establishes (docs/mmc.md).
 * An admission that accepts must also replace this refusal. */
int a733_stage2_card_present(void)
{
    return 0;
}

/* Prerequisites for entering a verified image, checked after loading and
 * before the watchdog is armed or any launch authority is built. None is
 * established, so entry is refused:
 * - Remote-writer exclusion: no stop or drain procedure exists for the E902
 *   core or other DMA-capable agents, and the boot0 staging area at
 *   0x42e00000 has no owner (docs/cold-entry.md).
 * - Watchdog recovery: a watchdog expiry after a cold boot has not been shown
 *   to restart the board into this monitor with both cards preserved.
 * - Entry state: the CPU, GIC and timer state that boot0 leaves at EL3 has
 *   not been compared with the reset state that FEL-launched images see. */
int a733_stage2_entry_admission(void)
{
    return 0;
}
