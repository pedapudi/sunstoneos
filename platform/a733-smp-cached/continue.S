#include "a733_cpu_continue.h"
#include "a733_cpu_coherence.h"
.section .text.a733_cpu_continue,"ax"
.balign 4
.global a733_cpu_secondary_continue
.type a733_cpu_secondary_continue,%function
a733_cpu_secondary_continue:
    mrs x4, CurrentEL
    cmp x4, #12
    b.ne .Ldead
    msr daifset, #15
    mrs x4, sctlr_el3
    mov x5, #0x1005
    movk x5, #0x0200, lsl #16
    tst x4, x5
    b.ne .Lpark
    adrp x4, a733_cached_el3_vectors
    add x4, x4, :lo12:a733_cached_el3_vectors
    msr vbar_el3, x4
    msr tpidr_el3, xzr
    mov x4, #0x3600
    movk x4, #0x016e, lsl #16
    msr cntfrq_el0, x4
    msr cntps_ctl_el1, xzr
    msr cntp_ctl_el0, xzr
    msr cntv_ctl_el0, xzr
    isb
    bl a733_cpu_continue_boot
    b .Lpark
.size a733_cpu_secondary_continue,.-a733_cpu_secondary_continue

// Primary uses its existing reserved EL3 boot stack.
.global a733_cpu_primary_continue
.type a733_cpu_primary_continue,%function
a733_cpu_primary_continue:
    mrs x4, CurrentEL
    cmp x4, #12
    b.ne .Ldead
    msr daifset, #15
    mrs x4, sctlr_el3
    mov x5, #0x1005
    movk x5, #0x0200, lsl #16
    tst x4, x5
    b.ne .Lpark
    adrp x4, a733_cached_el3_vectors
    add x4, x4, :lo12:a733_cached_el3_vectors
    msr vbar_el3, x4
    msr tpidr_el3, xzr
    mov x4, #0x3600
    movk x4, #0x016e, lsl #16
    msr cntfrq_el0, x4
    msr cntps_ctl_el1, xzr
    msr cntp_ctl_el0, xzr
    msr cntv_ctl_el0, xzr
    isb
    bl a733_cpu_continue_primary_boot
    b .Lpark
.size a733_cpu_primary_continue,.-a733_cpu_primary_continue

.global a733_cached_enter_el1
.type a733_cached_enter_el1,%function
a733_cached_enter_el1:
    // x0 core, x1 epoch, x2 startup ticks, x3 diagnostic ticks, x4 redistributor.
    // All EL3 participants already use canonical tables.
    mov x20, x0
    mov x21, x1
    mov x22, x2
    mov x23, x3
    mov x24, x4
    mov x2, #15
    msr icc_sre_el3, x2
    isb
    mrs x3, icc_sre_el3
    and x3, x3, #15
    cmp x3, x2
    b.ne a733_cached_el3_fault
    msr icc_ctlr_el3, xzr
    msr icc_igrpen0_el1, xzr
    msr icc_igrpen1_el3, xzr
    // With SCR.FIQ=1, NS PMR writes are ignored while the Secure mask is
    // below 0x80. Initialize its Secure view before entering Nonsecure EL1.
    mov x2, #255
    msr icc_pmr_el1, x2
    isb
    mrs x3, icc_pmr_el1
    tbz x3, #7, a733_cached_el3_fault
    // Architectural RES1 bits4/5 accompany NS, FIQ routing, HCE and RW.
    mov x2, #0x535
    msr scr_el3, x2
    mov x2, #0x10000000
    msr mdcr_el3, x2
    msr cptr_el3, xzr
    isb
    mov x2, #15
    msr icc_sre_el2, x2
    isb
    msr icc_sre_el1, x2
    isb
    mrs x3, icc_sre_el1
    tbz x3, #0, a733_cached_el3_fault
    mov x2, #0x80000000
    msr hcr_el2, x2
    msr hstr_el2, xzr
    msr mdcr_el2, xzr
    msr vttbr_el2, xzr
    mov x2, #0x33ff
    msr cptr_el2, x2
    mov x2, #3
    msr cnthctl_el2, x2
    msr cntvoff_el2, xzr
    mov x2, #0x0830
    movk x2, #0x30c5, lsl #16
    msr sctlr_el2, x2
    mov x2, #0x0800
    movk x2, #0x30d0, lsl #16
    // FreeRTOS recursive locks execute WFE at EL0 under contention.
    // SCTLR_EL1.nTWE permits that wait without an EL1 trap (EC=0x01).
    orr x25, x2, #(1 << 18)
    msr tpidr_el1, x20
    adrp x2, a733_cpu_el1_stacks
    add x2, x2, :lo12:a733_cpu_el1_stacks
    add x3, x20, #1
    add x2, x2, x3, lsl #15
    msr sp_el1, x2
    adrp x2, a733_cached_cold_vectors
    add x2, x2, :lo12:a733_cached_cold_vectors
    msr vbar_el1, x2
    mov x2, #(3 << 20)
    msr cpacr_el1, x2
    mov x2, #3
    msr cntkctl_el1, x2
    adr x2, .Lel1
    msr elr_el3, x2
    mov x2, #0x3c5
    msr spsr_el3, x2
    dsb sy
    isb
    msr sctlr_el1, x25
    isb
    // Restore TCR last, with M/C/I off until the stackless EL1 helper.
    mov x2, #(A733_CPU_COHERENCE_TCR & 0xffff)
    movk x2, #((A733_CPU_COHERENCE_TCR >> 16) & 0xffff), lsl #16
    movk x2, #((A733_CPU_COHERENCE_TCR >> 32) & 0xffff), lsl #32
    msr tcr_el1, x2
    isb
    eret
