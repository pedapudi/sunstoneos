.text
.global check_migrating_context
.type check_migrating_context, %function
// x0: expected snapshot; x1: actual snapshot; x2: migration gate.
// Frame: snapshot 0..799, arguments 800..823, ABI state 832..1007.
check_migrating_context:
    sub sp, sp, #1024
    str x0, [sp, #800]
    str x1, [sp, #808]
    str x2, [sp, #816]
    str x19, [sp, #832]
    str x20, [sp, #840]
    str x21, [sp, #848]
    str x22, [sp, #856]
    str x23, [sp, #864]
    str x24, [sp, #872]
    str x25, [sp, #880]
    str x26, [sp, #888]
    str x27, [sp, #896]
    str x28, [sp, #904]
    str x29, [sp, #912]
    str x30, [sp, #920]
    str d8, [sp, #928]
    str d9, [sp, #936]
    str d10, [sp, #944]
    str d11, [sp, #952]
    str d12, [sp, #960]
    str d13, [sp, #968]
    str d14, [sp, #976]
    str d15, [sp, #984]
    mrs x3, fpcr
    mrs x4, fpsr
    str x3, [sp, #992]
    str x4, [sp, #1000]
    mov x3, sp
    str x3, [x0, #248]
    ldr x3, [x0, #776]
    msr fpcr, x3
    ldr x3, [x0, #784]
    msr fpsr, x3
    ldr x3, [x0, #768]
    msr nzcv, x3
    add x2, x0, #256
    ldp q0, q1, [x2, #0]
    ldp q2, q3, [x2, #32]
    ldp q4, q5, [x2, #64]
    ldp q6, q7, [x2, #96]
    ldp q8, q9, [x2, #128]
    ldp q10, q11, [x2, #160]
    ldp q12, q13, [x2, #192]
    ldp q14, q15, [x2, #224]
    ldp q16, q17, [x2, #256]
    ldp q18, q19, [x2, #288]
    ldp q20, q21, [x2, #320]
    ldp q22, q23, [x2, #352]
    ldp q24, q25, [x2, #384]
    ldp q26, q27, [x2, #416]
    ldp q28, q29, [x2, #448]
    ldp q30, q31, [x2, #480]
    mov x30, x0
    ldp x0, x1, [x30, #0]
    ldp x2, x3, [x30, #16]
    ldp x4, x5, [x30, #32]
    ldp x6, x7, [x30, #48]
    ldp x8, x9, [x30, #64]
    ldp x10, x11, [x30, #80]
    ldp x12, x13, [x30, #96]
    ldp x14, x15, [x30, #112]
    ldp x16, x17, [x30, #128]
    ldp x18, x19, [x30, #144]
    ldp x20, x21, [x30, #160]
    ldp x22, x23, [x30, #176]
    ldp x24, x25, [x30, #192]
    ldp x26, x27, [x30, #208]
    ldp x28, x29, [x30, #224]
    ldr x30, [x30, #240]
    // Exercise the yield exception with every register holding its pattern.
    svc #105
.Lobserve:
    stp x0, x1, [sp]
    mrs x0, nzcv
    str x0, [sp, #16]
    svc #109
    ldr x1, [sp, #816]
    add x1, x1, #8
    stlr w0, [x1]
    sub x1, x1, #8
    mov w0, #1
    stlr w0, [x1]
    add x1, x1, #4
    ldar w0, [x1]
    cbnz w0, .Lreleased
    ldr x0, [sp, #16]
    msr nzcv, x0
    ldp x0, x1, [sp]
    b .Lobserve
.Lreleased:
    ldr x0, [sp, #16]
    msr nzcv, x0
    ldp x0, x1, [sp]
    // Snapshot before using any general or vector register as scratch.
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mov x0, sp
    str x0, [sp, #248]
    add x0, sp, #256
    stp q0, q1, [x0, #0]
    stp q2, q3, [x0, #32]
    stp q4, q5, [x0, #64]
    stp q6, q7, [x0, #96]
    stp q8, q9, [x0, #128]
    stp q10, q11, [x0, #160]
    stp q12, q13, [x0, #192]
    stp q14, q15, [x0, #224]
    stp q16, q17, [x0, #256]
    stp q18, q19, [x0, #288]
    stp q20, q21, [x0, #320]
    stp q22, q23, [x0, #352]
    stp q24, q25, [x0, #384]
    stp q26, q27, [x0, #416]
    stp q28, q29, [x0, #448]
    stp q30, q31, [x0, #480]
    mrs x0, nzcv
    str x0, [sp, #768]
    mrs x0, fpcr
    str x0, [sp, #776]
    mrs x0, fpsr
    str x0, [sp, #784]
    str xzr, [sp, #792]
    mov x2, sp
    ldr x3, [sp, #808]
    mov x4, #50
.Lcopy:
    ldp x0, x1, [x2], #16
    stp x0, x1, [x3], #16
    subs x4, x4, #1
    b.ne .Lcopy
    ldr x3, [sp, #992]
    ldr x4, [sp, #1000]
    msr fpcr, x3
    msr fpsr, x4
    ldr d8, [sp, #928]
    ldr d9, [sp, #936]
    ldr d10, [sp, #944]
    ldr d11, [sp, #952]
    ldr d12, [sp, #960]
    ldr d13, [sp, #968]
    ldr d14, [sp, #976]
    ldr d15, [sp, #984]
    ldr x19, [sp, #832]
    ldr x20, [sp, #840]
    ldr x21, [sp, #848]
    ldr x22, [sp, #856]
    ldr x23, [sp, #864]
    ldr x24, [sp, #872]
    ldr x25, [sp, #880]
    ldr x26, [sp, #888]
    ldr x27, [sp, #896]
    ldr x28, [sp, #904]
    ldr x29, [sp, #912]
    ldr x30, [sp, #920]
    add sp, sp, #1024
    ret
.size check_migrating_context, .-check_migrating_context
