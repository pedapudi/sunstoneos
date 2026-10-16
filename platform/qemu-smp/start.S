.section .text.boot, "ax"
.global _start
_start:
    msr daifset, #0xf
    mrs x0, CurrentEL
    cmp x0, #4
    b.ne .Lpark
    mrs x0, mpidr_el1
    and x1, x0, #0xff
    // This emulator target has two cores in one affinity group.
    lsr x0, x0, #8
    and x0, x0, #0xffff
    cbnz x0, .Lpark
    cmp x1, #2
    b.hs .Lpark
    msr tpidr_el1, x1
    msr spsel, #1
    ldr x0, =__exception_stacks
    add x1, x1, #1
    add x0, x0, x1, lsl #15
    mov sp, x0
    ldr x0, =_freertos_vector_table
    msr vbar_el1, x0
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    mov x0, #3
    msr cntkctl_el1, x0
    isb
    mrs x0, tpidr_el1
    cbnz x0, .Lsecondary
    ldr x0, =__bss_start
    ldr x1, =__bss_end
.Lclear:
    cmp x0, x1
    b.hs .Lprimary
    str xzr, [x0], #8
    b .Lclear
.Lprimary:
    bl kernel_main
.Lpark:
    wfe
    b .Lpark
.Lsecondary:
    bl platform_secondary
    b .Lpark

.section .text.vectors, "ax"
.balign 2048
.global _freertos_vector_table
_freertos_vector_table:
.macro slot handler
    b \handler
    .space 124
.endm
.rept 3
    slot synchronous_exception
    slot FreeRTOS_IRQ_Handler
    slot platform_exception
    slot platform_exception
.endr
.rept 4
    slot platform_exception
.endr

.section .text, "ax"
synchronous_exception:
    stp x0, x1, [sp, #-16]!
    mrs x0, esr_el1
    lsr x0, x0, #26
    cmp x0, #0x15
    b.ne .Lexception
    mrs x0, esr_el1
    and x0, x0, #0xffff
    cmp x0, #119
    b.eq .Lexit
    ldp x0, x1, [sp], #16
    b FreeRTOS_SWI_Handler
.Lexit:
    ldp x0, x1, [sp], #16
    b platform_exit_privileged
.Lexception:
    ldp x0, x1, [sp], #16
    b platform_exception
