#include "secondary-identity-probe.h"
.arch armv8-a
.section .text,"ax"
.global identity_primary_gate, identity_target, identity_park, identity_target_terminal
identity_primary_gate:
    bl identity_primary_run
    cmp w0, #1
    b.ne identity_park
    // Displaced first instruction of the physically exercised return sequence.
    ldr x0, =0x08001004
    ldr x17, =identity_original_return_next
    br x17
identity_target:
    // Production entry has observed RELEASE and supplied core/epoch in x0/x1.
    cmp x0, #IDENTITY_FIRST_CORE
    b.lo identity_park
    cmp x0, #7
    b.hi identity_park
    ldr x16, =IDENTITY_SLOTS
    add x16, x16, x0, lsl #8
    str x1, [x16]
    str x0, [x16, #8]
    mrs x17, CurrentEL
    cmp x17, #12
    b.ne identity_park
    mrs x17, midr_el1
    str x17, [x16, #16]
    ubfx x15, x17, #24, #8
    cmp x15, #0x41
    b.ne identity_park
    ubfx x15, x17, #4, #12
    cmp x0, #6
    b.hs 1f
    cmp x15, #0xd05
    b.ne identity_park
    b 2f
1:  cmp x15, #0xd0b
    b.ne identity_park
2:  mrs x17, mpidr_el1
    str x17, [x16, #24]
    mrs x17, sctlr_el3
    str x17, [x16, #32]
    ldr x15, =0x02001005
    tst x17, x15
    b.ne identity_park
    mrs x17, sctlr_el1
    str x17, [x16, #40]
    mrs x17, tcr_el1
    str x17, [x16, #48]
    mrs x17, S3_0_C15_C3_1
    str x17, [x16, #56]
    mrs x17, S3_0_C15_C3_0
    str x17, [x16, #64]
    mrs x17, clidr_el1
    str x17, [x16, #72]
    mrs x17, id_aa64mmfr0_el1
    str x17, [x16, #80]
    mrs x17, S3_0_C15_C2_7
    str x17, [x16, #88]
    tbnz x17, #0, identity_park
    ldr x14, =0x07050200
    add x14, x14, x0, lsl #2
    ldr w12, [x14]
    str x12, [x16, #104]
    ldr w13, [x14, #32]
    str x13, [x16, #112]
    orr w15, w12, w13
    tbnz w15, #0, identity_park
#ifdef SUNSTONE_SECONDARY_LOCAL
    // Preserve the target identities across actual production C functions.
    mov x19, x16
    mov x20, x0
    mov x21, x1
    msr daifset, #15
    ldr x4, =a733_cached_el3_vectors
    msr vbar_el3, x4
    msr tpidr_el3, xzr
    ldr x4, =24000000
    msr cntfrq_el0, x4
    msr cntps_ctl_el1, xzr
    msr cntp_ctl_el0, xzr
    msr cntv_ctl_el0, xzr
    isb
    bl prepare_core
    mov x0, x20
    mov x1, x21
    ldr x2, =IDENTITY_REPORT
    ldr x2, [x2, #32]
    ldr x3, =0x42018a00
    add x3, x3, x20, lsl #9
    bl secondary_local_probe
    cbz w0, identity_park
    // Return2 denotes a restored refusal; its report remains distinct from success.
    ldr x17, [x19, #40]
    msr sctlr_el1, x17
    isb
    mrs x15, sctlr_el1
    cmp x15, x17
    b.ne identity_park
    ldr x17, [x19, #48]
    msr tcr_el1, x17
    isb
    mrs x15, tcr_el1
    cmp x15, x17
    b.ne identity_park
    mov x16, x19
    mov x0, x20
    mov x1, x21
    ldr x14, =0x07050200
    add x14, x14, x0, lsl #2
    ldr x12, [x16, #104]
    ldr x13, [x16, #112]
    ldr x17, [x16, #88]
#endif
    dsb sy
    mov x15, #1
    str x15, [x16, #120]
    dsb sy
    sev
3:  dsb sy
    ldr x15, [x16, #128]
    cmp x15, x1
    b.ne 3b
    ldr w15, [x14]
    cmp w15, w12
    b.ne identity_park
    ldr w15, [x14, #32]
    cmp w15, w13
    b.ne identity_park
    orr x17, x17, #1
    str x17, [x16, #96]
    msr S3_0_C15_C2_7, x17
    // TF-A Cortex-A76 core_pwr_dwn: erratum2743102 requires DSB before ISB.
    dsb sy
    isb
    bic w13, w13, #1
    str w13, [x14, #32]
    dsb sy
    orr w12, w12, #2
    str w12, [x14]
    dsb sy
    str x1, [x16, #136]
    dsb sy
identity_target_terminal:
    dsb sy
    wfi
    b identity_target_terminal
identity_park:
    wfi
    b identity_park
.ltorg
.section .note.GNU-stack,"",%progbits
