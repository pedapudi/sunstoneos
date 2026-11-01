/* The exception handler selects only these fixed continuations. C executes
 * on SP_EL0 after ERET to EL1t, with ordinary task IRQ/preemption support. */
.text
.balign 4
.global pvr_gpu_call
.type pvr_gpu_call, %function
pvr_gpu_call:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    svc #120
.global pvr_gpu_call_enter_site
.hidden pvr_gpu_call_enter_site
pvr_gpu_call_enter_site:
    b pvr_gpu_call_user_resume
.global pvr_gpu_call_kernel
.hidden pvr_gpu_call_kernel
pvr_gpu_call_kernel:
    bl pvr_gpu_call_dispatch
    svc #121
.global pvr_gpu_call_exit_site
.hidden pvr_gpu_call_exit_site
pvr_gpu_call_exit_site:
    /* A refused exit must never return through caller-controlled LR at EL1. */
    brk #121
.global pvr_gpu_call_user_resume
.hidden pvr_gpu_call_user_resume
pvr_gpu_call_user_resume:
    ldp x29, x30, [sp], #16
    ret
.size pvr_gpu_call, .-pvr_gpu_call

/* Port users without a GPU dispatcher retain a bounded refusal. */
.weak pvr_gpu_call_dispatch
.type pvr_gpu_call_dispatch, %function
pvr_gpu_call_dispatch:
    mov w0, #-1
    ret
.size pvr_gpu_call_dispatch, .-pvr_gpu_call_dispatch
