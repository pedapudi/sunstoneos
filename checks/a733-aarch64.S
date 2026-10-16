.arch armv8-a
.section .text, "ax"
.global _start
_start:
    msr daifset, #0xf
    ldr x0, =0x42000000
    mrs x1, CurrentEL
    str x1, [x0, #8]
    mrs x1, midr_el1
    str x1, [x0, #16]
    mrs x1, mpidr_el1
    str x1, [x0, #24]
    mrs x1, cntfrq_el0
    str x1, [x0, #32]
    mrs x1, cntpct_el0
    str x1, [x0, #40]
    mrs x1, rvbar_el3
    str x1, [x0, #48]
    mrs x1, sctlr_el3
    str x1, [x0, #56]
    ldr x1, =0x41363450
    str x1, [x0]
    dsb sy
    // BROM's forced-FEL entry clears SRAM and initializes USB without
    // attempting a storage boot. The result remains in DRAM.
    ldr x0, =0x08001004
    mov w1, #0x20
    str w1, [x0]
    str wzr, [x0, #4]
    dsb sy
    mov x0, #2                    // Warm reset, highest EL in AArch32.
    msr rmr_el3, x0
    isb
1:  wfi
    b 1b
    .ltorg
