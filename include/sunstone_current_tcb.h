/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_CURRENT_TCB_H
#define SUNSTONE_CURRENT_TCB_H
/* The kernel that defines pxCurrentTCBs sets its layout, and every unit of
 * an image indexes the array with portCURRENT_TCB(core) in C or by shifting
 * the core ID left by portCURRENT_TCB_SHIFT in assembly.
 *
 * - The SunstoneOS kernels (FreeRTOS tasks.c with patches/freertos-smp-tasks.patch,
 *   and the control and partitioned schedulers built on it) give each core's
 *   entry its own 64-byte cache line, the line size of the A733's Cortex-A55
 *   and Cortex-A76 cores. Every context switch writes the switching core's
 *   entry, so with eight entries on one line a real-time core's switch took
 *   the line from whichever core switched last, and from DRAM when that core
 *   was streaming. An image with such a kernel compiles every unit with
 *   portCURRENT_TCB_SHIFT=6; the build adds it to each target that compiles
 *   the patched tasks.c.
 * - The unmodified FreeRTOS tasks.c, which the emulator models of the device
 *   services and some bring-up images use, keeps 8-byte entries; the default
 *   shift is 3.
 *
 * A mismatched pair cannot link. The kernel defines the absolute symbol
 * ss_current_tcb_layout_<shift> and references ss_current_tcb_port_<shift>;
 * the port (portASM.S) defines ss_current_tcb_port_<shift> and references
 * ss_current_tcb_layout_<shift>. The unmodified tasks.c defines no layout
 * symbol, so a port with shift 3 supplies a weak ss_current_tcb_layout_3.
 * Both references sit in retained data, which --gc-sections keeps.
 * checks/current-tcb-lines.py links each pairing. */
#define portCURRENT_TCB_LINE_SHIFT 6
#define portCURRENT_TCB_LINE_BYTES 64
#ifndef portCURRENT_TCB_SHIFT
#define portCURRENT_TCB_SHIFT 3
#endif
#define portCURRENT_TCB_BYTES ( 1 << portCURRENT_TCB_SHIFT )
#define portCURRENT_TCB_GUARD_NAME_( kind, shift ) ss_current_tcb_##kind##_##shift
#define portCURRENT_TCB_GUARD_NAME( kind, shift ) portCURRENT_TCB_GUARD_NAME_( kind, shift )
#define portCURRENT_TCB_LAYOUT_SYMBOL portCURRENT_TCB_GUARD_NAME( layout, portCURRENT_TCB_SHIFT )
#define portCURRENT_TCB_PORT_SYMBOL portCURRENT_TCB_GUARD_NAME( port, portCURRENT_TCB_SHIFT )
#if defined(__ASSEMBLER__)
/* For portASM.S: define this port's symbol and reference the kernel's. */
.macro portCURRENT_TCB_PORT_GUARD
   .globl portCURRENT_TCB_PORT_SYMBOL
   .set portCURRENT_TCB_PORT_SYMBOL, portCURRENT_TCB_SHIFT
#if portCURRENT_TCB_SHIFT == 3
   .weak ss_current_tcb_layout_3
   .set ss_current_tcb_layout_3, 3
#endif
   .pushsection .rodata.ss_current_tcb_port_guard, "aR"
   .balign 8
   .quad portCURRENT_TCB_LAYOUT_SYMBOL
   .popsection
.endm
#else
#define portCURRENT_TCB_STRIDE ( portCURRENT_TCB_BYTES / sizeof( void * ) )
#define portCURRENT_TCB( xCoreID ) pxCurrentTCBs[ ( xCoreID ) * portCURRENT_TCB_STRIDE ]
#define portCURRENT_TCB_TEXT_( x ) #x
#define portCURRENT_TCB_TEXT( x ) portCURRENT_TCB_TEXT_( x )
/* For the kernel that defines pxCurrentTCBs with one line per core: define
 * the layout symbol and reference the port's. It refuses to compile with
 * any other shift. */
#define portCURRENT_TCB_KERNEL_GUARD() \
    _Static_assert( portCURRENT_TCB_SHIFT == portCURRENT_TCB_LINE_SHIFT, "a SunstoneOS kernel needs portCURRENT_TCB_SHIFT=6" ); \
    __asm__( ".globl " portCURRENT_TCB_TEXT( portCURRENT_TCB_LAYOUT_SYMBOL ) "\n" \
             ".set " portCURRENT_TCB_TEXT( portCURRENT_TCB_LAYOUT_SYMBOL ) ", " portCURRENT_TCB_TEXT( portCURRENT_TCB_SHIFT ) "\n" \
             ".pushsection .rodata.ss_current_tcb_kernel_guard, \"aR\"\n" \
             ".balign 8\n" \
             ".quad " portCURRENT_TCB_TEXT( portCURRENT_TCB_PORT_SYMBOL ) "\n" \
             ".popsection" )
/* Runs on a core right after it stores its new current task; idle is
 * nonzero when that task is an idle task. A diagnostic may keep its own
 * per-core copy, so that another core reads that copy instead of this
 * core's entry or the idle-task handles (apps/latency/workload.c). */
#ifndef traceCURRENT_TCB_SET
#define traceCURRENT_TCB_SET( xCoreID, pxTCB, xIdle )
#endif
#endif
#endif
