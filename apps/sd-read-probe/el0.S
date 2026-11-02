// EL0 routines, their return path and the vector table that serves them.
#define CALL_ENTRY 0
#define CALL_STACK 8
#define CALL_A0 16
#define CALL_A1 24
#define CALL_A2 32
#define CALL_A3 40
#define CALL_ESR 48
#define CALL_FAR 56
#define CALL_ELR 64
#define CALL_X0 72
#define CALL_VBAR 80
#define CALL_DAIF 88

.section .text.region_el0,"ax"
.balign 16
.global region_el0_call
.type region_el0_call,%function
region_el0_call:
    stp x19,x20,[sp,#-96]!
    stp x21,x22,[sp,#16]
    stp x23,x24,[sp,#32]
    stp x25,x26,[sp,#48]
    stp x27,x28,[sp,#64]
    stp x29,x30,[sp,#80]
    mrs x9,daif
    str x9,[x0,#CALL_DAIF]
    msr daifset,#15
    mrs x9,vbar_el1
    str x9,[x0,#CALL_VBAR]
    adr x9,region_el0_vectors
    msr vbar_el1,x9
    isb
    adrp x10,region_el0_current
    str x0,[x10,:lo12:region_el0_current]
    ldr x11,[x0,#CALL_STACK]
    msr sp_el0,x11
    ldr x11,[x0,#CALL_ENTRY]
    msr elr_el1,x11
    mov x12,#0x3c0
    msr spsr_el1,x12
    ldr x1,[x0,#CALL_A1]
    ldr x2,[x0,#CALL_A2]
    ldr x3,[x0,#CALL_A3]
    ldr x0,[x0,#CALL_A0]
    isb
    eret
.size region_el0_call,.-region_el0_call

region_el0_unexpected:
    mov x14,#0xff
    b region_el0_finish
region_el0_return:
    mov x14,xzr
region_el0_finish:
    mrs x9,esr_el1
    mrs x10,far_el1
    mrs x11,elr_el1
    adrp x12,region_el0_current
    ldr x12,[x12,:lo12:region_el0_current]
    str x9,[x12,#CALL_ESR]
    str x10,[x12,#CALL_FAR]
    str x11,[x12,#CALL_ELR]
    str x0,[x12,#CALL_X0]
    ldr x13,[x12,#CALL_VBAR]
    msr vbar_el1,x13
    isb
    ldr x13,[x12,#CALL_DAIF]
    msr daif,x13
    lsr x0,x9,#26
    cbz x14,1f
    mov x0,x14
1:  ldp x21,x22,[sp,#16]
    ldp x23,x24,[sp,#32]
    ldp x25,x26,[sp,#48]
    ldp x27,x28,[sp,#64]
    ldp x29,x30,[sp,#80]
    ldp x19,x20,[sp],#96
    ret

.macro vec target
    .balign 128
    b \target
.endm
.balign 2048
region_el0_vectors:
    vec region_el0_unexpected
    vec region_el0_unexpected
    vec region_el0_unexpected
    vec region_el0_unexpected
    vec region_el0_unexpected
    vec region_el0_unexpected
    vec region_el0_unexpected
    vec region_el0_unexpected
    vec region_el0_return          // lower EL, AArch64, synchronous: SVC and aborts
    vec region_el0_unexpected
    vec region_el0_unexpected
    vec region_el0_unexpected
    vec region_el0_unexpected
    vec region_el0_unexpected
    vec region_el0_unexpected
    vec region_el0_unexpected

.section .bss.region_el0
.balign 8
region_el0_current: .space 8

// Position-independent routines, copied to the EL0 code window. Each ends in SVC #0 with its result in x0.
.section .rodata.region_el0_code,"a"
.balign 16
.global region_el0_code_start
region_el0_code_start:
// x0 base, x1 bytes, x2 stride, x3 mode (0 read, 1 read and write back, 2 store zero without a load);
// x0 = XOR of the words read. Mode 2 shows the write bit of a fault; it zeroes memory if it succeeds.
.global region_el0_touch
region_el0_touch:
    mov x4,xzr
    add x1,x0,x1
1:  cmp x3,#2
    b.eq 3f
    ldr x5,[x0]
    eor x4,x4,x5
    cbz x3,2f
    str x5,[x0]
    b 2f
3:  str xzr,[x0]
2:  add x0,x0,x2
    cmp x0,x1
    b.lo 1b
    mov x0,x4
    svc #0
// x0 base, x1 bytes (a multiple of 128): 128-byte vector loads as apps/dram-bandwidth/kernel.c; x0 = XOR fold
.balign 16
.global region_el0_stream
region_el0_stream:
    add x1,x0,x1
    movi v16.16b,#0
    movi v17.16b,#0
    movi v18.16b,#0
    movi v19.16b,#0
1:  ld1 {v0.16b-v3.16b},[x0],#64
    ld1 {v4.16b-v7.16b},[x0],#64
    eor v16.16b,v16.16b,v0.16b
    eor v17.16b,v17.16b,v1.16b
    eor v18.16b,v18.16b,v2.16b
    eor v19.16b,v19.16b,v3.16b
    eor v16.16b,v16.16b,v4.16b
    eor v17.16b,v17.16b,v5.16b
    eor v18.16b,v18.16b,v6.16b
    eor v19.16b,v19.16b,v7.16b
    cmp x0,x1
    b.lo 1b
    eor v16.16b,v16.16b,v17.16b
    eor v18.16b,v18.16b,v19.16b
    eor v16.16b,v16.16b,v18.16b
    mov x2,v16.d[0]
    mov x3,v16.d[1]
    eor x0,x2,x3
    svc #0
.balign 16
.global region_el0_code_end
region_el0_code_end:
