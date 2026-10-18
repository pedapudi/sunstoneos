// Fixed native-image checkpoints. The original startup owns stack and BSS.
.arch armv8-a
.section .text,"ax"
.global boot_checkpoint, cached_checkpoint, stop_checkpoint, park_checkpoint, fault_checkpoint
.global write_checkpoint, failed_checkpoint
.global block_checkpoint, gic_begin, prearm_checkpoint, release_checkpoint, secondary_checkpoint
.global next_checkpoint, ready_checkpoint, continue_write_checkpoint
boot_checkpoint:
    ldr x16, =0x42002000
    mov x17, #1
    str x17, [x16, #32]
    mov x17, sp
    str x17, [x16, #40]
    sub sp, sp, #0xb0           // Displaced native instruction.
    ldr x17, =0x410000d4
    br x17
cached_checkpoint:
    ldr x12, =0x42002280
    ldr w9, [x0]
    cmp w9, #1
    b.ne configuration_blocked
    ldr x17, [x12]
    cbz x17, configuration_snapshot
    cmp x17, #1
    b.ne configuration_blocked
    b configuration_allow
configuration_snapshot:
    ldr x13, =0x08002000
    add x17, x13, #4
    cmp x1, x17
    b.ne configuration_blocked
    ldr w17, =0x41002000
    cmp w2, w17
    b.ne configuration_blocked
    str x9, [x12, #8]
    str x13, [x12, #16]
    dsb sy
    ldr w17, [x13]
    str x17, [x12, #24]
    ldr w17, [x13, #4]
    str x17, [x12, #32]
    ldr w17, [x13, #8]
    str x17, [x12, #40]
    dsb sy
    mov x17, #1
    str x17, [x12]            // Every original precedes any admitted write.
configuration_allow:
    ldr x9, [x12, #104]
    ldr x13, =0x08002000
    cbz x9, configuration_low
    cmp x9, #1
    b.eq configuration_high
    cmp x9, #2
    b.hi activation_allow
    b.ne configuration_blocked
    cmp x1, x13
    b.ne configuration_blocked
    ldr w17, [x12, #24]
    orr w17, w17, #1
    cmp w2, w17
    b.ne configuration_blocked
    b configuration_permit
configuration_low:
    add x17, x13, #4
    cmp x1, x17
    b.ne configuration_blocked
    ldr w17, =0x41002000
    cmp w2, w17
    b.ne configuration_blocked
    b configuration_permit
configuration_high:
    add x17, x13, #8
    cmp x1, x17
    b.ne configuration_blocked
    cbnz w2, configuration_blocked
    b configuration_permit
activation_allow:
    ldr x13, =0x42002400
    cmp x9, #3
    b.ne activation_existing
    ldr x17, =0x07050224
    cmp x1, x17
    b.ne configuration_blocked
    ldr x10, =0x42002240
    ldr w11, [x10, #32]
    orr w11, w11, #1
    cmp w2, w11
    b.ne configuration_blocked
    ldr x17, [x10]
    str x17, [x13, #32]
    ldr x17, [x10, #24]
    str x17, [x13, #40]
    ldr x17, [x10, #32]
    str x17, [x13, #48]
    ldr x10, =0x42004018
    ldr x17, [x10]
    str x17, [x13, #8]
    mov x17, #1
    str x17, [x13]            // Any attempted activation forbids ordinary rollback.
    b configuration_permit
activation_existing:
    ldr x17, [x13]
    cmp x17, #1
    b.ne configuration_blocked
    ldr x17, =0x07050204
    cmp x9, #4
    b.eq activation_control_on
    cmp x9, #5
    b.eq activation_control_clear
    cmp x9, #6
    b.ne configuration_blocked
    add x17, x17, #32
    ldr w11, [x13, #48]
    b activation_match
activation_control_on:
    ldr w11, [x13, #40]
    orr w11, w11, #1
    b activation_match
activation_control_clear:
    ldr w11, [x13, #40]
activation_match:
    cmp x1, x17
    b.ne configuration_blocked
    cmp w2, w11
    b.ne configuration_blocked
configuration_permit:
    add x9, x9, #1
    str x9, [x12, #104]       // Includes an admitted attempt that may fault.
    mrs x8, CurrentEL         // Displaced native callback instruction.
    ldr x17, =0x4100a1e8
    br x17
configuration_blocked:
    ldr x16, =0x42002000
    str x1, [x16, #96]
    mov w17, w2
    str x17, [x16, #104]
    mov x17, #9
    b publish
release_checkpoint:
    ldr x12, =0x42002400
    cmp w1, #1
    b.ne park
    ldr x17, [x12, #8]
    cmp x2, x17
    b.ne park
    ldr x17, [x12]
    cmp x17, #1
    b.ne park
    ldr x13, =0x420022e8
    ldr x17, [x13]            // Configuration admitted attempts (offset104).
    cmp x17, #7
    b.ne park
    ldr x13, =0x4200ebb0
    ldr w17, [x13]           // Native power state at power+104.
    cmp w17, #2
    b.ne park
    ldr x13, =0x4200efa4
    ldr w17, [x13]           // Native owner phase at owner+220.
    cmp w17, #4
    b.ne park
    ldr x13, =0x07053008
    ldr w17, [x13]
    str x17, [x12, #56]
    and w17, w17, #15
    cmp w17, #8
    b.ne park
    ldr x13, =0x07050204
    ldr w10, [x13]
    ldr w11, [x13, #32]
    orr w17, w10, w11
    tbnz w17, #0, park
    ldr x12, =0x42002500
    ldr x13, =0x03460000
    dsb sy
    mov x8, #0
    mov x9, #0
    mov x14, #0
rdist_scan:
    ldr x10, [x13, #8]
    add x17, x12, #392
    str x10, [x17, x8, lsl #3]
    tbnz x10, #1, rdist_scan_refused
    lsr x11, x10, #32
    ldr x17, =0x700
    bic x17, x11, x17
    cbnz x17, rdist_scan_refused
    lsr x11, x11, #8
    mov x17, #1
    lsl x17, x17, x11
    tst x9, x17
    b.ne rdist_scan_refused
    orr x9, x9, x17
    cmp x11, #1
    csel x14, x13, x14, eq
    ubfx x11, x10, #4, #1
    cmp x8, #7
    cset x17, eq
    cmp x11, x17
    b.ne rdist_scan_refused
    add x8, x8, #1
    add x13, x13, #0x20, lsl #12
    cmp x8, #8
    b.ne rdist_scan
    cmp x9, #255
    b.ne rdist_scan_refused
    cbz x14, rdist_scan_refused
    str x14, [x12, #16]
    ldr w17, [x14]
    str x17, [x12, #24]
    tbnz w17, #0, rdist_ctlr_refused // EnableLPIs must be clear.
    tbnz w17, #3, rdist_ctlr_refused // RWP must be clear.
    ldr w17, [x14, #20]
    str x17, [x12, #32]
    and w17, w17, #6
    cmp w17, #6              // Restore only an initially asleep redistributor.
    b.ne rdist_waker_refused
    add x14, x14, #0x10, lsl #12
    ldr w17, [x14, #0x100]
    str x17, [x12, #40]
    ldr w17, [x14, #0x200]
    str x17, [x12, #48]
    cbnz w17, rdist_events_refused
    ldr w17, [x14, #0x300]
    str x17, [x12, #56]
    cbnz w17, rdist_events_refused
    ldr w17, [x14, #0x80]
    str x17, [x12, #64]
    ldr w17, [x14, #0xd00]
    str x17, [x12, #72]
    ldr w17, [x14, #0xc04]
    str x17, [x12, #80]
    ldr w17, [x14, #0x400]
    str x17, [x12, #88]
    dsb sy
    mov x17, #1
    str x17, [x12]
    sub w8, w1, #1           // Displaced native release instruction.
    ldr x17, =0x410036f0
    br x17
rdist_scan_refused:
    mov x17, #1
    b rdist_refused
rdist_ctlr_refused:
    mov x17, #2
    b rdist_refused
rdist_waker_refused:
    mov x17, #3
    b rdist_refused
rdist_events_refused:
    mov x17, #4
rdist_refused:
    str x17, [x12, #8]       // State remains zero: no RELEASE or GICR mutation.
    b activation_request
next_checkpoint:
    cmp w23, #1
    b.ne park               // Never initialize/activate core2.
    ldr x12, =0x42002500
    ldr x13, =0x4200e100
    ldr x15, =0x4200e008
    ldr x15, [x15]
    isb
    mrs x14, cntpct_el0
ready_wait:
    isb
    mrs x17, cntpct_el0
    cmp x17, x14
    b.lo park
    cmp x17, x15
    b.hs park
    mov x14, x17
    dsb sy
    ldr x17, [x12]
    cmp x17, #6
    b.eq park
    cmp x17, #3
    b.ne ready_wait
    ldr x17, [x13, #16]
    cmp x17, #2
    b.ne park
    ldr x14, =0x42002400
    ldr x2, [x14, #8]
    ldr x17, [x13]
    cmp x17, x2
    b.ne park
activation_request:
    ldr x12, =0x42002400
    isb
    mrs x14, cntpct_el0
    ldr x17, =2400000
    adds x15, x14, x17
    b.cs park
    str x15, [x12, #64]
    mov x17, #2
    str x17, [x12]
    str x2, [x12, #16]
    dsb sy
    sev
activation_wait_off:
    isb
    mrs x17, cntpct_el0
    cmp x17, x14
    b.lo park
    cmp x17, x15
    b.hs park
    mov x14, x17
    dsb sy
    ldr x10, [x12, #24]
    ldr x11, [x12, #8]
    cmp x10, x11
    b.ne activation_wait_off
    ldr x13, =0x07053008
    ldr w17, [x13]
    str x17, [x12, #56]
    tst w17, #15
    b.ne activation_wait_off
    ldr x13, =0x42002500
    ldr x17, [x13]
    cbz x17, rdist_return_refusal
    cmp x17, #5
    b.ne park
    mov x17, #12
    b rdist_return
rdist_return_refusal:
    mov x17, #11
rdist_return:
    mov x13, #3
    str x13, [x12]
    ldr x16, =0x42002000
    b publish
secondary_checkpoint:
    ldr x12, =0x42002500
    ldr x17, [x12]
    cmp x17, #1
    b.ne secondary_unreleased
secondary_native_wait:
    wfe                     // Displaced wait; native code observes RELEASE.
    ldr x17, =0x410020d0
    br x17
secondary_unreleased:
    cmp x20, #1
    b.ne park
    mrs x0, CurrentEL
    cmp x0, #12
    b.ne park
    ldr x12, =0x42002400
    str x0, [x12, #136]
    mrs x0, midr_el1
    str x0, [x12, #144]
    ldr x1, =0x412fd050
    cmp x0, x1
    b.ne park
    mrs x0, sctlr_el3
    str x0, [x12, #152]
    ldr x1, =0x02001005
    tst x0, x1
    b.ne park
    mrs x0, mpidr_el1
    str x0, [x12, #160]
secondary_wait_request:
    ldr x12, =0x42002500
    dsb sy
    ldr x17, [x12]
    cmp x17, #1
    b.eq secondary_native_validate
    ldr x17, =0x41002164
    blr x17                  // Original validator; preserves x20/x21/x24/x25.
    cbnz x0, park
    ldr x0, [x21, #24]
    cmp x0, #1
    b.eq secondary_private_armed
    cmp x0, #2
    b.ne park
    ldr x12, =0x42002500
    dsb sy
    ldr x17, [x12]
    cmp x17, #1
    b.ne park
secondary_native_validate:
    ldr x17, =0x410020d0
    br x17                  // Native validator observes the actual RELEASE.
secondary_private_armed:
    ldr x12, =0x42002400
    ldr x0, [x12, #8]
    cmp x0, x25
    b.ne park
    dsb sy
    ldr x0, [x12, #16]
    cmp x0, x25
    b.ne secondary_wait_request
secondary_power_off:
    mrs x0, S3_0_C15_C2_7
    str x0, [x12, #72]
    tbnz x0, #0, park
    ldr x13, =0x07050224
    ldr w1, [x13]
    str x1, [x12, #88]
    ldr w2, [x13, #-32]
    str x2, [x12, #96]
    orr w3, w1, w2
    tbnz w3, #0, park
    mov x17, #1
    str x17, [x12, #128]
    orr x0, x0, #1
    str x0, [x12, #80]
    msr S3_0_C15_C2_7, x0
    isb
    bic w1, w1, #1
    str w1, [x13]
    dsb sy
    orr w2, w2, #2
    str w2, [x13, #-32]
    dsb sy
    str x25, [x12, #24]       // Intent only; primary independently requires PPU OFF.
    dsb sy
secondary_terminal:
    dsb sy
    wfi
    b secondary_terminal
continue_write_checkpoint:
    ldr w8, [x0, #100]
    cmp w8, #1
    b.ne target_failed
    ldr x12, =0x42002500
    ldr x17, [x12]
    cmp x17, #1
    b.eq continue_write_admitted
    cmp x17, #2
    b.ne target_failed
continue_write_admitted:
    ldr x13, [x12, #16]
    ldr x17, [x0, #72]
    cmp x13, x17
    b.ne target_failed
    ldr x8, [x12, #248]
    cmp x8, #10
    b.hs target_failed
    adr x17, continue_phases
    ldr w10, [x17, x8, lsl #2]
    ldr w9, [x0, #108]
    cmp w9, w10
    b.ne target_failed
    cbz x8, continue_control
    cmp x8, #1
    b.eq continue_waker
    add x13, x13, #0x10, lsl #12
    cmp x8, #2
    b.eq continue_disable
    cmp x8, #3
    b.eq continue_pending
    cmp x8, #4
    b.eq continue_active
    cmp x8, #5
    b.eq continue_group
    cmp x8, #6
    b.eq continue_modifier
    cmp x8, #7
    b.eq continue_config
    cmp x8, #8
    b.eq continue_priority
    add x13, x13, #0x100
    mov w10, #1
    b continue_match
continue_control:
    ldr x13, =0x07050204
    ldr x17, =0x42002400
    ldr w10, [x17, #40]
    bic w10, w10, #2
    b continue_match
continue_waker:
    add x13, x13, #20
    ldr w10, [x12, #32]
    bic w10, w10, #2
    b continue_match
continue_disable:
    add x13, x13, #0x180
    b continue_all
continue_pending:
    ldr w17, [x13, #0x200]
    cbnz w17, target_failed
    add x13, x13, #0x280
    b continue_all
continue_active:
    ldr w17, [x13, #0x300]
    cbnz w17, target_failed
    add x13, x13, #0x380
    b continue_all
continue_group:
    add x13, x13, #0x80
continue_all:
    mov w10, #-1
    b continue_match
continue_modifier:
    add x13, x13, #0xd00
    mov w10, #0
    b continue_match
continue_config:
    add x13, x13, #0xc04
    ldr w10, [x12, #80]
    ldr w17, =0xd7ffffff
    and w10, w10, w17
    b continue_match
continue_priority:
    add x13, x13, #0x400
    ldr w10, [x12, #88]
    and w10, w10, #0xffffff00
    orr w10, w10, #0xf0
continue_match:
    cmp x1, x13
    b.ne target_failed
    cmp w2, w10
    b.ne target_failed
    add x8, x8, #1
    str x8, [x12, #248]
    mov x17, #2
    str x17, [x12]           // Snapshot complete; native mutation now permitted.
    ldr w8, [x0, #100]       // Displaced native callback instruction.
    ldr x17, =0x41005654
    br x17
continue_phases:
    .word 4,8,10,12,13,15,16,18,20,21
ready_checkpoint:
    cmp w20, #1
    b.ne target_failed
    ldr x12, =0x42002500
    ldr x17, [x12]
    cmp x17, #2
    b.ne target_failed
    ldr x17, [x12, #248]
    cmp x17, #10
    b.ne target_failed
    ldr w17, [sp, #0xb0]
    cmp w17, #2
    b.ne target_failed
    ldr x17, [sp, #0x88]
    ldr x13, [x12, #16]
    cmp x17, x13
    b.ne target_failed
    mov x17, sp
    str x17, [x12, #104]
    mrs x17, sctlr_el1
    str x17, [x12, #128]
    mrs x17, tcr_el1
    str x17, [x12, #136]
    add x13, sp, #0x40
    add x14, x12, #256
    mov x15, #17
copy_continue:
    ldr x17, [x13], #8
    str x17, [x14], #8
    subs x15, x15, #1
    b.ne copy_continue
    mov x25, x19             // Native local_boot epoch; no further C return.
    ldr x21, =0x42005100
    ldr x24, =0x42005180
    ldr x13, =0x42002400
    ldr x17, [x13, #8]
    cmp x25, x17
    b.ne target_failed
    mrs x17, CurrentEL
    str x17, [x13, #136]
    cmp x17, #12
    b.ne target_failed
    mrs x17, midr_el1
    str x17, [x13, #144]
    ldr x14, =0x412fd050
    cmp x17, x14
    b.ne target_failed
    mrs x17, sctlr_el3
    str x17, [x13, #152]
    ldr x14, =0x02001005
    tst x17, x14
    b.ne target_failed
    mrs x17, mpidr_el1
    str x17, [x13, #160]
    dsb sy
    mov x17, #3
    str x17, [x12]
ready_shutdown_wait:
    ldr x17, =0x41002164
    blr x17
    cbnz x0, target_failed
    ldr x17, [x21, #24]
    cmp x17, #2
    b.ne target_failed
    ldr x13, =0x42002400
    dsb sy
    ldr x17, [x13, #16]
    cmp x17, x25
    b.ne ready_shutdown_wait
    ldr x12, =0x42002500
    mov x17, #4
    str x17, [x12]           // Guard all target cleanup accesses and waits.
    isb
    mrs x14, cntpct_el0
    ldr x17, =2400000
    adds x15, x14, x17
    b.cs target_failed
    str x15, [x12, #96]
    mrs x0, icc_sre_el3
    str x0, [x12, #192]
    orr x0, x0, #7
    str x0, [x12, #200]
    msr icc_sre_el3, x0
    isb
    mrs x17, icc_sre_el3
    and x17, x17, #7
    cmp x17, #7
    b.ne target_failed
    mrs x0, icc_igrpen0_el1
    mrs x1, icc_igrpen1_el3
    str x0, [x12, #208]
    str x1, [x12, #216]
    bic x0, x0, #1
    bic x1, x1, #3
    msr icc_igrpen0_el1, x0
    msr icc_igrpen1_el3, x1
    isb
    dsb sy
    mrs x0, icc_igrpen0_el1
    str x0, [x12, #224]
    tbnz x0, #0, target_failed
    mrs x0, icc_igrpen1_el3
    str x0, [x12, #232]
    tst x0, #3
    b.ne target_failed
    ldr x13, [x12, #16]
    add x11, x13, #0x10, lsl #12
    mov w17, #-1
    str w17, [x11, #0x180]
    dsb sy
    bl rdist_wait_rwp
    bl rdist_require_no_events
    ldr w17, [x12, #64]
    str w17, [x11, #0x80]
    ldr w17, [x12, #72]
    str w17, [x11, #0xd00]
    ldr w17, [x12, #80]
    str w17, [x11, #0xc04]
    ldr w17, [x12, #88]
    str w17, [x11, #0x400]
    dsb sy
    bl rdist_wait_rwp
target_restore_check:
    ldr w10, [x11, #0x80]
    str x10, [x12, #160]
    ldr w17, [x12, #64]
    cmp w10, w17
    b.ne target_failed
    ldr w10, [x11, #0xd00]
    str x10, [x12, #168]
    ldr w17, [x12, #72]
    cmp w10, w17
    b.ne target_failed
    ldr w10, [x11, #0xc04]
    str x10, [x12, #176]
    ldr w17, [x12, #80]
    cmp w10, w17
    b.ne target_failed
    ldr w10, [x11, #0x400]
    str x10, [x12, #184]
    ldr w17, [x12, #88]
    cmp w10, w17
    b.ne target_failed
    ldr w17, [x12, #40]
    str w17, [x11, #0x100]
    dsb sy
    bl rdist_wait_rwp
    ldr w10, [x11, #0x100]
    str x10, [x12, #152]
    ldr w17, [x12, #40]
    cmp w10, w17
    b.ne target_failed
    bl rdist_require_no_events
    ldr w17, [x12, #32]
    str w17, [x13, #20]
    dsb sy
    bl rdist_wait_asleep
    str x10, [x12, #144]
    ldr w17, [x12, #32]
    cmp w10, w17
    b.ne target_failed
    ldr w10, [x13]
    ldr w17, [x12, #24]
    cmp w10, w17
    b.ne target_failed
    dsb sy
    mov x17, #5
    str x17, [x12]
    ldr x12, =0x42002400
    b secondary_power_off
rdist_require_no_events:
    ldr w17, [x11, #0x200]
    cbnz w17, target_failed
    ldr w17, [x11, #0x300]
    cbnz w17, target_failed
    ret
rdist_wait_rwp:
    isb
    mrs x17, cntpct_el0
    cmp x17, x14
    b.lo target_failed
    cmp x17, x15
    b.hs target_failed
    mov x14, x17
    ldr w10, [x13]
    tbnz w10, #3, rdist_wait_rwp
    ret
rdist_wait_asleep:
    isb
    mrs x17, cntpct_el0
    cmp x17, x14
    b.lo target_failed
    cmp x17, x15
    b.hs target_failed
    mov x14, x17
    ldr w10, [x13, #20]
    tbz w10, #2, rdist_wait_asleep
    ret
target_block:
    cmp x17, #1
    b.ne park
    ldr x17, =0x42002500
    mrs x16, sctlr_el1
    str x16, [x17, #112]
    mrs x16, tcr_el1
    str x16, [x17, #120]
    mrs x16, tcr_el1          // Displaced native block_walks instruction.
    ldr x17, =0x41008268
    br x17
target_failed:
    ldr x17, =0x42002500
    mrs x16, esr_el3
    str x16, [x17, #456]
    mrs x16, elr_el3
    str x16, [x17, #464]
    mrs x16, far_el3
    str x16, [x17, #472]
    str x1, [x17, #480]
    str x2, [x17, #488]
    mov x16, #6
    str x16, [x17]
    dsb sy
    b park                  // Target never publishes primary report or executes RMR.

prearm_checkpoint:
    ldr x12, =0x42002240
    mov w9, w1
    str x9, [x12]
    sub w10, w9, #1
    cmp w10, #6
    b.hi prearm_invalid
    isb
    mrs x15, cntpct_el0
    str x15, [x12, #56]
    ldr x13, =0x4200e008
    ldr x13, [x13]
    cmp x15, x13
    b.hs prearm_late
    ldr x8, =0x07052008
    add x8, x8, x9, lsl #12
    dsb sy
    ldr w10, [x8]
    dsb sy
    str x10, [x12, #16]
    tst w10, #15
    b.ne prearm_not_off
    ldr x11, =0x07050200
    add x11, x11, x9, lsl #2
    dsb sy
    ldr w10, [x11]
    dsb sy
    str x10, [x12, #24]
    tbnz w10, #0, prearm_control
    dsb sy
    ldr w10, [x11, #32]
    dsb sy
    str x10, [x12, #32]
    tbnz w10, #0, prearm_mode
    dsb sy
    ldr w10, [x8]
    dsb sy
    str x10, [x12, #40]
    tst w10, #15
    b.ne prearm_changed
    isb
    mrs x17, cntpct_el0
    str x17, [x12, #56]
    cmp x17, x15
    b.lo prearm_late
    cmp x17, x13
    b.hs prearm_late
    mov x17, #1
    str x17, [x12, #48]
    mov w0, wzr               // Displaced native arm instruction; w1/x2 retained.
    ldr x17, =0x410034d8
    br x17
prearm_invalid:
    mov x17, #1
    b prearm_refused
prearm_not_off:
    mov x17, #2
    b prearm_refused
prearm_control:
    mov x17, #3
    b prearm_refused
prearm_mode:
    mov x17, #4
    b prearm_refused
prearm_changed:
    mov x17, #5
    b prearm_refused
prearm_late:
    mov x17, #6
prearm_refused:
    str x17, [x12, #8]
    mov x17, #2
    str x17, [x12, #48]
    ldr x16, =0x42002000
    mov x17, #10
    b publish
gic_begin:
    ldr x16, =0x42002000
    ldr x12, =0x42002100
    ldr x13, =0x03400000
    ldr w17, [x13]             // Fresh CTLR before any native GIC write.
    str x17, [x16, #192]
    str x17, [x12, #8]
    mov w17, w21
    str x17, [x16, #200]
    str x17, [x12, #16]
    mov w17, w19
    str x17, [x16, #208]
    mov x17, #1
    str x17, [x16, #216]
    ldr w10, [x12, #8]
    and w11, w10, #0x70
    cmp w11, #0x30            // ARE_S/ARE_NS already set; DS clear.
    b.ne gic_admission_refused
    tbnz w10, #31, gic_admission_refused
    bic w11, w10, #7
    cmp w11, w19
    b.ne gic_admission_refused
    and w14, w21, #31
    add x14, x14, #1
    str x14, [x12, #24]
    mov x9, #1
    add x11, x13, #0x100
    add x15, x12, #64
gic_save_spi:
    cmp x9, x14
    b.hs gic_saved
    ldr w17, [x11, x9, lsl #2]
    str w17, [x15, x9, lsl #2]
    add x9, x9, #1
    b gic_save_spi
gic_saved:
    mov x17, #1
    str x17, [x12]            // Complete snapshot precedes the first write.
    ldr x8, =0x03400000
    str w19, [x8]             // Displaced native instruction; live x19..x22 retained.
    ldr x17, =0x410003d4
    br x17
gic_admission_refused:
    mov x17, #5
    str x17, [x12, #48]
    mov x17, #8
    b publish
block_checkpoint:
    mrs x17, mpidr_el1
    ubfx x17, x17, #8, #3
    cbnz x17, target_block
    ldr x16, =0x42002000
    mrs x17, sctlr_el1
    str x17, [x16, #152]
    mrs x17, tcr_el1
    str x17, [x16, #160]
    mov x17, #1
    str x17, [x16, #168]        // Originals are complete before any mutation.
    mrs x16, tcr_el1           // Displaced native instruction.
    ldr x17, =0x41008268
    br x17
stop_checkpoint:
    mrs x17, mpidr_el1
    ubfx x17, x17, #8, #3
    cbnz x17, target_failed
    ldr x16, =0x42002000
    str x0, [x16, #96]
    str x1, [x16, #104]
    mov x17, #3
    b publish
park_checkpoint:
    mrs x17, mpidr_el1
    ubfx x17, x17, #8, #3
    cbnz x17, target_failed
    ldr x16, =0x42002000
    mov x17, #4
    b publish
fault_checkpoint:
    mrs x17, mpidr_el1
    ubfx x17, x17, #8, #3
    cbnz x17, target_failed
    ldr x16, =0x42002000
    str x18, [x16, #112]
    mrs x17, esr_el3
    str x17, [x16, #120]
    mrs x17, elr_el3
    str x17, [x16, #128]
    mrs x17, far_el3
    str x17, [x16, #136]
    mov x17, #5
    b publish
write_checkpoint:
    mrs x17, mpidr_el1
    ubfx x17, x17, #8, #3
    cbnz x17, target_failed
    ldr x16, =0x42002000
    str x1, [x16, #96]          // Register number at the untouched callback entry.
    str x2, [x16, #104]         // Proposed value; the write never executes.
    mov x17, #6
    b publish
failed_checkpoint:
    mrs x17, mpidr_el1
    ubfx x17, x17, #8, #3
    cbnz x17, target_failed
    ldr x16, =0x42002000
    mov x17, #7
publish:
    str x17, [x16, #24]
    mrs x17, CurrentEL
    str x17, [x16, #48]
    cmp x17, #12
    b.ne park                  // A wrong EL cannot use the EL3 return.
    ldr x17, [x16, #168]
    cbz x17, snapshot_bank
    cmp x17, #1
    b.eq restore_bank
    cmp x17, #3
    b.eq verify_bank
    b park                     // In-progress, failed or unknown restoration: no retry.
restore_bank:
    mov x17, #2
    str x17, [x16, #168]
    mrs x17, sctlr_el1
    str x17, [x16, #224]
    mrs x17, tcr_el1
    str x17, [x16, #232]
    isb
    ldr x14, [x16, #152]
    msr sctlr_el1, x14
    isb
    ldr x15, [x16, #160]
    msr tcr_el1, x15
    isb
verify_bank:
    mov x17, #2
    str x17, [x16, #168]
    ldr x14, [x16, #152]
    ldr x15, [x16, #160]
    mrs x17, sctlr_el1
    str x17, [x16, #176]
    cmp x17, x14
    b.ne restore_failed
    mrs x17, tcr_el1
    str x17, [x16, #184]
    cmp x17, x15
    b.ne restore_failed
    mov x17, #3
    str x17, [x16, #168]
    b snapshot
restore_failed:
    mov x17, #4
    str x17, [x16, #168]
    b park
snapshot_bank:
    mrs x17, sctlr_el1
    str x17, [x16, #224]
    mrs x17, tcr_el1
    str x17, [x16, #232]
snapshot:
    ldr x12, =0x42002400
    ldr x17, [x12]
    cbz x17, activation_done
    cmp x17, #5
    b.eq activation_done
    cmp x17, #3
    b.ne park
    mov x17, #4
    str x17, [x12]            // Covers every restoration MMIO and verification read.
    ldr x13, =0x07053008
activation_restore_check:
    ldr w17, [x13]
    tst w17, #15
    b.ne park
    ldr x13, =0x07050204
    ldr w17, [x12, #40]
    str w17, [x13]
    dsb sy
    ldr w17, [x12, #48]
    str w17, [x13, #32]
    dsb sy
    ldr w10, [x13]
    str x10, [x12, #104]
    ldr w17, [x12, #40]
    cmp w10, w17
    b.ne park
    ldr w10, [x13, #32]
    str x10, [x12, #112]
    ldr w17, [x12, #48]
    cmp w10, w17
    b.ne park
    ldr x13, =0x07053008
    ldr w17, [x13]
    str x17, [x12, #56]
    tst w17, #15
    b.ne park
    mov x17, #5
    str x17, [x12]
activation_done:
    ldr x12, =0x42002280
    ldr x17, [x12]
    cbz x17, configuration_done
    cmp x17, #3
    b.eq configuration_done
    cmp x17, #1
    b.ne park
    mov x17, #2
    str x17, [x12]            // Guards every OFF check, snapshot, restore and readback.
    ldr x13, =0x08002000
    ldr x14, =0x07053008
    ldr x15, =0x07050204
    bl configuration_require_off
    ldr w17, [x13]
    str x17, [x12, #48]
    ldr w17, [x13, #4]
    str x17, [x12, #56]
    ldr w17, [x13, #8]
    str x17, [x12, #64]
    ldr w17, [x12, #32]
    str w17, [x13, #4]
    dsb sy
    ldr w17, [x12, #40]
    str w17, [x13, #8]
    dsb sy
    ldr w17, [x12, #24]
    str w17, [x13]
    dsb sy
    ldr w10, [x13]
    str x10, [x12, #72]
    ldr w17, [x12, #24]
    cmp w10, w17
    b.ne configuration_readback_failed
    ldr w10, [x13, #4]
    str x10, [x12, #80]
    ldr w17, [x12, #32]
    cmp w10, w17
    b.ne configuration_readback_failed
    ldr w10, [x13, #8]
    str x10, [x12, #88]
    ldr w17, [x12, #40]
    cmp w10, w17
    b.ne configuration_readback_failed
    bl configuration_require_off
    mov x17, #3
    str x17, [x12]
configuration_done:
    ldr x12, =0x42002100
    ldr x17, [x12]
    cbz x17, snapshot_final
    cmp x17, #3
    b.eq snapshot_final
    cmp x17, #1
    b.ne park
    mov x17, #2
    str x17, [x12]            // Covers every cleanup read, write and counter sample.
    ldr x13, =0x03400000
    ldr x15, [x12, #24]
    isb
    mrs x8, cntpct_el0
    ldr x14, =2400000         // Fresh 100 ms cleanup budget, not the startup lease.
    adds x14, x8, x14
    b.cs gic_counter_failed
    str x14, [x12, #40]
    bl gic_wait_rwp
    str x11, [x12, #32]       // Native terminal CTLR before cleanup.
    ldr w10, [x12, #8]
    bic w10, w10, #7
    str w10, [x13]            // Retains ARE and every original non-group bit.
    dsb sy
    bl gic_wait_rwp
    mov x9, #1
    add x10, x13, #0x180
    mov w17, #-1
gic_clear_spi:
    cmp x9, x15
    b.hs gic_clear_done
    str w17, [x10, x9, lsl #2]
    dsb sy
    add x9, x9, #1
    b gic_clear_spi
gic_clear_done:
    bl gic_wait_rwp
    mov x9, #1
    add x10, x13, #0x100
    add x11, x12, #64
gic_restore_spi:
    cmp x9, x15
    b.hs gic_verify_start
    ldr w17, [x11, x9, lsl #2]
    str w17, [x10, x9, lsl #2]
    dsb sy
    add x9, x9, #1
    b gic_restore_spi
gic_verify_start:
    mov x9, #1
    add x7, x12, #192
gic_verify_spi:
    cmp x9, x15
    b.hs gic_restore_ctlr
    ldr w6, [x10, x9, lsl #2]
    str w6, [x7, x9, lsl #2]
    ldr w17, [x11, x9, lsl #2]
    cmp w6, w17
    b.ne gic_spi_failed
    add x9, x9, #1
    b gic_verify_spi
gic_restore_ctlr:
    ldr w10, [x12, #8]
    str w10, [x13]
    dsb sy
    bl gic_wait_rwp
    cmp w11, w10
    b.ne gic_ctlr_failed
    mov x17, #3
    str x17, [x12]
snapshot_final:
    mrs x17, mpidr_el1
    str x17, [x16, #56]
    mrs x17, sctlr_el3
    str x17, [x16, #64]
    mrs x17, sctlr_el1
    str x17, [x16, #72]
    mrs x17, cntfrq_el0
    str x17, [x16, #80]
    isb
    mrs x17, cntpct_el0
    str x17, [x16, #88]
    mov x17, sp
    str x17, [x16, #144]
    mov x17, #1
    str x17, [x16, #8]
    mov x17, #256
    str x17, [x16, #16]
    ldr x17, =0x4137335354415254
    str x17, [x16]
    dsb sy
    ldr x0, =0x08001004
    mov w1, #0x20
    str w1, [x0]
    str wzr, [x0, #4]
    dsb sy
    mov x0, #2
    msr rmr_el3, x0
    isb
    b park
configuration_require_off:
    dsb sy
    ldr w17, [x14]
    dsb sy
    str x17, [x12, #112]
    tst w17, #15
    b.ne configuration_power_changed
    ldr w10, [x15]
    dsb sy
    ldr w11, [x15, #32]
    dsb sy
    orr x17, x10, x11, lsl #32
    str x17, [x12, #120]
    orr w17, w10, w11
    tbnz w17, #0, configuration_power_changed
    ret
configuration_power_changed:
    mov x17, #1
    b configuration_cleanup_failed
configuration_readback_failed:
    mov x17, #2
configuration_cleanup_failed:
    str x17, [x12, #96]
    mov x17, #4
    str x17, [x12]
    b park
gic_wait_rwp:
    isb
    mrs x17, cntpct_el0
    cmp x17, x8
    b.lo gic_counter_failed
    cmp x17, x14
    b.hs gic_deadline_failed
    mov x8, x17
    ldr w11, [x13]
    tbnz w11, #31, gic_wait_rwp
    ret
gic_deadline_failed:
    mov x17, #1
    b gic_cleanup_failed
gic_ctlr_failed:
    mov x17, #2
    b gic_cleanup_failed
gic_spi_failed:
    mov x17, #3
    b gic_cleanup_failed
gic_counter_failed:
    mov x17, #4
gic_cleanup_failed:
    str x17, [x12, #48]
    mov x17, #4
    str x17, [x12]
park:
    wfi
    b park
.ltorg
.section .note.GNU-stack,"",%progbits
