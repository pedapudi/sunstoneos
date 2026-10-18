/* Timer PPI30 and SGI0 only. Shared distributor SPIs and all LPIs stay disabled.
 * IRQ handler owns x4..x11; interrupted code never uses those registers during
 * delivery. No IRQ stack or unrelated Normal-memory write is introduced. */
.section .irq_vectors,"ax"
.balign 2048
.global identity_interrupt_vectors
identity_interrupt_vectors:
.set vector_slot,0
.rept 16
    .balign 128
    .if vector_slot == 5
        b identity_interrupt_handler
    .else
        b identity_interrupt_park
    .endif
    .space 124
    .set vector_slot,vector_slot+1
.endr

.section .text.identity_interrupt_el1,"ax"
.balign 4
.global identity_interrupt_el1, identity_interrupt_handler, identity_interrupt_park
.global identity_interrupt_groups_disabled
.type identity_interrupt_el1,%function
identity_interrupt_el1:
    // x18 own shared slot, x19 page, x20 core, x21 epoch, x22 deadline,
    // x24 redistributor. Preserve x25..x30 and reserve x4..x11 for the handler.
    mrs x12, vbar_el1
    ldr x0, =identity_interrupt_vectors
    msr vbar_el1, x0
    isb
    msr icc_ctlr_el1, xzr   // EOImode=0: EOIR performs priority drop/deactivation.
    msr icc_bpr1_el1, xzr
    mov x0, #255
    msr icc_pmr_el1, x0
    isb
    mrs x0, icc_pmr_el1
    cmp x0, #0xe0          // SGI0 Secure priority0xf0 is Nonsecure0xe0.
    b.ls identity_interrupt_park
    add x13, x24, #0x10000
    // PMR can round down to an implemented priority. Do not inherit a timer
    // priority equal to that mask. Nonsecure access exposes at least four
    // priority bits: 0xa0 remains below a maximum PMR rounded to 0xf0.
    // Preserve the other three PPI priorities.
    ldr w14, [x13, #0x41c]
    mov w0, #0xa0
    mov w1, w14
    bfi w1, w0, #16, #8
    str w1, [x13, #0x41c]
    dsb sy
    ldr w0, [x13, #0x41c]
    cmp w0, w1
    b.ne identity_interrupt_park
    add x13, x13, #0x100
    mov w0, #0x40000000
    str w0, [x13]          // SGI0 was enabled by production local initialization.
    dsb sy
.macro irq_deadline
    isb
    mrs x0, cntpct_el0
    cmp x0, x22
    b.hs identity_interrupt_park
.endm
.Lenable_wait:
    irq_deadline
    ldr w0, [x24]
    tbnz w0, #3, .Lenable_wait
    ldr w0, [x13]
    mov w1, #1
    orr w1, w1, #0x40000000
    cmp w0, w1
    b.ne identity_interrupt_park
    mov x0, #1
    msr icc_igrpen1_el1, x0
    isb
    msr daifclr, #2
    add x16, x18, #64
    stlr x21, [x16]
.macro irq_all offset
    mov x15, #1
.Lpeer\@:
    add x16, x19, #0x100
    add x16, x16, x15, lsl #7
    add x16, x16, #\offset
.Lwait\@:
    irq_deadline
    ldar x17, [x16]
    cmp x17, x21
    b.ne .Lwait\@
    add x15, x15, #1
    cmp x15, #8
    b.lo .Lpeer\@
.endm
    irq_all 64
    mrs x0, cntpct_el0
    mov x1, #24000
    add x0, x0, x1
    msr cntp_cval_el0, x0
    mov x0, #1
    msr cntp_ctl_el0, x0
    isb
    add x0, x20, #1
    cmp x0, #8
    mov x1, #1
    csel x0, x1, x0, eq
    lsl x0, x0, #16       // Aff1 is the A733 core index; Aff0 target list is bit0.
    orr x0, x0, #1
    dsb ishst
    msr icc_sgi1r_el1, x0
    isb
    add x16, x18, #72
    stlr x21, [x16]
.Lreceived:
    irq_deadline
    ldp x1, x2, [x18, #48]
    cmp x1, #1
    b.hi identity_interrupt_park
    b.lo .Lreceived
    cmp x2, #1
    b.hi identity_interrupt_park
    b.lo .Lreceived
    add x16, x18, #80
    stlr x21, [x16]
    irq_all 80
    // Every sender has finished and every target has handled both interrupts.
    msr daifset, #2
    msr cntp_ctl_el0, xzr
    isb
    msr icc_igrpen1_el1, xzr
    isb
    dsb sy
identity_interrupt_groups_disabled:
    mrs x0, icc_igrpen1_el1
    str x0, [x18, #88]
    cbnz x0, identity_interrupt_park
    add x13, x24, #0x10000
.Ldrain:
    irq_deadline
    ldr w1, [x13, #0x300]
    ldr w2, [x13, #0x200]
    orr w0, w1, w2
    cbnz w0, .Ldrain
    stp x1, x2, [x18, #96]
    str w14, [x13, #0x41c]
    dsb sy
    ldr w0, [x13, #0x41c]
    cmp w0, w14
    b.ne identity_interrupt_park
    msr vbar_el1, x12
    isb
    mrs x0, vbar_el1
    cmp x0, x12
    b.ne identity_interrupt_park
    ret
.size identity_interrupt_el1,.-identity_interrupt_el1

identity_interrupt_handler:
    mrs x4, icc_iar1_el1
    cmp x4, #1020
    b.hs .Lirq_return
    cbz x4, .Lsgi
    cmp x4, #30
    b.ne identity_interrupt_park
    msr cntp_ctl_el0, xzr
    isb
    ldr x5, [x18, #56]
    cbnz x5, identity_interrupt_park
    mov x5, #1
    str x5, [x18, #56]
    b .Leoi
.Lsgi:
    ldr x5, [x18, #48]
    cbnz x5, identity_interrupt_park
    mov x5, #1
    str x5, [x18, #48]
.Leoi:
    dsb sy
    msr icc_eoir1_el1, x4
    isb
.Lirq_return:
    eret
identity_interrupt_park:
    msr daifset, #15
    wfi
    b identity_interrupt_park
.section .note.GNU-stack,"",%progbits
