/* Boot-only publication of clean, externally owned Nonsecure aliases.
 * Secure writes have already reached PoC: the tables because core 0 cleaned and
 * invalidated them and then wrote them with Device stores, code and initial RAM
 * through the DC CVAC publication in continue.c. No secondary or DMA writer may run.
 * This is not a dirty-data discard or a general warm OS replacement path. */
.section .text.a733_supervised_ns_publish,"ax"
.balign 4
.global a733_supervised_ns_publish
.type a733_supervised_ns_publish,%function
a733_supervised_ns_publish:
    // x0 is the absolute startup deadline. The caller uses a Secure Device stack.
    mrs x1, CurrentEL
    cmp x1, #12
    b.ne .Lrefuse
    mrs x1, mpidr_el1
    mov x2, #0xc1000000
    bic x1, x1, x2
    cbnz x1, .Lrefuse
    mrs x1, daif
    and x1, x1, #0x3c0
    cmp x1, #0x3c0
    b.ne .Lrefuse
    mrs x1, sctlr_el3
    mov x2, #0x1005
    movk x2, #0x200, lsl #16
    tst x1, x2
    b.ne .Lrefuse
    mrs x1, ctr_el0
    ubfx x1, x1, #16, #4
    cmp x1, #4
    b.ne .Lrefuse
    isb
    mrs x1, cntpct_el0
    cmp x1, x0
    b.hs .Lrefuse
    stp x19, x20, [sp, #-64]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x30, [sp, #48]
    mov x23, x0
    mov x24, #0x40000000
    mrs x19, sctlr_el3
    mrs x20, tcr_el3
    mrs x21, ttbr0_el3
    mrs x22, mair_el3
    adrp x0, a733_cached_el3_l1
    add x0, x0, :lo12:a733_cached_el3_l1
    adrp x1, a733_supervised_publish_l1
    add x1, x1, :lo12:a733_supervised_publish_l1
    mov x2, #512
.Lcopy_l1:
    ldr x3, [x0], #8
    str x3, [x1], #8
    subs x2, x2, #1
    b.ne .Lcopy_l1
    adrp x0, a733_cached_el3_l2
    add x0, x0, :lo12:a733_cached_el3_l2
    mov x2, #512
.Lcopy_l2:
    ldr x3, [x0], #8
    str x3, [x1], #8
    subs x2, x2, #1
    b.ne .Lcopy_l2
    adrp x0, a733_supervised_publish_l1
    add x0, x0, :lo12:a733_supervised_publish_l1
    add x1, x0, #4096
    orr x2, x1, #3
    str x2, [x0, #8]
    // VA80000000..bfffffff -> NS PA40000000..7fffffff, NC/RW/XN.
    ldr x2, =0x0040000040000769
    str x2, [x0, #16]
    // Execute Secure Normal-NC code while maintaining the Nonsecure alias.
    ldr x2, [x1, #64]
    bic x2, x2, #0x20
    bic x2, x2, #0x1c
    orr x2, x2, #8
    str x2, [x1, #64]
    add x1, x0, #8192
.Lpublish_tables:
    dc cvac, x0
    add x0, x0, #64
    cmp x0, x1
    b.lo .Lpublish_tables
    dsb sy
    ldr x0, =0x44ff00
    msr mair_el3, x0
    ldr x0, =0x80823019
    msr tcr_el3, x0
    adrp x0, a733_supervised_publish_l1
    add x0, x0, :lo12:a733_supervised_publish_l1
    msr ttbr0_el3, x0
    isb
    tlbi alle3
    dsb sy
    tlbi alle3
    dsb sy
    isb
    orr x0, x19, #1
    msr sctlr_el3, x0
    isb
    // Code, initialized data, BSS and EL1 stacks. No device pool in this image.
    mov x0, #0x81000000
    adrp x1, __bss_end
    add x1, x1, :lo12:__bss_end
    add x1, x1, x24
.Linitial_ram:
    dc ivac, x0
    add x0, x0, #64
    cmp x0, x1
    b.lo .Linitial_ram
    // The EL1 root and L2 need maintenance in their Nonsecure address space.
    adrp x0, a733_cached_el1_l1
    add x0, x0, :lo12:a733_cached_el1_l1
    add x0, x0, x24
    add x1, x0, #4096
.Lel1_root:
    dc ivac, x0
    add x0, x0, #64
    cmp x0, x1
    b.lo .Lel1_root
    adrp x0, a733_cached_el1_l2
    add x0, x0, :lo12:a733_cached_el1_l2
    add x0, x0, x24
    add x1, x0, #4096
.Lel1_l2:
    dc ivac, x0
    add x0, x0, #64
    cmp x0, x1
    b.lo .Lel1_l2
    dsb sy
    ic iallu
    dsb sy
    isb
    mrs x0, cntpct_el0
    cmp x0, x23
    cset x25, lo
    // Return to the exact entry controls before any caller access.
    dsb sy
    msr sctlr_el3, x19
    isb
    tlbi alle3
    dsb sy
    tlbi alle3
    dsb sy
    msr tcr_el3, x20
    msr ttbr0_el3, x21
    msr mair_el3, x22
    isb
.macro verify reg, expected
    mrs x0, \reg
    cmp x0, \expected
    b.ne .Lretain
.endm
    verify sctlr_el3, x19
    verify tcr_el3, x20
    verify ttbr0_el3, x21
    verify mair_el3, x22
    mov x0, x25
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x30, [sp, #48]
    ldp x19, x20, [sp], #64
    ret
.Lrefuse:
    mov x0, #0
    ret
.Lretain:
    wfi
    b .Lretain
.size a733_supervised_ns_publish,.-a733_supervised_ns_publish
.section .bss.a733_cached_tables,"aw",%nobits
.balign 4096
.global a733_supervised_publish_l1
a733_supervised_publish_l1:
    .skip 8192
.section .note.GNU-stack,"",%progbits