.Lel1:
    // No stack or ordinary RAM access until enable and readback succeed.
    adrp x0, a733_cached_el1_l1
    add x0, x0, :lo12:a733_cached_el1_l1
    mov x1, x22
    bl a733_cpu_coherence_enable_el1
    cmp w0, #1
    b.ne a733_cached_cold_fault
    adrp x2, a733_cached_el1_vectors
    add x2, x2, :lo12:a733_cached_el1_vectors
    msr vbar_el1, x2
    isb
    msr icc_ctlr_el1, xzr
    msr icc_bpr1_el1, xzr
    mov x2, #255
    msr icc_pmr_el1, x2
    mov x2, #1
    msr icc_igrpen1_el1, x2
    isb
    mov x0, x20
    mov x1, x21
    mov x2, x22
    mov x3, x23
    mov x4, x24
    bl a733_cached_el1
    b a733_cached_el1_fault
.size a733_cached_enter_el1,.-a733_cached_enter_el1

/* Workaround ordering and BHB loop adapted from Trusted Firmware-A.
 * Copyright (c) 2017-2026, Arm Limited and Contributors. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
.macro block_walks
    mrs x16, tcr_el1
    orr x16, x16, #(1 << 7)
    orr x16, x16, #(1 << 23)
    msr tcr_el1, x16
    isb
    mrs x16, sctlr_el1
    orr x16, x16, #1
    msr sctlr_el1, x16
    isb
.endm
.macro clear_bhb
    mov x16, #24
1:  b 2f
2:  subs x16, x16, #1
    b.ne 1b
    dsb sy
    isb
.endm
// Secure instruction fetches from NS-mapped immutable code require SIF=0.
.global a733_cached_allow_ns_code
.type a733_cached_allow_ns_code,%function
a733_cached_allow_ns_code:
    mrs x1, CurrentEL
    cmp x1, #12
    b.ne 1f
    mrs x1, scr_el3
    bic x1, x1, #(1 << 9)
    msr scr_el3, x1
    isb
    mrs x2, scr_el3
    cmp x2, x1
    b.ne 1f
    mov w0, #1
    ret
1:  mov w0, #0
    ret
.size a733_cached_allow_ns_code,.-a733_cached_allow_ns_code
.global a733_cached_block_walks
a733_cached_block_walks:
    block_walks
    ret
.global a733_cached_smc
a733_cached_smc:
    msr daifset, #15
    smc #0
    b .Lpark
.global a733_cached_cold_fault
a733_cached_cold_fault:
    clear_bhb
    // Both pre-enable and uncertain post-enable failures use no EL1 memory.
    msr daifset, #15
    mov x0, #4
    mrs x1, esr_el1
    smc #0
    b .Lpark
.global a733_cached_el1_fault
a733_cached_el1_fault:
    b a733_cached_cold_fault
.global a733_cached_el3_fault
a733_cached_el3_fault:
    msr daifset, #15
    mov x20, x0
    mov x21, x1
    clear_bhb
    block_walks
    mrs x0, mpidr_el1
    mov x1, #0xc1000000
    orr x1, x1, #0x700
    bic x1, x0, x1
    cbnz x1, .Lpark
    ubfx x19, x0, #8, #3
    adrp x1, a733_cached_records
    add x1, x1, :lo12:a733_cached_records
    add x1, x1, x19, lsl #7
    ldr x2, [x1, #16]
    cbz x2, .Lpark
    cmp x2, #3
    b.hi .Lpark
    str xzr, [x1, #16]
    dsb sy
    ldr x2, [x1, #8]
    cmp x2, x19
    b.ne .Lpark
    mrs x2, esr_el3
    str x2, [x1, #40]
    lsr x3, x2, #26
    cmp x18, #8
    b.ne .Lexception_report
    cmp x3, #0x17
    b.eq .Lsmc_report
.Lexception_report:
    mov x20, #4
    mov x21, x2
.Lsmc_report:
    mrs x2, far_el3
    str x2, [x1, #48]
    mrs x2, elr_el3
    str x2, [x1, #56]
    str x21, [x1, #24]
    mov x2, #6
    cbnz x20, 1f
    mov x2, #4
1:  dsb sy
    str x2, [x1, #16]
    dsb sy
    cbnz x19, .Lpark
    mov x0, x20
    mov x1, x21
    bl a733_smp_stop
.Lpark:
    wfe
    b .Lpark
.Ldead:
    b .Ldead

.balign 2048
.global a733_cached_el3_vectors
a733_cached_el3_vectors:
.set slot, 0
.rept 16
    mov x18, #slot
    b a733_cached_el3_fault
    .space 120
.set slot, slot+1
.endr
.balign 2048
.global a733_cached_cold_vectors
a733_cached_cold_vectors:
.rept 16
    b a733_cached_cold_fault
    .space 124
.endr
.balign 2048
.global a733_cached_el1_vectors
a733_cached_el1_vectors:
.macro vector handler
    b \handler
    .space 124
.endm
// Current EL with SP0 is never an admitted runtime context.
.rept 4
    vector a733_cached_cold_fault
.endr
.rept 2
    vector .Lsync_bhb
    vector .Lirq_bhb
    vector a733_cached_cold_fault
    vector a733_cached_cold_fault
.endr
.rept 4
    vector a733_cached_cold_fault
.endr
.Lirq_bhb:
    stp x16, x17, [sp, #-16]!
    clear_bhb
    ldp x16, x17, [sp], #16
    b FreeRTOS_IRQ_Handler
.Lsync_bhb:
    stp x16, x17, [sp, #-16]!
    clear_bhb
    ldp x16, x17, [sp], #16
    stp x0, x1, [sp, #-16]!
    mrs x0, esr_el1
    lsr x0, x0, #26
    cmp x0, #0x15
    b.ne .Lsync_fault
    mrs x0, esr_el1
    and x0, x0, #0xffff
    cmp x0, #119
    b.eq .Lexit
    ldp x0, x1, [sp], #16
    b FreeRTOS_SWI_Handler
.Lexit:
    ldp x0, x1, [sp], #16
    b a733_smp_exit_privileged
.Lsync_fault:
    ldp x0, x1, [sp], #16
    b a733_cached_cold_fault
.section .note.GNU-stack,"",%progbits
