/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_CURRENT_TCB_H
#define SUNSTONE_CURRENT_TCB_H
/* Each core's entry of the SMP kernel's pxCurrentTCBs fills its own 64-byte
 * cache line (the line size of the A733's Cortex-A55 and Cortex-A76 cores).
 * Every context switch writes the switching core's entry, so with eight
 * entries on one line a real-time core's switch took the line from whichever
 * core switched last, and from DRAM when that core was streaming. Assembly
 * indexes the array with the core ID shifted left by portCURRENT_TCB_SHIFT;
 * C uses portCURRENT_TCB(core). checks/current-tcb-lines.py fails a build or
 * source in which two cores' entries can share a line. */
#define portCURRENT_TCB_SHIFT 6
#define portCURRENT_TCB_BYTES 64
#if !defined(__ASSEMBLER__)
#define portCURRENT_TCB_STRIDE ( portCURRENT_TCB_BYTES / sizeof( void * ) )
#define portCURRENT_TCB( xCoreID ) pxCurrentTCBs[ ( xCoreID ) * portCURRENT_TCB_STRIDE ]
_Static_assert( ( 1 << portCURRENT_TCB_SHIFT ) == portCURRENT_TCB_BYTES, "shift and line size agree" );
/* Runs on a core right after it stores its new current task. A diagnostic
 * may keep its own per-core copy, so that another core reads that copy
 * instead of this core's entry (apps/latency/workload.c). */
#ifndef traceCURRENT_TCB_SET
#define traceCURRENT_TCB_SET( xCoreID, pxTCB )
#endif
#endif
#endif
