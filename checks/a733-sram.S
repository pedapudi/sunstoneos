.syntax unified
.arch armv8-a
.arm
.section .text, "ax"
.global _start
_start:
    // FEL calls this in AArch32. Only caller-saved registers are changed.
    ldr r1, =0x52200
    ldr r0, =0x53534f53
    str r0, [r1], #4
    mrs r0, cpsr
    str r0, [r1], #4
    mrc p15, 0, r0, c0, c0, 0  // MIDR.
    str r0, [r1], #4
    mrc p15, 0, r0, c0, c0, 5  // MPIDR.
    str r0, [r1], #4
    mrc p15, 0, r0, c1, c0, 0  // SCTLR.
    str r0, [r1], #4
    dsb sy
    bx lr
    .ltorg
