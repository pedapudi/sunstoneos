.section .text.vectors, "ax"
.balign 2048
.global _freertos_vector_table
_freertos_vector_table:
.macro slot handler
    b \handler
    .space 124
.endm
    // Current EL, SP_EL0.
    slot synchronous_exception
    slot FreeRTOS_IRQ_Handler
    slot platform_exception
    slot platform_exception
    // Current EL, SP_EL1.
    slot synchronous_exception
    slot FreeRTOS_IRQ_Handler
    slot platform_exception
    slot platform_exception
    // Lower EL, AArch64.
    slot platform_exception
    slot platform_exception
    slot platform_exception
    slot platform_exception
    // Lower EL, AArch32.
    slot platform_exception
    slot platform_exception
    slot platform_exception
    slot platform_exception
.size _freertos_vector_table, .-_freertos_vector_table
.if (. - _freertos_vector_table) != 2048
    .error "AArch64 requires sixteen 128-byte exception vector slots"
.endif

.section .text, "ax"
synchronous_exception:
    // Test the exception class before the port switches to the task stack.
    stp x0, x1, [sp, #-16]!
    mrs x0, esr_el1
    lsr x0, x0, #26
    cmp x0, #0x15
    ldp x0, x1, [sp], #16
    b.eq FreeRTOS_SWI_Handler
    b platform_exception
