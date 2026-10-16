.section .text.boot.monitor, "ax"
.global monitor_entry
monitor_entry:
    msr daifset, #0xf
    msr spsel, #1
    ldr x0, =__monitor_stack_top
    mov sp, x0
    adr x0, monitor_vectors
    msr vbar_el3, x0
    isb
    bl invalidate_data_cache
    ic iallu
    dsb sy
    isb
    bl monitor_init
    // monitor_init returns with NS=1 and EL2/EL1 timer/GIC access configured.
    ldr x0, =0x30d00800
    msr sctlr_el1, x0
    msr sctlr_el2, xzr
    adr x0, _start
    msr elr_el3, x0
    mov x0, #0x3c5
    msr spsr_el3, x0
    dsb sy
    isb
    eret

.section .text.monitor_vectors, "ax"
.balign 2048
monitor_vectors:
.macro slot kind
    mov x2, #\kind
    b monitor_exception
    .space 120
.endm
.rept 4
    slot 1
    slot 3
    slot 2
    slot 3
.endr

.section .text, "ax"
.global monitor_reset
monitor_reset:
    msr daifset, #0xf
    bl clean_data_cache
    ic iallu
    dsb sy
    isb
    ldr x0, =0x08001004
    mov w1, #0x20
    str w1, [x0]
    str wzr, [x0, #4]
    dsb sy
    mov x0, #2
    msr rmr_el3, x0
    isb
1:  wfi
    b 1b
