.section .text.vectors, "ax"
.balign 2048
.global _freertos_vector_table
_freertos_vector_table:
.set npu_el1_slot,0
.macro slot handler
.ifc \handler,npu_finite_el1_fault
    stp x0,x1,[sp,#-16]!
    mov x1,#npu_el1_slot
    b npu_finite_el1_fault_saved
    .space 116
.else
    b \handler
    .space 124
.endif
    .set npu_el1_slot,npu_el1_slot+1
.endm
    // Current EL, SP_EL0.
    slot synchronous_exception
    slot FreeRTOS_IRQ_Handler
    slot npu_finite_el1_fault
    slot npu_finite_el1_fault
    // Current EL, SP_EL1.
    slot synchronous_exception
    slot FreeRTOS_IRQ_Handler
    slot npu_finite_el1_fault
    slot npu_finite_el1_fault
    // Lower EL, AArch64.
    slot npu_finite_el1_fault
    slot npu_finite_el1_fault
    slot npu_finite_el1_fault
    slot npu_finite_el1_fault
    // Lower EL, AArch32.
    slot npu_finite_el1_fault
    slot npu_finite_el1_fault
    slot npu_finite_el1_fault
    slot npu_finite_el1_fault
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
    b.eq npu_finite_svc
    b npu_finite_el1_fault

/* Keep the FreeRTOS SVC path unchanged for every other immediate. */
npu_finite_svc:
    stp x0,x1,[sp,#-16]!
    mrs x0,esr_el1
    and x0,x0,#0xffff
    mov x1,#0x5c0d
    cmp x0,x1
    b.eq .Lnpu_crumb
    mov x1,#0x5c0e
    cmp x0,x1
    b.eq .Lnpu_fail
    ldp x0,x1,[sp],#16
    b FreeRTOS_SWI_Handler
.Lnpu_crumb:
    stp x2,x3,[sp,#-16]!
    ldp x0,x1,[sp,#16]
    bfi w1,w0,#16,#16
    mov x2,#0x07090000
    str w1,[x2,#0x104]
    mrs x3,cntpct_el0
    ubfx x3,x3,#10,#29
    mrs x0,mpidr_el1
    ubfx x0,x0,#8,#3
    orr w3,w3,w0,lsl #29
    str w3,[x2,#0x10c]
    dsb sy
    ldp x2,x3,[sp],#16
    ldp x0,x1,[sp],#16
    eret
.Lnpu_fail:
    stp x2,x3,[sp,#-16]!
    stp x4,x5,[sp,#-16]!
    ldp x0,x1,[sp,#32]
    mov x3,#0x07090000
    ldr w4,[x3,#0x110]
    cbnz w4,.Lnpu_fail_done
    str w0,[x3,#0x114]
    str w2,[x3,#0x118]
    str w1,[x3,#0x11c]
    mrs x4,mpidr_el1
    ubfx x4,x4,#8,#3
    mrs x5,spsr_el1
    and w5,w5,#0xf
    mov w0,#0xfa0b0000
    orr w0,w0,w4,lsl #20
    orr w0,w0,w5,lsl #8
    dsb sy
    str w0,[x3,#0x110]
    dsb sy
.Lnpu_fail_done:
    ldp x4,x5,[sp],#16
    ldp x2,x3,[sp],#16
    ldp x0,x1,[sp],#16
    eret
/* Preserve the first synchronous EL1 fault before platform_exception can use
 * task metadata or call an exit path. Synchronous current-EL slots are
 * selected by the saved SP mode; other slots pass their table index. */
npu_finite_el1_fault:
    stp x0,x1,[sp,#-16]!
    mrs x1,spsr_el1
    and x1,x1,#1
    lsl x1,x1,#2
npu_finite_el1_fault_saved:
    stp x2,x3,[sp,#-16]!
    mov x3,x1
    mov x0,#0x07090000
    ldr w1,[x0,#0x110]
    cbnz w1,.Lnpu_fault_done
    mrs x1,esr_el1
    str w1,[x0,#0x114]
    mrs x1,elr_el1
    str w1,[x0,#0x118]
    mrs x1,far_el1
    str w1,[x0,#0x11c]
    mrs x1,mpidr_el1
    ubfx x1,x1,#8,#3
    mov w2,#0xfa010000
    orr w2,w2,w1,lsl #20
    orr w2,w2,w3,lsl #8
    ldr w1,[sp,#16]
    and w1,w1,#255
    orr w2,w2,w1
    dsb sy
    str w2,[x0,#0x110]
    dsb sy
.Lnpu_fault_done:
    ldp x2,x3,[sp],#16
    ldp x0,x1,[sp],#16
    b platform_exception
