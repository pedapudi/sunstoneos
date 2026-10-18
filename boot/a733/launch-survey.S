.syntax unified
.arch armv8-a
.arm
.section .text,"ax"
.global _start
.type _start,%function
_start:
    // Save the caller without touching its stack or changing any flags.
    str r0, saved_r0
    adr r0, saved_registers
    stmia r0, {r1-r12, sp, lr}
    mrs r1, cpsr
    str r1, saved_cpsr
    cpsid aif
    ldr r2, =0x86000
    str r1, [r2, #16]
    mrc p15, 0, r1, c1, c0, 0  // SCTLR, as in the returning DDR wrapper.
    str r1, [r2, #20]
    mov r1, #0                 // SCR unavailable: reading it may trap from SVC.
    str r1, [r2, #24]
    mrc p15, 0, r1, c0, c0, 5  // MPIDR, as in the native launcher.
    str r1, [r2, #28]
    mrc p15, 0, r1, c14, c0, 0 // CNTFRQ, as in the native launcher.
    str r1, [r2, #32]
    mrc p15, 0, r1, c12, c0, 2 // RMR read only; pinned reset64 reads it here.
    str r1, [r2, #36]
    isb
    mrrc p15, 0, r3, r4, c14
    str r3, [r2, #40]
    str r4, [r2, #44]
    isb
    mrrc p15, 0, r3, r4, c14
    str r3, [r2, #48]
    str r4, [r2, #52]
    ldr r1, [r0, #48]
    str r1, [r2, #56]
    ldr r1, [r0, #52]
    str r1, [r2, #60]
    ldr r1, saved_r0
    str r1, [r2, #64]
    mov r3, #0
1:  ldr r1, [r0, r3]
    add r4, r2, #68
    str r1, [r4, r3]
    add r3, r3, #4
    cmp r3, #48
    bne 1b
    mov r1, #1
    str r1, [r2, #4]
    mov r1, #128
    str r1, [r2, #8]
    mov r1, #1
    str r1, [r2, #12]
    dsb sy
    ldr r1, =0x3153564c
    str r1, [r2]
    dsb sy
    ldmia r0, {r1-r12, sp, lr}
    ldr r0, saved_cpsr
    msr cpsr_cxsf, r0
    ldr r0, saved_r0
    bx lr
.balign 4
saved_r0: .word 0
saved_cpsr: .word 0
saved_registers: .space 56
.ltorg
.size _start, .-_start
.section .note.GNU-stack,"",%progbits
