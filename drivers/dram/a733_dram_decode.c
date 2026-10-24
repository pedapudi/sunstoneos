/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_dram.h"
/* No pinned source documents the DRAMC Common, controller, SMC0 or SMC1
 * registers that would show a linear decode to 0x3_4000_0000 with no
 * protected region in 0x8000_0000-0xFFFF_FFFF. Until a trace of boot0's DRAM
 * initialization names them and an EL3 board read matches the trace, the
 * check makes no access and refuses (docs/designs/dram-bus-priority.md). */
int a733_dram_decode_check(void)
{
    return 0;
}
