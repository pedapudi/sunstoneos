.syntax unified
.arch armv8-a
.arm
.section .text.entry,"ax"
.global a733_smp_launch
.type a733_smp_launch,%function
a733_smp_launch:
    push {r0-r12, lr}
    mrs r0, cpsr
    push {r0}
    cpsid aif
    mrc p15, 0, r0, c1, c0, 0
    ldr r1, =0x02001005
    tst r0, r1
    bne a733_smp_launch_park
    mrs r0, cpsr
    tst r0, #0x200
    bne a733_smp_launch_park
    and r0, r0, #0x1f
    cmp r0, #0x13
    bne a733_smp_launch_park
    mrc p15, 0, r0, c1, c1, 0
    tst r0, #1
    bne a733_smp_launch_park
    mrc p15, 0, r0, c0, c0, 5
    ldr r1, =0xc1000000
    bic r0, r0, r1
    cmp r0, #0
    bne a733_smp_launch_park
    mov r4, sp
    ldr sp, =0x54000
    bl a733_smp_launch_prepare
    cmp r0, #0
    beq a733_smp_launch_return
    // Successful admission includes readback, publication and elapsed checks.
    // The pinned FEL reset64 helper uses RVBAR, DSB/ISB, RMR.RR|AA64.
    ldr r0, =0x08001004
    ldr r1, =0x41000000
    str r1, [r0]
    mov r1, #0
    str r1, [r0, #4]
    dsb sy
    isb sy
    bl a733_smp_launch_last_check
    mrc p15, 0, r0, c12, c0, 2
    orr r0, r0, #3
    mcr p15, 0, r0, c12, c0, 2
    isb sy
    b a733_smp_launch_park
.global a733_smp_launch_return
a733_smp_launch_return:
    dsb sy
    mov sp, r4
    pop {r0}
    msr cpsr_cxsf, r0
    pop {r0-r12, lr}
    bx lr
.global a733_smp_launch_park
a733_smp_launch_park:
    wfi
    b a733_smp_launch_park
    .ltorg
.section .note.GNU-stack,"",%progbits
