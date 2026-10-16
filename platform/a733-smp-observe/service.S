.section .text.a733_observe,"ax"
.balign 16
.global a733_observe_svc
.type a733_observe_svc,%function
a733_observe_svc:
    svc #120
.global a733_observe_svc_return
a733_observe_svc_return:
    ret
.size a733_observe_svc,.-a733_observe_svc
// Entered after the canonical BHB sequence, with x0/x1 restored. Only this
// handler returns; faults retain the original non-returning monitor path.
.global a733_observe_exception
.type a733_observe_exception,%function
a733_observe_exception:
    sub sp,sp,#176
    stp x0,x1,[sp,#0]
    stp x2,x3,[sp,#16]
    stp x4,x5,[sp,#32]
    stp x6,x7,[sp,#48]
    stp x8,x9,[sp,#64]
    stp x10,x11,[sp,#80]
    stp x12,x13,[sp,#96]
    stp x14,x15,[sp,#112]
    stp x16,x17,[sp,#128]
    stp x18,x30,[sp,#144]
    mrs x0,elr_el1
    mrs x1,spsr_el1
    stp x0,x1,[sp,#160]
    bl a733_observe_privileged
    ldp x0,x1,[sp,#160]
    msr elr_el1,x0
    msr spsr_el1,x1
    ldp x0,x1,[sp,#0]
    ldp x2,x3,[sp,#16]
    ldp x4,x5,[sp,#32]
    ldp x6,x7,[sp,#48]
    ldp x8,x9,[sp,#64]
    ldp x10,x11,[sp,#80]
    ldp x12,x13,[sp,#96]
    ldp x14,x15,[sp,#112]
    ldp x16,x17,[sp,#128]
    ldp x18,x30,[sp,#144]
    add sp,sp,#176
    eret
.size a733_observe_exception,.-a733_observe_exception
.section .note.GNU-stack,"",%progbits
