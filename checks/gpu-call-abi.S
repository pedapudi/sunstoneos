.text
.global gpu_call_abi
.type gpu_call_abi, %function
gpu_call_abi:
    sub sp, sp, #160
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x19, #19
    mov x20, #20
    mov x21, #21
    mov x22, #22
    mov x23, #23
    mov x24, #24
    mov x25, #25
    mov x26, #26
    mov x27, #27
    mov x28, #28
    mov x29, #29
    mov x3, #8
    fmov d8, x3
    mov x3, #9
    fmov d9, x3
    mov x3, #10
    fmov d10, x3
    mov x3, #11
    fmov d11, x3
    mov x3, #12
    fmov d12, x3
    mov x3, #13
    fmov d13, x3
    mov x3, #14
    fmov d14, x3
    mov x3, #15
    fmov d15, x3
    bl pvr_gpu_call
    mov w2, w0
    cmp x19, #19
    b.ne .Labi_failed
    cmp x20, #20
    b.ne .Labi_failed
    cmp x21, #21
    b.ne .Labi_failed
    cmp x22, #22
    b.ne .Labi_failed
    cmp x23, #23
    b.ne .Labi_failed
    cmp x24, #24
    b.ne .Labi_failed
    cmp x25, #25
    b.ne .Labi_failed
    cmp x26, #26
    b.ne .Labi_failed
    cmp x27, #27
    b.ne .Labi_failed
    cmp x28, #28
    b.ne .Labi_failed
    cmp x29, #29
    b.ne .Labi_failed
    fmov x1, d8
    cmp x1, #8
    b.ne .Labi_failed
    fmov x1, d9
    cmp x1, #9
    b.ne .Labi_failed
    fmov x1, d10
    cmp x1, #10
    b.ne .Labi_failed
    fmov x1, d11
    cmp x1, #11
    b.ne .Labi_failed
    fmov x1, d12
    cmp x1, #12
    b.ne .Labi_failed
    fmov x1, d13
    cmp x1, #13
    b.ne .Labi_failed
    fmov x1, d14
    cmp x1, #14
    b.ne .Labi_failed
    fmov x1, d15
    cmp x1, #15
    b.ne .Labi_failed
    b .Labi_restore
.Labi_failed:
    mov w2, #-2
.Labi_restore:
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    mov w0, w2
    add sp, sp, #160
    ret
.size gpu_call_abi, .-gpu_call_abi
