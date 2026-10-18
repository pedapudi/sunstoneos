/* Publish immutable code and EL1 tables through their Nonsecure physical
 * addresses. Temporary EL3 mappings keep this code Secure and noncacheable;
 * maintenance uses a separate, nonexecutable Nonsecure alias. Only clean
 * inherited lines are admitted: this is not dirty runtime-data disposal. */
.section .text.native_ns_publication,"ax"
.balign 4
.global native_ns_publication
.type native_ns_publication,%function
native_ns_publication:
    stp x19, x20, [sp, #-32]!
    stp x21, x30, [sp, #16]
    ldr x19, =0x42002c20
    ldr x20, =0x42002a80  // Original controls saved by the enclosing probe.
    ldr x2, [x19]
    cbnz x2, .Lretain
    stp x0, x1, [x19, #8]
    mov x21, x1
    mov x2, #1
    str x2, [x19]
    mrs x2, sctlr_el3
    mov x3, #0x1005
    tst x2, x3
    b.ne .Lretain
    mrs x2, ctr_el0
    ubfx x2, x2, #16, #4
    cmp x2, #4
    b.ne .Lretain
    // Copy the established Secure table hierarchy. The temporary code block
    // changes to Secure Normal-NC. Stack/result stay Secure Device through L3.
    ldr x0, =0x421f0000
    ldr x1, =0x421f5000
    mov x2, #1024
.Lcopy:
    ldr x3, [x0], #8
    str x3, [x1], #8
    subs x2, x2, #1
    b.ne .Lcopy
    ldr x0, =0x421f5000
    ldr x1, =0x421f6003
    str x1, [x0, #8]
    // VA80000000..bfffffff -> NS PA40000000..7fffffff, NC/RW/XN.
    ldr x1, =0x0040000040000769
    str x1, [x0, #16]
    ldr x1, =0x421f6040  // L2 block containing41000000..411fffff.
    ldr x2, [x1]
    bic x2, x2, #0x20
    bic x2, x2, #0x1c
    orr x2, x2, #8
    str x2, [x1]
    ldr x1, =0x421f7000
.Lpublish_tables:
    dc cvac, x0             // These table-walk addresses are Secure.
    add x0, x0, #64
    cmp x0, x1
    b.lo .Lpublish_tables
    dsb sy
    ldr x0, =0x44ff00
    msr mair_el3, x0
    ldr x0, =0x80823019
    msr tcr_el3, x0
    ldr x0, =0x421f5000
    msr ttbr0_el3, x0
    isb
    tlbi alle3
    dsb sy
    tlbi alle3
    dsb sy
    isb
    ldr x0, [x20, #32]
    orr x0, x0, #1          // M only: C/I remain clear.
    msr sctlr_el3, x0
    isb
    mrs x0, ttbr0_el3
    str x0, [x19, #40]
    mrs x0, sctlr_el3
    str x0, [x19, #48]
    mrs x0, tcr_el3
    str x0, [x19, #56]
    mrs x0, mair_el3
    str x0, [x19, #64]
    ldr x0, =0x81000000
    ldr x1, =0x81200000
    mov x2, #0
.Lcode:
    dc ivac, x0             // Targets Nonsecure code, through the alias.
    add x2, x2, #1
    add x0, x0, #64
    cmp x0, x1
    b.lo .Lcode
    str x2, [x19, #24]
    ldr x0, =0x821f2000
    ldr x1, =0x821f4000
    mov x2, #0
.Ltables:
    dc ivac, x0             // Nonsecure EL1 root and L2 table pages.
    add x2, x2, #1
    add x0, x0, #64
    cmp x0, x1
    b.lo .Ltables
    str x2, [x19, #32]
    dsb sy
    ic iallu
    dsb sy
    isb
    // Compare one changed instruction through both physical address spaces.
    ldr x0, =native_el1_call
    ldr w2, [x0]
    mov x4, #0x40000000
    add x0, x0, x4
    ldr w3, [x0]
    stp x2, x3, [x19, #112]
    cmp w2, w3
    b.ne .Lretain
    isb
    mrs x0, cntpct_el0
    cmp x0, x21
    b.hs .Lretain
    ldr x0, [x20, #32]
    dsb sy
    msr sctlr_el3, x0
    isb
    tlbi alle3
    dsb sy
    tlbi alle3
    dsb sy
    ldr x0, [x20, #40]
    msr tcr_el3, x0
    ldr x0, [x20, #48]
    msr ttbr0_el3, x0
    ldr x0, [x20, #56]
    msr mair_el3, x0
    isb
.macro verify reg, offset
    mrs x0, \reg
    str x0, [x19, #(\offset + 40)]
    ldr x1, [x20, #\offset]
    cmp x0, x1
    b.ne .Lretain
.endm
    verify sctlr_el3, 32
    verify tcr_el3, 40
    verify ttbr0_el3, 48
    verify mair_el3, 56
    mov x0, #3
    dsb sy
    str x0, [x19]
    ldp x21, x30, [sp, #16]
    ldp x19, x20, [sp], #32
    ret
.Lretain:
    mov x0, #1
    str x0, [x19, #104]
    dsb sy
.Lpark:
    wfi
    b .Lpark
.size native_ns_publication,.-native_ns_publication
.section .note.GNU-stack,"",%progbits
