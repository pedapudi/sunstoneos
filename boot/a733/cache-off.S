.syntax unified
.arch armv8-a
.arm
.section .text, "ax"
.global _start
_start:
    // Use a fresh SRAM address for the first instruction-cache transition.
    mrc p15, 0, r0, c1, c0, 0
    tst r0, #5                   // Refuse an active MMU or data cache.
    bxne lr
    bic r0, r0, #0x1000
    mcr p15, 0, r0, c1, c0, 0
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
    dsb sy
    isb
    bx lr
