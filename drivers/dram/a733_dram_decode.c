/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_dram.h"
/* No pinned source documents the DRAMC Common, controller, SMC0 or SMC1
 * registers that would show a linear decode of the listed ranges up to
 * 0x3_4000_0000 with no protected region in them. Until a trace of boot0's
 * DRAM initialization names them and an EL3 board read matches the trace,
 * the check makes no access and refuses (docs/designs/dram-bus-priority.md). */
int a733_dram_decode_check(const struct a733_dram_range *ranges, unsigned count)
{
    (void)ranges; (void)count;
    return 0;
}
