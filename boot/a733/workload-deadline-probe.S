/* CPU0 return deadline for the returning eight-core workload profiles.
 *
 * The workload overlay parks a core whenever a check or a bounded wait fails,
 * and CPU0 parks too, so a failed board run gives no report. This unit makes
 * every CPU0 stop return to the ROM with a record of where CPU0 was:
 *
 * - identity_deadline_arm replaces the MSR ICC_IGRPEN0_EL1 in the production
 *   cached EL1 entry. On CPU0 only it makes PPI 29 (the EL3 physical timer)
 *   Group 0 at the highest priority, arms CNTPS for the startup deadline plus
 *   DEADLINE_TICKS and enables Group 0. With SCR_EL3.FIQ set, the deadline
 *   interrupt is taken to EL3 whatever EL1 masks, locks or waits CPU0 is in.
 * - identity_deadline_fiq is the EL3 vector for an FIQ from a lower exception
 *   level. Other cores keep the production fault path.
 * - identity_deadline_smc runs first for every SMC from EL1. CPU0's completion
 *   SMC disarms the deadline and continues the unchanged return; any other
 *   CPU0 SMC (a refusal, a deadline exit of the IRQ handler, a panic) is
 *   captured like the deadline interrupt.
 * - identity_deadline_capture stops the deadline, turns the EL3 MMU off,
 *   writes the record and returns to CPU0 EL1 through identity_deadline_el1,
 *   which cleans the owned writable interval to the point of coherency
 *   through the same Nonsecure mapping the writers used. Its SMC #0x736
 *   reaches identity_deadline_finish, which returns to the ROM through the
 *   base overlay's RVBAR/RMR sequence.
 * - identity_deadline_gate replaces the EL3 park after a failed survey.
 *
 * The secondaries are not stopped: a deadline return leaves them running or
 * parked, with their GIC and power state unrestored, so the host must read
 * the record and memory only, restore nothing and request a power removal.
 * No stack is used. EL3 record stores are Device stores. */
#include "secondary-identity-probe.h"
#include "secondary-local-probe.h"

#define SMC_COMPLETE 0x5e000733
#define SMC_CLEANED 0x5e000736

.section .text.workload_deadline,"ax"
.balign 4
.global identity_deadline_arm, identity_deadline_fiq, identity_deadline_smc
.global identity_deadline_gate, identity_deadline_el1, identity_deadline_finish

