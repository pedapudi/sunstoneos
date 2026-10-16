.section .text.a733_smp_boot,"ax"
.global a733_smp_entry
.type a733_smp_entry,%function
a733_smp_entry:
    msr daifset, #15
    mrs x0, CurrentEL
    cmp x0, #12
    b.ne .Lpark
    mrs x0, mpidr_el1
    mov x1, #0xc1000000
    bic x0, x0, x1
    cbnz x0, .Lpark
    mrs x0, sctlr_el3
    mov x1, #0x1005
    movk x1, #0x0200, lsl #16
    tst x0, x1
    b.ne .Lpark
    msr spsel, #1
    ldr x0, =__a733_smp_boot_stack_top
    mov sp, x0
    adr x0, a733_smp_boot_vectors
    msr vbar_el3, x0
    mov x0, #0x3600
    movk x0, #0x016e, lsl #16
    msr cntfrq_el0, x0
    msr cntps_ctl_el1, xzr
    msr cntp_ctl_el0, xzr
    msr cntv_ctl_el0, xzr
    isb
    ldr x0, =__bss_start
    ldr x1, =__bss_end
.Lclear:
    cmp x0, x1
    b.hs .Lmonitor
    str xzr, [x0], #8
    b .Lclear
.Lmonitor:
    ldr x0, =__monitor_bss_start
    ldr x1, =__monitor_bss_end
.Lclear_monitor:
    cmp x0, x1
    b.hs .Lboot
    str xzr, [x0], #8
    b .Lclear_monitor
.Lboot:
    bl a733_smp_boot
.Lpark:
    wfe
    b .Lpark
.balign 2048
.global a733_smp_boot_vectors
a733_smp_boot_vectors:
.rept 16
    b .Lpark
    .space 124
.endr
.section .note.GNU-stack,"",%progbits
