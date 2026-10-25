/* SPDX-License-Identifier: Apache-2.0
 * Unit harness of the vendor-state PMIC provider copy, run by
 * vendor-state-units.py. This unit includes the copy, so its static
 * writable() is called directly: vsu_writable on the rail and target of
 * vsu_supply (vendor-state-units-supply.c).
 */
#include "vendor-state-pmic.c"
#include "vendor-state-units-supply.h"

uint32_t vsu_writable(void) { return (uint32_t)writable((enum a733_rail)vsu_supply.rail, (uint8_t)vsu_supply.target); }
