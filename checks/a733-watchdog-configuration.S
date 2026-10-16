.syntax unified
.arch armv8-a
.arm
.section .text.entry, "ax"
.global _start
_start:
    push {r0-r12, lr}
    mrs r0, cpsr
    push {r0}
    cpsid if
    mov r4, sp
    ldr sp, =0x54000
    bl watchdog_configuration
    dsb sy
    mov sp, r4
    pop {r0}
    msr cpsr_cxsf, r0
    pop {r0-r12, lr}
    bx lr
    .ltorg
