// Observe the complete image's primary preparation before GIC or CPU release.
// The builder replaces only the caller's BL, retaining the production callee.
.arch armv8-a
.section .text,"ax"
.global primary_publication_probe, primary_publication_return, primary_publication_park
.global primary_publication_after_prepare
primary_publication_probe:
    ldr x16, =0x42018000
    mov x17, #1
    str x17, [x16, #24]
    str x17, [x16, #8]
    mov x17, #256
    str x17, [x16, #16]
    ldr x17, =0x4137335052494d31
    str x17, [x16]
    // Bytes 32..47 contain the host nonce and must survive this path.
    mrs x17, CurrentEL
    str x17, [x16, #48]
    cmp x17, #12
    b.ne primary_publication_park
    mrs x17, mpidr_el1
    str x17, [x16, #56]
    mov x15, #0xc1000000
    bic x17, x17, x15
    cbnz x17, primary_publication_park
    mrs x17, daif
    and x17, x17, #0x3c0
    cmp x17, #0x3c0
    b.ne primary_publication_park
    // This revision requires no implementation-defined workaround writes.
    // Other revisions need their own restoration review before this probe.
    mrs x17, midr_el1
    str x17, [x16, #64]
    ldr x15, =0x412fd050
    cmp x17, x15
    b.ne primary_publication_park
    mrs x17, S3_0_C15_C3_1
    str x17, [x16, #72]
    cmp x17, #0x41
    b.ne primary_publication_park
    mrs x17, S3_0_C15_C3_0
    str x17, [x16, #80]
    ldr x15, =0x073fc237
    cmp x17, x15
    b.ne primary_publication_park
    mrs x17, cntfrq_el0
    str x17, [x16, #88]
    isb
    mrs x17, cntpct_el0
    str x17, [x16, #96]
    mrs x17, sctlr_el1
    str x17, [x16, #112]
    mrs x17, tcr_el1
    str x17, [x16, #120]
    mrs x17, sctlr_el3
    str x17, [x16, #128]
    mrs x17, tcr_el3
    str x17, [x16, #136]
    mrs x17, ttbr0_el3
    str x17, [x16, #144]
    mrs x17, mair_el3
    str x17, [x16, #152]
    mov x17, sp
    str x17, [x16, #224]
    mov x17, #2
    str x17, [x16, #24]
    dsb sy
    // x0..x3 still contain the actual admitted production arguments.
    ldr x16, =probe_production_prepare
    blr x16
primary_publication_after_prepare:
    ldr x16, =0x42018000
    mov x17, #3
    str x17, [x16, #24]
    isb
    mrs x17, cntpct_el0
    str x17, [x16, #104]
    mrs x17, sctlr_el1
    str x17, [x16, #160]
    mrs x17, tcr_el1
    str x17, [x16, #168]
    mrs x17, sctlr_el3
    str x17, [x16, #176]
    ldr x15, [x16, #128]
    cmp x17, x15
    b.ne primary_publication_park
    mrs x17, tcr_el3
    str x17, [x16, #184]
    ldr x15, [x16, #136]
    cmp x17, x15
    b.ne primary_publication_park
    mrs x17, ttbr0_el3
    str x17, [x16, #192]
    ldr x15, [x16, #144]
    cmp x17, x15
    b.ne primary_publication_park
    mrs x17, mair_el3
    str x17, [x16, #200]
    ldr x15, [x16, #152]
    cmp x17, x15
    b.ne primary_publication_park
    // The native helper restores EL3. Restore the bank changed by block_walks.
    ldr x14, [x16, #112]
    msr sctlr_el1, x14
    isb
    ldr x15, [x16, #120]
    msr tcr_el1, x15
    isb
    mrs x17, sctlr_el1
    str x17, [x16, #208]
    cmp x17, x14
    b.ne primary_publication_park
    mrs x17, tcr_el1
    str x17, [x16, #216]
    cmp x17, x15
    b.ne primary_publication_park
    mov x17, #4
    str x17, [x16, #24]
    dsb sy
primary_publication_return:
    // Same RVBAR/RMR sequence as the physically exercised returning probe.
    // No GIC or secondary power write has occurred on this execution path.
    ldr x0, =0x08001004
    mov w1, #0x20
    str w1, [x0]
    str wzr, [x0, #4]
    dsb sy
    mov x0, #2
    msr rmr_el3, x0
    isb
primary_publication_park:
    wfi
    b primary_publication_park
.ltorg
.section .note.GNU-stack,"",%progbits
