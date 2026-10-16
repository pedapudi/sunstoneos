// Bounded EL3 -> nonsecure EL1 -> EL3 probe. No interrupt controller changes.
.arch armv8-a
.section .text, "ax"
.global _start
_start:
    msr daifset, #0xf
    msr spsel, #1
    ldr x0, =0x41800000
    mov sp, x0
    adr x0, el3_vectors
    msr vbar_el3, x0
    isb
    ldr x20, =0x42000000
    mov x0, #0
1:  str xzr, [x20, x0]
    add x0, x0, #8
    cmp x0, #128
    b.lo 1b
    ldr x0, =0x454c3150
    str x0, [x20]
    mov x0, #1
    str x0, [x20, #8]
    // Board device tree declares a 24 MHz architectural counter.
    ldr x0, =24000000
    msr cntfrq_el0, x0
    mov x0, #0x501              // NS=1, RW=1, HCE=1; SMC remains enabled.
    msr scr_el3, x0
    str x0, [x20, #48]
    msr cptr_el3, xzr
    isb
    mov x0, #(1 << 31)         // EL1 is AArch64; no EL2 traps requested.
    msr hcr_el2, x0
    str x0, [x20, #56]
    mov x0, #3                 // EL1 physical counter and timer access.
    msr cnthctl_el2, x0
    str x0, [x20, #64]
    msr cntvoff_el2, xzr
    mov x0, #0x33ff            // CPTR_EL2 required-one fields; FP trap clear.
    msr cptr_el2, x0
    ldr x0, =0x30d00800        // SCTLR_EL1 required-one fields, MMU/cache off.
    msr sctlr_el1, x0
    ldr x0, =0x41900000
    msr sp_el1, x0
    adr x0, el1_vectors
    msr vbar_el1, x0
    adr x0, el1_entry
    msr elr_el3, x0
    mov x0, #0x3c5             // EL1h, exceptions masked.
    msr spsr_el3, x0
    mov x0, #2
    str x0, [x20, #8]
    dsb sy
    isb
    eret
el1_entry:
    mrs x0, CurrentEL
    str x0, [x20, #16]
    cmp x0, #4
    b.ne el1_failed
    mrs x0, cntfrq_el0
    str x0, [x20, #24]
    ldr x1, =24000000
    cmp x0, x1
    b.ne el1_failed
    mrs x0, cntpct_el0
    str x0, [x20, #32]
    mrs x0, sctlr_el1
    str x0, [x20, #40]
    mov x0, #3
    str x0, [x20, #8]
    mov x1, #0
    b el1_return
el1_failed:
    mov x1, #1
el1_return:
    mov x0, #0x733
    dsb sy
    smc #0
    b el1_failed
el1_exception:
    mrs x0, esr_el1
    str x0, [x20, #112]
    mrs x0, elr_el1
    str x0, [x20, #120]
    b el1_failed
el3_exception:
    // All unexpected exceptions also return through the known FEL reset path.
    ldr x20, =0x42000000
    mrs x2, esr_el3
    str x2, [x20, #72]
    mrs x3, elr_el3
    str x3, [x20, #80]
    mrs x3, far_el3
    str x3, [x20, #88]
    lsr x2, x2, #26
    cmp x2, #0x17              // SMC64.
    b.ne el3_failed
    cmp x0, #0x733
    b.ne el3_failed
    str x1, [x20, #96]
    mov x0, #4
    str x0, [x20, #8]
    b recover
el3_failed:
    mov x1, #2
    str x1, [x20, #96]
recover:
    mrs x0, cntpct_el0
    str x0, [x20, #104]
    dsb sy
#ifdef QEMU_CHECK
    // QEMU validates the same handoff; board recovery remains a physical check.
    sub sp, sp, #16
    ldr x0, =0x20026
    stp x0, x1, [sp]
    mov x1, sp
    mov x0, #0x20
    hlt #0xf000
#else
    ldr x0, =0x08001004
    mov w1, #0x20
    str w1, [x0]
    str wzr, [x0, #4]
    dsb sy
    mov x0, #2
    msr rmr_el3, x0
    isb
#endif
1:  wfi
    b 1b
.ltorg
.balign 2048
el3_vectors:
.rept 16
    b el3_exception
    .balign 128
.endr
el1_vectors:
.rept 16
    b el1_exception
    .balign 128
.endr
