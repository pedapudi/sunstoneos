/* Qualification fixture: record entry, then let the host-armed WDT0 expire. */
.section .text
.global _start
_start:
    mov x0, #0x0100
    movk x0, #0x0709, lsl #16
    mov w1, #1
    movk w1, #0x5c0d, lsl #16
    str w1, [x0]
    movz w1, #0x0101, lsl #16
    str w1, [x0, #4]
    dsb sy
1:  wfi
    b 1b
