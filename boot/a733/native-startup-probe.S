// Fixed native-image checkpoints. The original startup owns stack and BSS.
.arch armv8-a
.section .text,"ax"
.global boot_checkpoint, cached_checkpoint, stop_checkpoint, park_checkpoint, fault_checkpoint
.global write_checkpoint, failed_checkpoint
.global block_checkpoint, gic_begin, prearm_checkpoint
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
    ldr x16, =0x42002000
    str x0, [x16, #96]
    str x1, [x16, #104]
    mov x17, #3
    b publish
park_checkpoint:
    ldr x16, =0x42002000
    mov x17, #4
    b publish
fault_checkpoint:
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
    ldr x16, =0x42002000
    str x1, [x16, #96]          // Register number at the untouched callback entry.
    str x2, [x16, #104]         // Proposed value; the write never executes.
    mov x17, #6
    b publish
failed_checkpoint:
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
