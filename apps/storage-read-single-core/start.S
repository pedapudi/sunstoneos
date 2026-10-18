.section .text.sd_single_core_boot,"ax"
.global sd_single_core_entry
.type sd_single_core_entry,%function
sd_single_core_entry:
    msr daifset,#15
    mrs x0,CurrentEL
    cmp x0,#12
    b.ne .Lpark
    mrs x0,mpidr_el1
    mov x1,#0xc1000000
    bic x0,x0,x1
    cbnz x0,.Lpark
    mrs x0,sctlr_el3
    mov x1,#0x1005
    movk x1,#0x0200,lsl #16
    tst x0,x1
    b.ne .Lpark
    msr spsel,#1
    ldr x0,=__monitor_stack_top
    mov sp,x0
    adr x0,sd_single_core_vectors
    msr vbar_el3,x0
    msr cntps_ctl_el1,xzr
    msr cntp_ctl_el0,xzr
    msr cntv_ctl_el0,xzr
    isb
    bl invalidate_data_cache
    ic iallu
    dsb sy
    isb
    ldr x0,=__monitor_state_start
    ldr x1,=__monitor_state_end
.Lclear_monitor:
    cmp x0,x1
    b.hs .Ladmit
    str xzr,[x0],#8
    b .Lclear_monitor
.Ladmit:
    bl sd_single_core_monitor_init
    // The admitted snapshots are outside _start's EL1-cleared BSS.
    ldr x0,=0x30d00800
    msr sctlr_el1,x0
    msr sctlr_el2,xzr
    adr x0,_start
    msr elr_el3,x0
    mov x0,#0x3c5
    msr spsr_el3,x0
    dsb sy
    isb
    eret
.Lpark:
    wfe
    b .Lpark
.size sd_single_core_entry,.-sd_single_core_entry
.section .text.sd_single_core_vectors,"ax"
.balign 2048
sd_single_core_vectors:
.macro slot kind
    mov x2,#\kind
    b sd_single_core_exception
    .space 120
.endm
.rept 4
    slot 1
    slot 3
    slot 2
    slot 3
.endr
.section .note.GNU-stack,"",%progbits
