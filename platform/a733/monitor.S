.section .text.boot.monitor, "ax"
.global monitor_entry
monitor_entry:
    msr daifset, #0xf
    msr spsel, #1
    ldr x0, =__monitor_stack_top
    mov sp, x0
    adr x0, monitor_vectors
    msr vbar_el3, x0
    isb
#ifdef SUNSTONE_BOARD_PROBE
    bl sunstone_probe_begin
#endif
    bl invalidate_data_cache
    ic iallu
    dsb sy
    isb
    bl monitor_init
    // monitor_init returns with NS=1 and EL2/EL1 timer/GIC access configured.
    ldr x0, =0x30d00800
    msr sctlr_el1, x0
    msr sctlr_el2, xzr
    adr x0, _start
    msr elr_el3, x0
    mov x0, #0x3c5
    msr spsr_el3, x0
    dsb sy
    isb
    eret

.section .text.monitor_vectors, "ax"
.balign 2048
monitor_vectors:
.macro slot kind
#ifdef SUNSTONE_USB_RUNTIME
    .if \kind == 1
        b monitor_sync
        .space 124
    .else
        mov x2, #\kind
        b monitor_exception
        .space 120
    .endif
#else
    mov x2, #\kind
    b monitor_exception
    .space 120
#endif
.endm
.rept 4
    slot 1
    slot 3
    slot 2
    slot 3
.endr

// Renewal and reset code, which the renewal SMC and the timer, exception and
// exit paths run with the monitor's MMU off. apps/memtest/link.ld places this
// section in the memory qualification image's recovery pages.
.section .text.monitor_recovery, "ax"
#ifdef SUNSTONE_USB_RUNTIME
// A narrow SMC renews the recovery deadline while the USB task makes progress.
monitor_sync:
    stp x2, x3, [sp, #-16]!
    mrs x2, esr_el3
    lsr x2, x2, #26
    cmp x2, #0x17
    b.ne 2f
#ifdef SUNSTONE_BOARD_PROBE
    cmp x0,#0x735
    b.eq 5f
#endif
    cmp x0, #0x734
    b.ne 2f
    mrs x2, cntpct_el0
#ifdef SUNSTONE_BOARD_PROBE
    ldr x3, =sunstone_probe_deadline
    ldr x3, [x3]
    cmp x2, x3
    b.hs sunstone_probe_timeout
    // x1-x30 stay unchanged across renewal, including x16 in memtest kernels.
    ldr x0, =0x02050014
    ldr w3, [x0]
    and w3, w3, #0xffff
    cmp w3, #0xb1
    b.ne sunstone_probe_timeout
    ldr x0, =0x0205000c
    mov w3, #0x14af
    str w3, [x0]
    ldr x0, =0x07090104
    movz w3, #0x0802, lsl #16
    str w3, [x0]
    lsr x3, x2, #10
    str w3, [x0, #8]
    dsb sy
#endif
    ldr x3, =48000000
    add x2, x2, x3
    msr cntps_cval_el1, x2
    isb
    mov x0, #0
    ldp x2, x3, [sp], #16
    eret
#ifdef SUNSTONE_BOARD_PROBE
5:  ldp x2,x3,[sp],#16
    mov x0,x1
    mov x1,x2
    mov x2,x3
    mov x3,x4
    b sunstone_probe_fault
#endif
2:  ldp x2, x3, [sp], #16
    mov x2, #1
    b monitor_exception
#endif
.global monitor_reset
monitor_reset:
    msr daifset, #0xf
    bl clean_data_cache
    ic iallu
    dsb sy
    isb
    ldr x0, =0x08001004
    mov w1, #0x20
    str w1, [x0]
    str wzr, [x0, #4]
    dsb sy
    mov x0, #2
    msr rmr_el3, x0
    isb
1:  wfi
    b 1b
