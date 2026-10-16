.syntax unified
.arch armv8-a
.arm
.section .text.entry, "ax"
.global _start
_start:
    push {r0-r12, lr}
    mrs r0, cpsr
    push {r0}
    cpsid aif
    mov r4, sp
    ldr sp, =0x54000
    bl watchdog_expiry
    dsb sy
    mov sp, r4
    pop {r0}
    msr cpsr_cxsf, r0
    pop {r0-r12, lr}
    bx lr
    .ltorg

.global watchdog_expiry_wait
watchdog_expiry_wait:
    /* Interrupts remain masked. Only the independent watchdog ends this wait. */
    b watchdog_expiry_wait
