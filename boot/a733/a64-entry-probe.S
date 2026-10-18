// Read initial A64 state, then use the physically exercised forced-FEL return.
.arch armv8-a
.section .text,"ax"
.global _start
_start:
    msr daifset, #15
    ldr x20, =0x42000000
    mov x1, #1
    str x1, [x20, #24]
    mrs x1, CurrentEL
    str x1, [x20, #32]
    cmp x1, #12
    b.ne park                    // No EL3 access if the transition was wrong.
    adr x1, vectors
    msr vbar_el3, x1
    isb
    mrs x1, midr_el1
    str x1, [x20, #40]
    mrs x1, mpidr_el1
    str x1, [x20, #48]
    mrs x1, cntfrq_el0
    str x1, [x20, #56]
    isb
    mrs x1, cntpct_el0
    str x1, [x20, #64]
    mrs x1, rvbar_el3
    str x1, [x20, #72]
    mrs x1, sctlr_el3
    str x1, [x20, #80]
    mrs x1, sctlr_el1
    str x1, [x20, #88]
    mrs x1, scr_el3              // CurrentEL==12 established before this read.
    str x1, [x20, #96]
    mov x1, sp
    str x1, [x20, #152]
    adr x2, expected_layout
    add x3, x20, #160
    mov x4, #7
1:  ldr x1, [x2], #8
    str x1, [x3], #8
    subs x4, x4, #1
    b.ne 1b
    isb
    mrs x1, cntpct_el0
    str x1, [x20, #224]
    b publish
exception:
    ldr x20, =0x42000000
    mrs x1, esr_el3
    str x1, [x20, #120]
    mrs x1, elr_el3
    str x1, [x20, #128]
    mrs x1, far_el3
    str x1, [x20, #136]
    str x18, [x20, #144]
    mov x1, #1
    str x1, [x20, #216]
publish:
    mov x1, #1
    str x1, [x20, #8]
    mov x1, #256
    str x1, [x20, #16]
    mov x1, #4
    str x1, [x20, #24]
    ldr x1, =0x413634454e545259
    str x1, [x20]
    dsb sy
    // Exact destination and RMR mode used by checks/a733-aarch64.S.
    ldr x0, =0x08001004
    mov w1, #0x20
    str w1, [x0]
    str wzr, [x0, #4]
    dsb sy
    mov x0, #2
    msr rmr_el3, x0
    isb
park:
    wfi
    b park
.ltorg
.balign 8
.global expected_layout
expected_layout:
    .quad 0x42014000,0x42005000,0x4200f590,0x421f0000,0x421f5000,0x41201000,0x41290d10
.balign 2048
vectors:
.macro vector kind
    mov x18, #\kind
    b exception
    .space 120
.endm
.rept 4
    vector 1
    vector 3
    vector 2
    vector 3
.endr
.section .note.GNU-stack,"",%progbits
