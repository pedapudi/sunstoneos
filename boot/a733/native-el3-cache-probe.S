/* Core-1 EL3 adoption using the frozen production helpers. No cacheable store
 * occurs while translation is enabled: stack and report remain Secure Device.
 * This is not a general cache shutdown routine. */
.section .text.native_el3_cache_probe,"ax"
.balign 4
.global native_el3_cache_probe
.type native_el3_cache_probe,%function
native_el3_cache_probe:
    stp x19, x20, [sp, #-32]!
    stp x21, x30, [sp, #16]
    ldr x19, =0x42002a80
    ldr x2, [x19]
    cbnz x2, .Lrefuse
    str x0, [x19, #8]
    str x1, [x19, #16]
    mov x20, x1
    mov x2, #1
    str x2, [x19]
    mrs x2, sctlr_el3
    str x2, [x19, #32]
    mrs x2, tcr_el3
    str x2, [x19, #40]
    mrs x2, ttbr0_el3
    str x2, [x19, #48]
    mrs x2, mair_el3
    str x2, [x19, #56]
    mrs x2, scr_el3
    str x2, [x19, #64]
    bl probe_production_allow_ns_code
    cmp w0, #1
    b.ne .Lrefuse
    ldr x0, =probe_production_el3_root
    mov x1, x20
    bl probe_production_enable_el3
    str x0, [x19, #24]
    cmp w0, #1
    b.ne .Lrefuse
    mrs x2, sctlr_el3
    str x2, [x19, #72]
    mrs x2, tcr_el3
    str x2, [x19, #80]
    mrs x2, ttbr0_el3
    str x2, [x19, #88]
    mrs x2, mair_el3
    str x2, [x19, #96]
    mrs x2, scr_el3
    str x2, [x19, #104]
    mov x2, #2
    str x2, [x19]
    // No dirty Normal-memory lines exist from this interval. Complete Device
    // stores before disabling M/C/I. Original control has all three clear.
    ldr x2, [x19, #32]
    dsb sy
    msr sctlr_el3, x2
    isb
    mrs x3, sctlr_el3
    str x3, [x19, #112]
    cmp x3, x2
    b.ne .Lrefuse
    tlbi alle3
    dsb sy
    tlbi alle3
    dsb sy
    ic iallu
    dsb sy
    isb
    ldr x2, [x19, #40]
    msr tcr_el3, x2
    ldr x3, [x19, #48]
    msr ttbr0_el3, x3
    ldr x4, [x19, #56]
    msr mair_el3, x4
    ldr x5, [x19, #64]
    msr scr_el3, x5
    isb
    mrs x6, tcr_el3
    str x6, [x19, #120]
    cmp x6, x2
    b.ne .Lrefuse
    mrs x6, ttbr0_el3
    str x6, [x19, #128]
    cmp x6, x3
    b.ne .Lrefuse
    mrs x6, mair_el3
    str x6, [x19, #136]
    cmp x6, x4
    b.ne .Lrefuse
    mrs x6, scr_el3
    str x6, [x19, #144]
    cmp x6, x5
    b.ne .Lrefuse
    mov x2, #3
    dsb sy
    str x2, [x19]
    mov w0, #1
    b .Lreturn
.Lrefuse:
    mov x2, #1
    str x2, [x19, #152]
    dsb sy
    mov w0, #0
.Lreturn:
    ldp x21, x30, [sp, #16]
    ldp x19, x20, [sp], #32
    ret
.size native_el3_cache_probe,.-native_el3_cache_probe
.section .note.GNU-stack,"",%progbits
