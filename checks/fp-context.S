.text
.global check_fp_context
.type check_fp_context, %function
// Hold a distinct callee-saved FP register across a blocking context switch.
check_fp_context:
    stp x19, x30, [sp, #-32]!
    str d8, [sp, #16]
    mov x19, x0
    fmov d8, x0
    mov x0, #1
    bl vTaskDelay
    fmov x0, d8
    cmp x0, x19
    cset x0, eq
    ldr d8, [sp, #16]
    ldp x19, x30, [sp], #32
    ret
.size check_fp_context, .-check_fp_context
