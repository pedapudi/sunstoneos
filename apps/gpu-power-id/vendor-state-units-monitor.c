// SPDX-License-Identifier: Apache-2.0
// Unit harness of the vendor-state monitor copy, run by
// vendor-state-units.py with the monitor's own compile flags. This unit
// includes the copy, so vsu_el3 calls its static vendor_state_el3_setup()
// directly, as monitor_init does last.
#include "vendor-state-monitor.c"

void vsu_el3(void) { vendor_state_el3_setup(); }
