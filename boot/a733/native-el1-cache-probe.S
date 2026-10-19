/* Returning core-1 diagnostic. The production entry/helper execute unchanged;
 * the checkpoint replaces the first EL1 interrupt-interface configuration.
 * EL1 executes no data stores. EL3 stack/report accesses remain Device memory.
 * This is not a general cache shutdown or a live context switch. */
.section .text.native_el1_cache_probe,"ax"
.balign 4
.global native_el1_cache_probe, el1_observe_checkpoint, el1_return_checkpoint
.global el1_smc_return
.type native_el1_cache_probe,%function
native_el1_cache_probe:
#ifdef SUNSTONE_SECONDARY_INTERRUPTS
    stp x19, x20, [sp, #-112]!
    // The frozen production entry predates Secure PMR initialization. Mirror
    // the corrected entry, saving its original value on the Device stack.
    mrs x2, icc_pmr_el1
    str x2, [sp, #96]
    mov x2, #255
    msr icc_pmr_el1, x2
    isb
    mrs x2, icc_pmr_el1
    tbz x2, #7, .Lel1_park
#else
    stp x19, x20, [sp, #-96]!
#endif
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
#ifdef SUNSTONE_SECONDARY_CACHE
    mrs x2, mpidr_el1
    ubfx x2, x2, #8, #3
    ldr x19, =0x4201a800
    add x19, x19, x2, lsl #8
#else
    ldr x19, =0x42002b20
#endif
    ldr x2, [x19]
    cbnz x2, .Lel1_park
    stp x0, x1, [x19, #8]
    mov x2, sp
    str x2, [x19, #24]
    mrs x2, sctlr_el1
    mrs x3, tcr_el1
    stp x2, x3, [x19, #32]
    mrs x2, ttbr0_el1
    mrs x3, ttbr1_el1
    stp x2, x3, [x19, #48]
    mrs x2, mair_el1
    str x2, [x19, #64]
    mov x2, #1
    str x2, [x19]
    mov x2, x1
    mov x3, x1
    mov x1, x0
#ifdef SUNSTONE_SECONDARY_CACHE
    mrs x0, mpidr_el1
    ubfx x0, x0, #8, #3
    ldr x4, =0x03460000
    add x4, x4, x0, lsl #17
#else
    mov x0, #1
    ldr x4, =0x03480000
#endif
    dsb sy
    bl probe_production_enter_el1
    b .Lel1_park
.size native_el1_cache_probe,.-native_el1_cache_probe

el1_observe_checkpoint:
#ifdef SUNSTONE_SECONDARY_COHERENCE
    bl identity_coherence_el1
#endif
    mov x0, #0x733
    mrs x1, CurrentEL
    mrs x2, sctlr_el1
    mrs x3, tcr_el1
    mrs x4, ttbr0_el1
    mrs x5, mair_el1
    mrs x6, ttbr1_el1
    mov x7, sp
    mrs x8, tpidr_el1
    mrs x9, cntfrq_el0
    mrs x10, mpidr_el1
    mrs x11, icc_igrpen1_el1
    smc #0x733
el1_smc_return:
    b el1_smc_return

el1_return_checkpoint:
    msr daifset, #15
    // Match the production BHB/Speculative-AT exception-entry ordering.
    // AArch64 EL1 translation controls have one copy, not security banks.
    mov x16, #24
.Lbhb:
    b .Lbhb_next
.Lbhb_next:
    subs x16, x16, #1
    b.ne .Lbhb
    mrs x16, tcr_el1
    orr x16, x16, #(1 << 7)
    orr x16, x16, #(1 << 23)
    msr tcr_el1, x16
    isb
    mrs x16, sctlr_el1
    orr x16, x16, #1
    msr sctlr_el1, x16
    isb
    mrs x16, mpidr_el1
    ubfx x16, x16, #8, #3
#ifdef SUNSTONE_SECONDARY_CACHE
#ifdef SUNSTONE_PRIMARY_EL1
    cbnz x16, .Lel1_park
#else
    cbz x16, .Lel1_park
#endif
    ldr x19, =0x4201a800
    add x19, x19, x16, lsl #8
#else
    cmp x16, #1
    b.ne .Lel1_park
    ldr x19, =0x42002b20
#endif
#ifdef SUNSTONE_SECONDARY_COHERENCE
    // The stackless EL1 interval returns only after all writers stop and this
    // target completes NS cache cleaning. Preserve its proof in Device memory.
    cmp x25, #1
    b.ne .Lel1_refuse
    ldr x17, =0x4201b000
    add x17, x17, x16, lsl #6
    stp x20, x21, [x17, #8]
    stp x26, x27, [x17, #24]
    stp x28, x29, [x17, #40]
    mov x16, #3
    dsb sy
    str x16, [x17]
#endif
    stp x0, x1, [x19, #72]
    stp x2, x3, [x19, #88]
    stp x4, x5, [x19, #104]
    stp x6, x7, [x19, #120]
    stp x8, x9, [x19, #136]
    stp x10, x11, [x19, #152]
    mrs x12, esr_el3
    mrs x13, elr_el3
    mrs x14, spsr_el3
    stp x12, x13, [x19, #168]
    str x14, [x19, #184]
    str x18, [x19, #240]
    mrs x15, CurrentEL
    str x15, [x19, #248]
    ldr x16, [x19]
    cmp x16, #1
    b.ne .Lel1_refuse
    ldr x16, [x19, #8]
    cmp x16, x21
    b.ne .Lel1_refuse
    ldr x16, [x19, #24]
    mov x17, sp
    cmp x16, x17
    b.ne .Lel1_refuse
    cmp x18, #8
    b.ne .Lel1_refuse
    cmp x15, #12
    b.ne .Lel1_refuse
    cmp x0, #0x733
    b.ne .Lel1_refuse
    cmp x1, #4
    b.ne .Lel1_refuse
    ldr x16, =0x30d01805
    cmp x2, x16
    b.ne .Lel1_refuse
    ldr x16, =0x200803019
    cmp x3, x16
    b.ne .Lel1_refuse
#ifdef SUNSTONE_SECONDARY_CACHE
    ldr x16, =probe_production_el1_root
#else
    ldr x16, =0x421f2000
#endif
    cmp x4, x16
    b.ne .Lel1_refuse
    ldr x16, =0x44ff00
    cmp x5, x16
    b.ne .Lel1_refuse
    cbnz x6, .Lel1_refuse
#ifdef SUNSTONE_SECONDARY_CACHE
    ldr x16, =probe_production_el1_stacks
    add x17, x20, #1
    add x16, x16, x17, lsl #15
#else
    ldr x16, =0x41253000
#endif
    cmp x7, x16
    b.ne .Lel1_refuse
#ifdef SUNSTONE_SECONDARY_CACHE
    cmp x8, x20
#else
    cmp x8, #1
#endif
    b.ne .Lel1_refuse
    ldr x16, =24000000
    cmp x9, x16
    b.ne .Lel1_refuse
#ifdef SUNSTONE_SECONDARY_CACHE
    ldr x16, =0x81000000
    orr x16, x16, x20, lsl #8
#else
    ldr x16, =0x81000100
#endif
    cmp x10, x16
    b.ne .Lel1_refuse
    cbnz x11, .Lel1_refuse
    ldr x16, =0x5e000733  // SMC64, IL=1, matching immediate.
    cmp x12, x16
    b.ne .Lel1_refuse
    adr x16, el1_smc_return
    cmp x13, x16
    b.ne .Lel1_refuse
    and x14, x14, #0x3ff
    cmp x14, #0x3c5
    b.ne .Lel1_refuse
    mov x16, #2
    str x16, [x19]
    // No dirty EL1 data lines exist. Blocked walks remain in force while
    // disabling caches and restoring translation parameters. TCR is last.
    dsb sy
    mrs x16, sctlr_el1
    bic x16, x16, #4
    bic x16, x16, #0x1000
    msr sctlr_el1, x16
    isb
    tlbi vmalle1
    dsb sy
    tlbi vmalle1
    dsb sy
    ldr x16, [x19, #48]
    msr ttbr0_el1, x16
    ldr x16, [x19, #56]
    msr ttbr1_el1, x16
    ldr x16, [x19, #64]
    msr mair_el1, x16
    ldr x16, [x19, #32]
    msr sctlr_el1, x16
    isb
    ldr x16, [x19, #40]
    msr tcr_el1, x16
    isb
.macro verify reg, original, restored
    mrs x16, \reg
    str x16, [x19, #\restored]
    ldr x17, [x19, #\original]
    cmp x16, x17
    b.ne .Lel1_refuse
.endm
    verify sctlr_el1, 32, 192
    verify tcr_el1, 40, 200
    verify ttbr0_el1, 48, 208
    verify ttbr1_el1, 56, 216
    verify mair_el1, 64, 224
#ifdef SUNSTONE_SECONDARY_INTERRUPTS
    ldr x16, [sp, #96]
    msr icc_pmr_el1, x16
    isb
    mrs x17, icc_pmr_el1
    cmp x16, x17
    b.ne .Lel1_refuse
#endif
    mov x16, #3
    dsb sy
    str x16, [x19]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
#ifdef SUNSTONE_SECONDARY_INTERRUPTS
    ldp x19, x20, [sp], #112
#else
    ldp x19, x20, [sp], #96
#endif
    ret
.Lel1_refuse:
    mov x16, #1
    str x16, [x19, #232]
    dsb sy
.Lel1_park:
    wfi
    b .Lel1_park
.section .note.GNU-stack,"",%progbits
