/* Seven simultaneous cached EL1 participants, with no stack accesses.
 * The boot CPU remains at EL3, so its first EL1 stack page is exclusively
 * borrowed. Native BSS initialization and NS publication cover this page.
 * All stores stop before each participant cleans the page in the NS address
 * space. TF-A flush_dcache_range uses DC CIVAC by line followed by DSB SY. */
.section .text.identity_coherence_el1,"ax"
.balign 4
.global identity_coherence_el1, identity_coherence_clean, identity_coherence_clean_done
.global identity_coherence_park
.type identity_coherence_el1,%function
identity_coherence_el1:
    // Preserve x20..x24: core, epoch, startup/diagnostic deadlines, redistributor.
    // Return x25..x29: completed, rounds, atomic count, payload, cleaned lines.
    ldr x19, =probe_production_el1_stacks
    mrs x0, ctr_el0
    ubfx x0, x0, #16, #4
    cmp x0, #4
    b.ne identity_coherence_park
    add x18, x19, #0x100
    add x18, x18, x20, lsl #7
    ldp x0, x1, [x18]
    ldr x2, [x18, #16]
    orr x0, x0, x1
    orr x0, x0, x2
    cbnz x0, identity_coherence_park
    ldr x0, [x19]
    ldr x1, [x19, #64]
    ldr x2, [x19, #128]
    orr x0, x0, x1
    orr x0, x0, x2
    cbnz x0, identity_coherence_park
    stlr x21, [x18]
.macro deadline
    isb
    mrs x0, cntpct_el0
    cmp x0, x22
    b.hs identity_coherence_park
.endm
.macro all_ready offset
    mov x15, #1
.Lpeer\@:
    add x16, x19, #0x100
    add x16, x16, x15, lsl #7
    add x16, x16, #\offset
.Lwait\@:
    deadline
    ldar x17, [x16]
    cmp x17, x21
    b.ne .Lwait\@
    add x15, x15, #1
    cmp x15, #8
    b.lo .Lpeer\@
.endm
    all_ready 0
    // Contended exclusive increments exercise actual inter-core monitors.
    add x14, x19, #64
    mov x13, #32
.Latomic:
    deadline
    ldaxr x1, [x14]
    add x1, x1, #1
    stlxr w2, x1, [x14]
    cbnz w2, .Latomic
    subs x13, x13, #1
    b.ne .Latomic
    add x16, x18, #8
    stlr x21, [x16]
    all_ready 8
    ldar x27, [x14]
    cmp x27, #224
    b.ne identity_coherence_park
    // Release/acquire turn passing must make the preceding payload visible.
    mov x26, #0
    sub x13, x20, #1
.Lturn:
    deadline
    ldar x1, [x19]
    cmp x1, x13
    b.ne .Lturn
    cbz x13, .Lfirst
    sub x2, x13, #1
    eor x2, x2, x21
    ldr x3, [x19, #128]
    cmp x3, x2
    b.ne identity_coherence_park
.Lfirst:
    eor x2, x13, x21
    str x2, [x19, #128]
    add x1, x13, #1
    stlr x1, [x19]
    add x13, x13, #7
    add x26, x26, #1
    cmp x26, #32
    b.lo .Lturn
.Lcomplete:
    deadline
    ldar x1, [x19]
    cmp x1, #224
    b.ne .Lcomplete
    ldr x28, [x19, #128]
    mov x2, #223
    eor x2, x2, x21
    cmp x28, x2
    b.ne identity_coherence_park
    str x27, [x18, #24]
    str x28, [x18, #32]
    str x26, [x18, #40]
#ifdef SUNSTONE_SECONDARY_INTERRUPTS
    mov x25, x30
    bl identity_interrupt_el1
    mov x30, x25
#endif
    add x16, x18, #16
    stlr x21, [x16]
    all_ready 16
    // All participants have made their final store. No Normal-memory store
    // follows this point, including during SMC restoration and shutdown.
    mov x16, x19
    add x17, x19, #4096
    mov x29, #0
identity_coherence_clean:
    dc civac, x16
    add x29, x29, #1
    add x16, x16, #64
    cmp x16, x17
    b.lo identity_coherence_clean
    dsb sy
identity_coherence_clean_done:
    mov x25, #1
    ret
identity_coherence_park:
    wfi
    b identity_coherence_park
.size identity_coherence_el1,.-identity_coherence_el1
.section .note.GNU-stack,"",%progbits
