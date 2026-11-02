.section .text.boot,"ax"
.global _start
_start:
    // Tasks of the FreeRTOS port run on SP_EL0 (SPSel 0) and exceptions use SP_EL1, as on the board.
    ldr x0,=isr_stack_top
    mov sp,x0
    ldr x0,=stack_top
    msr sp_el0,x0
    msr spsel,#0
    mrs x1,CurrentEL
    mov x2,#0x3c0
    msr daifset,#15
    mov x3,#(3<<20)
    msr cpacr_el1,x3
    isb
    msr vbar_el1,xzr
    bl main
1:  wfe
    b 1b
.section .bss
.balign 16
.space 65536
stack_top:
.space 4096
isr_stack_top:
