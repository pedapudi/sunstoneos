#include "a733_cpu_continue.h"
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
    adrp x4, a733_cpu_continue_vectors
    add x4, x4, :lo12:a733_cpu_continue_vectors
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
    adrp x4, a733_cpu_continue_vectors
    add x4, x4, :lo12:a733_cpu_continue_vectors
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

.global a733_cpu_continue_enter_el1
.type a733_cpu_continue_enter_el1,%function
a733_cpu_continue_enter_el1:
    // x0 core, x1 epoch. MMU and caches remain disabled at EL3.
    mov x20, x0
    mov x21, x1
    mov x2, #15
    msr icc_sre_el3, x2
    isb
    mrs x3, icc_sre_el3
    and x3, x3, #15
    cmp x3, x2
    b.ne a733_cpu_continue_el3_fault
    msr icc_ctlr_el3, xzr
    msr icc_igrpen0_el1, xzr
    msr icc_igrpen1_el3, xzr
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
    tbz x3, #0, a733_cpu_continue_el3_fault
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
    msr sctlr_el1, x2
    msr tpidr_el1, x20
    adrp x2, a733_cpu_el1_stacks
    add x2, x2, :lo12:a733_cpu_el1_stacks
    add x3, x20, #1
    add x2, x2, x3, lsl #15
    msr sp_el1, x2
    adrp x2, a733_cpu_continue_el1_vectors
    add x2, x2, :lo12:a733_cpu_continue_el1_vectors
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
    eret
.Lel1:
    mov x2, #0x4400
    msr mair_el1, x2
    mov x2, #0x3019
    movk x2, #0x0080, lsl #16
    movk x2, #2, lsl #32
    msr tcr_el1, x2
    adrp x2, a733_cpu_continue_level1
    add x2, x2, :lo12:a733_cpu_continue_level1
    msr ttbr0_el1, x2
    msr ttbr1_el1, xzr
    dsb sy
    isb
    tlbi vmalle1
    dsb sy
    isb
    mrs x2, sctlr_el1
    orr x2, x2, #1
    msr sctlr_el1, x2
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
    bl a733_cpu_continue_el1
    b a733_cpu_continue_el1_fault
.size a733_cpu_continue_enter_el1,.-a733_cpu_continue_enter_el1

.global a733_cpu_continue_exclusive_probe
.type a733_cpu_continue_exclusive_probe,%function
a733_cpu_continue_exclusive_probe:
    cmp x0, #8
    b.hs .Lexclusive_fail
    adrp x2, a733_cpu_continue_exclusives
    add x2, x2, :lo12:a733_cpu_continue_exclusives
    add x2, x2, x0, lsl #7
    add x5, x0, #1
    mov x6, #64
.Lexclusive_try:
    ldaxr x3, [x2]
    cbnz x3, .Lexclusive_clear
    stlxr w4, x5, [x2]
    cbz w4, .Lexclusive_ok
    subs x6, x6, #1
    b.ne .Lexclusive_try
.Lexclusive_clear:
    clrex
.Lexclusive_fail:
    mov x0, #0
    ret
.Lexclusive_ok:
    dmb sy
    mov x0, #1
    ret
.size a733_cpu_continue_exclusive_probe,.-a733_cpu_continue_exclusive_probe

// Each exception path uses only its core's reserved record and no stack. The
// record's nonzero state admits one diagnostic write; zero prevents recursion.
.macro fault_handler name, level
.global \name
\name:
    msr daifset, #15
    mrs x0, mpidr_el1
    mov x1, #0xc1000000
    orr x1, x1, #0x700
    bic x1, x0, x1
    cbnz x1, .Lpark
    ubfx x0, x0, #8, #3
    adrp x1, a733_cpu_continue_records
    add x1, x1, :lo12:a733_cpu_continue_records
    add x1, x1, x0, lsl #7
    ldr x2, [x1, #16]
    cbz x2, .Lpark
    str xzr, [x1, #16]
    dsb sy
    ldr x2, [x1, #8]
    cmp x2, x0
    b.ne .Lpark
    mrs x2, esr_el\level
    str x2, [x1, #40]
    mrs x2, far_el\level
    str x2, [x1, #48]
    mrs x2, elr_el\level
    str x2, [x1, #56]
    mov x2, #(16+\level)
    str x2, [x1, #24]
    dsb sy
    mov x2, #A733_CONTINUE_RETAINED
    str x2, [x1, #16]
    dsb sy
    sev
    b .Lpark
.endm
fault_handler a733_cpu_continue_el3_fault, 3
fault_handler a733_cpu_continue_el1_fault, 1
.Lpark:
    wfe
    b .Lpark
.Ldead:
    b .Ldead

.balign 2048
.global a733_cpu_continue_vectors
a733_cpu_continue_vectors:
.rept 16
    b a733_cpu_continue_el3_fault
    .space 124
.endr
.balign 2048
.global a733_cpu_continue_el1_vectors
a733_cpu_continue_el1_vectors:
.macro vector handler
    b \handler
    .space 124
.endm
.rept 3
    vector .Lsync
    vector FreeRTOS_IRQ_Handler
    vector a733_cpu_continue_el1_fault
    vector a733_cpu_continue_el1_fault
.endr
.rept 4
    vector a733_cpu_continue_el1_fault
.endr
.Lsync:
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
    b a733_cpu_continue_el1_fault
.section .note.GNU-stack,"",%progbits
