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
    // Zero the active priorities before any group is enabled, Group 0 first
    // (Linux gic_cpu_sys_reg_init). SCR_EL3.NS selects the ICC_AP1R0_EL1
    // copy: the Secure copy here, the Nonsecure copy at EL1. The CPU
    // interface has five priority bits, so only the APxR0 registers exist.
    mov x2, #0x534
    msr scr_el3, x2
    isb
    msr icc_ap0r0_el1, xzr
    msr icc_ap1r0_el1, xzr
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
    // MDCR_EL2.HPMN = PMCR_EL0.N gives EL1 every event counter; TPM, TPMCR
    // and HPME stay zero.
    mrs x2, pmcr_el0
    ubfx x2, x2, #11, #5
    msr mdcr_el2, x2
    msr vttbr_el2, xzr
    mov x2, #0x33ff
    msr cptr_el2, x2
    mov x2, #3
    msr cnthctl_el2, x2
    msr cntvoff_el2, xzr
    // EL2 stays unused: EL1 reads of MIDR and MPIDR return VPIDR_EL2 and
    // VMPIDR_EL2, and the EL2 physical timer (PPI 26) is disabled; all three
    // are UNKNOWN after reset (TF-A init_nonsecure_el2_unused).
    mrs x2, midr_el1
    msr vpidr_el2, x2
    mrs x2, mpidr_el1
    msr vmpidr_el2, x2
    msr cnthp_ctl_el2, xzr
    mov x2, #0x0830
    movk x2, #0x30c5, lsl #16
    msr sctlr_el2, x2
    mov x2, #0x0800
    movk x2, #0x30d0, lsl #16
    // FreeRTOS recursive locks execute WFE at EL0 under contention, and
    // idle tasks execute WFI at EL0. SCTLR_EL1.nTWE and nTWI permit those
    // waits without an EL1 trap (EC=0x01).
    orr x25, x2, #(1 << 18)
    orr x25, x25, #(1 << 16)
    msr tpidr_el1, x20
    // TPIDRRO_EL0 is UNKNOWN at reset. A zero task field makes the FreeRTOS
    // port read the core index through SVC until it first resumes a task.
    msr tpidrro_el0, xzr
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
#ifdef SUNSTONE_A733_FAULT_PAGE
    b.ne a733_cached_el1_fault
#else
    b.ne a733_cached_cold_fault
#endif
    adrp x2, a733_cached_el1_vectors
    add x2, x2, :lo12:a733_cached_el1_vectors
    msr vbar_el1, x2
    isb
    msr icc_ctlr_el1, xzr
    msr icc_bpr1_el1, xzr
    mov x2, #255
    msr icc_pmr_el1, x2
    msr icc_ap1r0_el1, xzr
    isb
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
#ifdef SUNSTONE_A733_FAULT_PAGE
    // A deliberate exit: no marker, whatever the status in x0.
    mov x2, #0
#endif
    smc #0
    b .Lpark
// In fault-page builds every EL1 vector entry sets x2 = 0x100 | slot first.
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
#ifdef SUNSTONE_A733_FAULT_PAGE
    // Not a vector entry: recorded as an exit with status 4.
    mov x2, #0
#endif
    b a733_cached_cold_fault
.global a733_cached_el3_fault
a733_cached_el3_fault:
    msr daifset, #15
    mov x20, x0
    mov x21, x1
#ifdef SUNSTONE_A733_FAULT_PAGE
    mov x22, x2
#endif
    clear_bhb
    block_walks
    mrs x0, mpidr_el1
    mov x1, #0xc1000000
    orr x1, x1, #0x700
    bic x1, x0, x1
#ifdef SUNSTONE_A733_FAULT_PAGE
    // Stop watchdog renewal before any check: every entry of every core
    // marks its byte (byte 0 for an MPIDR that names no core).
    ubfx x19, x0, #8, #3
    cmp x1, #0
    csel x2, x19, xzr, eq
    adrp x3, a733_fault_pending
    add x3, x3, :lo12:a733_fault_pending
    mov w0, #1
    strb w0, [x3, x2]
    dsb sy
    cbnz x1, .Lpark
#else
    cbnz x1, .Lpark
    ubfx x19, x0, #8, #3
#endif
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
#ifdef SUNSTONE_A733_FAULT_PAGE
    b.ne .Lexception_report
    // An SMC from EL1: fill this core's retained slot.
    ldr x4, [x1]
    mov x23, x1
    mov x0, x19
    mov x1, x20
    mov x2, x21
    mov x3, x22
    bl a733_fault_capture
    mov x1, x23
    b .Lsmc_report
#else
    b.eq .Lsmc_report
#endif
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
.set slot, 0
.rept 16
#ifdef SUNSTONE_A733_FAULT_PAGE
    mov x2, #(0x100 + slot)
    b a733_cached_cold_fault
    .space 120
#else
    b a733_cached_cold_fault
    .space 124
#endif
.set slot, slot+1
.endr
.balign 2048
.global a733_cached_el1_vectors
a733_cached_el1_vectors:
.set vector_slot, 0
.macro vector handler
#ifdef SUNSTONE_A733_FAULT_PAGE
    .set vector_slot, vector_slot+1
    .ifc \handler, a733_cached_cold_fault
    mov x2, #(0x100 + vector_slot - 1)
    b \handler
    .space 120
    .exitm
    .endif
#endif
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
    // Classify without touching a possibly invalid SP. Exception entry masks
    // DAIF. TPIDR_EL1 holds only the core index, recoverable from MPIDR, so it
    // can preserve x16 until an SVC takes the normal stack-using path.
    msr tpidr_el1, x16
    mrs x16, esr_el1
    lsr x16, x16, #26
    cmp x16, #0x15
    b.ne .Lsync_fault
    mrs x16, tpidr_el1
    stp x16, x17, [sp, #-16]!
    mrs x16, mpidr_el1
    ubfx x16, x16, #8, #3
    msr tpidr_el1, x16
    clear_bhb
    ldp x16, x17, [sp], #16
    stp x0, x1, [sp, #-16]!
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
#ifdef SUNSTONE_A733_FAULT_PAGE
    // The vector slot from SPSR_EL1.M: 8 from EL0t, 0 from EL1t (images
    // whose private tasks run on SP_EL0), 4 from EL1h.
    mrs x16, spsr_el1
    and x16, x16, #0xf
    mov x2, #0x108
    cbz x16, 1f
    mov x2, #0x100
    tbz x16, #0, 1f
    mov x2, #0x104
1:
#endif
    b a733_cached_cold_fault
.section .note.GNU-stack,"",%progbits