identity_deadline_arm:
    // EL3 on every core, inside a733_cached_enter_el1: x20 core, x22 startup
    // deadline in counter ticks, x24 this core's redistributor. x2/x3 are free.
    msr icc_igrpen0_el1, xzr
    cbnz x20, 1f
    add x2, x24, #0x10000
    ldr w3, [x2, #0x80]
    bic w3, w3, #(1 << 29)
    str w3, [x2, #0x80]
    // GICR_IGRPMODR0 is zero after the continuation, so this selects Group 0.
    // INTID 29 is byte 1 of IPRIORITYR7. native_el3_cache_probe saved this
    // word before EL1 entry and restores it after a completed return.
    ldr w3, [x2, #0x41c]
    bic w3, w3, #0xff00
    str w3, [x2, #0x41c]
    mov w3, #(1 << 29)
    str w3, [x2, #0x100]
    dsb sy
    mov x3, #(DEADLINE_TICKS & 0xffff)
    movk x3, #(DEADLINE_TICKS >> 16), lsl #16
    add x3, x22, x3
    msr cntps_cval_el1, x3
    mov x3, #1
    msr cntps_ctl_el1, x3
    isb
    msr icc_igrpen0_el1, x3
    isb
1:  ret

identity_deadline_fiq:
    mrs x16, mpidr_el1
    ubfx x16, x16, #8, #3
    cbz x16, 1f
    // Displaced vector instruction; only CPU0 has a Group 0 source.
    mov x18, #0xa
    b a733_cached_el3_fault
1:  mov x16, #1
    b identity_deadline_capture

identity_deadline_smc:
    mrs x16, mpidr_el1
    ubfx x16, x16, #8, #3
    cbnz x16, el1_return_checkpoint_continue
    mrs x16, esr_el3
    mov x17, #(SMC_COMPLETE & 0xffff)
    movk x17, #(SMC_COMPLETE >> 16), lsl #16
    sub x16, x16, x17
    cmp x16, #(SMC_CLEANED - SMC_COMPLETE)
    b.eq identity_deadline_finish
    cbnz x16, 1f
    // Completed workload: the deadline must not fire during the unchanged
    // restoration, and restore_local requires Group 0 disabled.
    msr cntps_ctl_el1, xzr
    msr icc_igrpen0_el1, xzr
    isb
    b el1_return_checkpoint_continue
1:  mov x16, #2
    b identity_deadline_capture

identity_deadline_gate:
    // identity_primary_run returned a failure at EL3.
    mov x16, #3
    b identity_deadline_capture

.global identity_deadline_refused
identity_deadline_refused:
    // el1_return_checkpoint refused CPU0's completion SMC.
    mov x16, #4
    // Fall through.

identity_deadline_capture:
    // EL3, CPU0, x16 reason, x0/x1 as at entry. Never returns to the
    // interrupted context, so every other register is free.
    msr daifset, #15
    // Acknowledge the deadline interrupt while its level source still holds it.
    mov x9, #1023
    cmp x16, #1
    b.ne 1f
    mrs x9, icc_iar0_el1
1:  msr cntps_ctl_el1, xzr
    mrs x17, sctlr_el3
    bic x17, x17, #1
    bic x17, x17, #4
    bic x17, x17, #0x1000
    dsb sy
    msr sctlr_el3, x17
    isb
    ic iallu
    tlbi alle3
    dsb sy
    isb
    ldr x15, =DEADLINE_RECORD
    ldr x14, [x15]
    ldr x13, =DEADLINE_RECORD_MAGIC
    cmp x14, x13
    // A second entry (a fault in the cleaning helper) returns at once.
    b.eq identity_deadline_return
    mov x2, #1
    stp x13, x2, [x15]
    mrs x3, cntpct_el0
    stp x16, x2, [x15, #16]
    mrs x4, cntps_cval_el1
    stp x3, x4, [x15, #32]
    mrs x2, elr_el3
    mrs x3, spsr_el3
    stp x2, x3, [x15, #48]
    mrs x2, esr_el3
    stp x2, x0, [x15, #64]
    mrs x2, elr_el1
    stp x1, x2, [x15, #80]
    mrs x2, spsr_el1
    mrs x3, esr_el1
    stp x2, x3, [x15, #96]
    mrs x3, sp_el0
    str x3, [x15, #120]
    mrs x2, sp_el1
    stp x2, x29, [x15, #128]
    stp x30, x9, [x15, #144]
    cmp x9, #1020
    b.hs 2f
    msr icc_eoir0_el1, x9
2:
    msr icc_igrpen0_el1, xzr
    mrs x2, cntp_ctl_el0
    mrs x3, cntp_cval_el0
    stp x2, x3, [x15, #160]
    mov x3, #(IDENTITY_REPORT & 0xffff)
    movk x3, #(IDENTITY_REPORT >> 16), lsl #16
    ldp x2, x3, [x3, #40]
    stp x3, x2, [x15, #216]
    // Pending, active and enabled SGI/PPI words of all eight redistributors,
    // then the power status words that identity_primary_run reads.
    mov x4, #0x3470000
    mov x7, #0x2008
    movk x7, #0x0705, lsl #16
    add x5, x15, #232
3:  ldr w2, [x4, #0x200]
    ldr w3, [x4, #0x300]
    stp x2, x3, [x5]
    ldr w2, [x4, #0x100]
    ldr w3, [x7]
    stp x2, x3, [x5, #16]
    add x4, x4, #0x20000
    add x7, x7, #0x1000
    add x5, x5, #32
    sub x6, x5, x15
    cmp x6, #488
    b.lo 3b
    dsb sy
    // Reasons 3 and 4 arrive after CPU0's EL1 interval ended: no clean.
    cmp x16, #3
    b.hs identity_deadline_return
    adr x2, identity_deadline_el1
    msr elr_el3, x2
    mov x2, #0x3c5
    msr spsr_el3, x2
    isb
    eret

identity_deadline_el1:
    // Nonsecure EL1h with DAIF masked, on CPU0's own translation regime.
    // Stop CPU0's tick timer: CNTP_CTL_EL0.ENABLE is UNKNOWN after the warm
    // reset, so the reset alone does not stop it.
    msr cntp_ctl_el0, xzr
    msr icc_igrpen1_el1, xzr
    dsb sy
    ldr x0, =probe_writable_start
    ldr x2, =probe_writable_end
    mov x1, #0
5:  dc civac, x0
    add x0, x0, #64
    add x1, x1, #1
    cmp x0, x2
    b.lo 5b
    dsb sy
    mov x0, #0x736
    smc #0x736
6:  wfi
    b 6b

identity_deadline_finish:
    // EL3 MMU is off since the capture; x1 holds the cleaned line count.
    ldr x15, =DEADLINE_RECORD
    mrs x2, cntpct_el0
    stp x1, x2, [x15, #200]
    mov x2, #3
    str x2, [x15, #24]
    // Fall through.

identity_deadline_return:
    // x15 holds DEADLINE_RECORD on both paths here. Request the warm reset with the
    // SCR_EL3 that native_el3_cache_probe saved (CPU0 EL3 record + 64), as
    // the exercised return does; zero means it never left the ROM value.
    mov x3, #0xa040
    movk x3, #0x4201, lsl #16
    ldr x2, [x3]
    cbz x2, 7f
    msr scr_el3, x2
    isb
7:  mov x2, #1
    str x2, [x15, #496]
    dsb sy
    // Displaced first instruction of the physically exercised return sequence.
    ldr x0, =0x08001004
    ldr x17, =identity_original_return_next
    br x17
.ltorg
.section .note.GNU-stack,"",%progbits
