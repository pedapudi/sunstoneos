/* SPDX-License-Identifier: Apache-2.0
 * Unit harness of the vendor-state PMIC consumer copy, run by
 * vendor-state-units.py. This unit includes the copy, so its static checks
 * are called directly: vsu_correct is its correct() and vsu_recorded its
 * recorded(), on the arguments in vsu_supply. vendor-state-units-pmic.c
 * holds the provider copy's case, and the resource copy is linked as its
 * own object and reached through its public API in vendor-state-units.c.
 */
#include "vendor-state-consumer.c"
#include "vendor-state-units-supply.h"

volatile struct vsu_supply vsu_supply;

static struct a733_pmic_consumer consumer;
static struct a733_pmic provider;
uint32_t vsu_correct(void)
{
    struct a733_pmic_snapshot s;
    const enum a733_rail rail = (enum a733_rail)vsu_supply.rail;
    if (!a733_pmic_decode(rail, (uint8_t)vsu_supply.enable, (uint8_t)vsu_supply.selector, &s)) return 2;
    s.page_before = (uint8_t)vsu_supply.page_before; s.chip = (uint8_t)vsu_supply.chip;
    s.revision = (uint8_t)vsu_supply.revision; s.page_after = (uint8_t)vsu_supply.page_after;
    consumer.config.rail = rail;
    return (uint32_t)correct(&consumer, &s);
}
/* The image binds the provider to a power-domain owner; the copy still
 * samples DCDC4 itself. */
uint32_t vsu_recorded(void)
{
    provider.domains = (struct a733_power_domains *)0x40180000;
    consumer.config.provider = &provider;
    consumer.config.rail = A733_RAIL_DCDC4;
    return (uint32_t)recorded(&consumer);
}
