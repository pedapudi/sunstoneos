/* CPU0 timer PPI30 and self-directed SGI0. No Normal-memory stores or other
 * cores. x4..x11 belong to the IRQ handler; the interrupted code avoids them.
 * Missed delivery returns after disabling and draining only these owned IRQs.
 * Unexpected IRQs or uncertain cleanup retain execution. */
.section .text.primary_irq_vectors,"ax"
.balign 2048
.global primary_irq_vectors
primary_irq_vectors:
.set slot,0
.rept 16
    .if slot == 5
        b primary_irq_handler
    .else
        b primary_irq_park
    .endif
    .space 124
    .set slot,slot+1
.endr

.section .text.primary_irq_probe,"ax"
.global primary_irq_probe, primary_irq_handler, primary_irq_park
.global primary_irq_delivery_stopped
.type primary_irq_probe,%function
primary_irq_probe:
    mov x12, x30
    mov x25, #0               // Timer deliveries.
    mov x26, #0               // SGI deliveries.
    mov x27, #2               // Insufficient time before any IRQ changes.
    mov x28, #0
    mov x29, #0
    isb
    mrs x0, cntpct_el0
    ldr x1, =4800000          // Reserve 100 ms for delivery and 100 ms cleanup.
    add x1, x0, x1
    cmp x1, x22
    b.hs .Lreturn
    ldr x15, =2400000
    add x15, x0, x15
    mrs x0, cntp_ctl_el0
    tst x0, #3               // ISTATUS is UNKNOWN while ENABLE=0.
    b.ne primary_irq_park
    mrs x0, icc_igrpen1_el1
    cbnz x0, primary_irq_park
    mrs x16, vbar_el1
    mrs x17, cntp_cval_el0
    add x13, x24, #0x10000
    ldr w14, [x13, #0x41c]
    adr x0, primary_irq_vectors
    msr vbar_el1, x0
    isb
    msr icc_ctlr_el1, xzr
    msr icc_bpr1_el1, xzr
    mov x0, #255
    msr icc_pmr_el1, x0
    isb
    mrs x0, icc_pmr_el1
    cmp x0, #0xe0
    b.ls primary_irq_park
    mov w0, #0xa0
    mov w1, w14
    bfi w1, w0, #16, #8
    str w1, [x13, #0x41c]
    dsb sy
    ldr w0, [x13, #0x41c]
    cmp w0, w1
    b.ne primary_irq_park
    mov w0, #0x40000000
    str w0, [x13, #0x100]
    dsb sy
.Lwait_enable:
    isb
    mrs x0, cntpct_el0
    cmp x0, x15
    b.hs primary_irq_park
    ldr w0, [x24]
    tbnz w0, #3, .Lwait_enable
    ldr w0, [x13, #0x100]
    mov w1, #1
    orr w1, w1, #0x40000000
    cmp w0, w1
    b.ne primary_irq_park
    mov x0, #1
    msr icc_igrpen1_el1, x0
    isb
    mrs x0, cntpct_el0
    mov x1, #24000
    add x0, x0, x1
    msr cntp_cval_el0, x0
    mov x0, #1
    msr cntp_ctl_el0, x0
    dsb ishst
    msr icc_sgi1r_el1, x0   // Aff1=0, Aff0 target bit0: CPU0 only.
    isb
    msr daifclr, #2
.Lwait_delivery:
    cmp x25, #1
    b.ne .Ltime
    cmp x26, #1
    b.eq .Lreceived
.Ltime:
    isb
    mrs x0, cntpct_el0
    cmp x0, x15
    b.lo .Lwait_delivery
    mov x27, #1             // Delivery incomplete; cleanup may still return.
    b .Lstop
.Lreceived:
    mov x27, #0
.Lstop:
    msr daifset, #2
    msr cntp_ctl_el0, xzr
    isb
    msr icc_igrpen1_el1, xzr
    isb
    dsb sy
primary_irq_delivery_stopped:
    mrs x0, cntp_ctl_el0
    tst x0, #3
    b.ne primary_irq_park
    ldr w28, [x13, #0x200]
    ldr w29, [x13, #0x300]
    cbnz x29, primary_irq_park
    // A missed SGI must already be observed pending before it can be cleared.
    // An unobserved request cannot be assumed drained merely because time passed.
    cbnz x26, .Lsgi_handled
    tbz x28, #0, primary_irq_park
    b .Lsgi_observed
.Lsgi_handled:
    tbnz x28, #0, primary_irq_park
.Lsgi_observed:
    mov w1, #1
    orr w1, w1, #0x40000000
    bic w0, w28, w1
    cbnz w0, primary_irq_park
    str w1, [x13, #0x180]
    dsb sy
.Lwait_disable:
    isb
    mrs x0, cntpct_el0
    cmp x0, x22
    b.hs primary_irq_park
    ldr w0, [x24]
    tbnz w0, #3, .Lwait_disable
    str w1, [x13, #0x280]
    dsb sy
    ldr w0, [x13, #0x200]
    cbnz w0, primary_irq_park
    ldr w0, [x13, #0x300]
    cbnz w0, primary_irq_park
    str w14, [x13, #0x41c]
    dsb sy
    ldr w0, [x13, #0x41c]
    cmp w0, w14
    b.ne primary_irq_park
    msr cntp_cval_el0, x17
    msr vbar_el1, x16
    isb
    mrs x0, cntp_cval_el0
    cmp x0, x17
    b.ne primary_irq_park
    mrs x0, vbar_el1
    cmp x0, x16
    b.ne primary_irq_park
.Lreturn:
    ret x12
.size primary_irq_probe,.-primary_irq_probe

primary_irq_handler:
    mrs x4, icc_iar1_el1
    cmp x4, #1020
    b.hs .Lirq_return
    cbz x4, .Lsgi
    cmp x4, #30
    b.ne primary_irq_park
    cbnz x25, primary_irq_park
    msr cntp_ctl_el0, xzr
    isb
    mov x25, #1
    b .Leoi
.Lsgi:
    cbnz x26, primary_irq_park
    mov x26, #1
.Leoi:
    dsb sy
    msr icc_eoir1_el1, x4
    isb
.Lirq_return:
    eret
primary_irq_park:
    msr daifset, #15
    wfi
    b primary_irq_park
.section .note.GNU-stack,"",%progbits
