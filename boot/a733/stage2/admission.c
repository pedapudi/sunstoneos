/* Cold SD admission for the stage-2 loader. No evidence exists yet: the
 * controller clock, pins, card supplies, presence and exclusive ownership
 * that boot0 leaves are unknown, and this monitor has no fixed-board subset
 * of the shared clock, pin, rail and PMIC providers. The refusal happens
 * before any controller access. A separate unit keeps the decision out of
 * the loader's optimization, so the instruction model can substitute
 * modeled evidence at this one call. */
#include <stdint.h>

int a733_stage2_card_admission(uint32_t module_hz);

int a733_stage2_card_admission(uint32_t module_hz)
{
    (void)module_hz;
    return 0;
}
