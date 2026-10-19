/* Run actual task construction and first context restoration. The first idle
 * instruction is redirected to SVC; no task body or timer/SGI workload runs.
 * All eight writers stop before cleaning the complete owned writable interval.
 * This profile is offline-only until its instruction and return checks pass. */
.section .text.identity_first_task,"ax"
.balign 4
.global identity_pre_scheduler_el1, identity_pre_scheduler_clean_done
.global identity_first_task_svc, identity_first_task_el0
.type identity_pre_scheduler_el1,%function
identity_pre_scheduler_el1:
    mrs x0, ctr_el0
    ubfx x0, x0, #16, #4
    cmp x0, #4
    b.ne identity_park
    cmp x20, #8
    b.hs identity_park
    // The first page of CPU0's 32 KiB EL1 stack is reserved for this profile.
    // Completion records occupy its first 512 bytes; saved frames start at 2 KiB.
    ldr x0, =probe_production_el1_stacks
    add x0, x0, #0x800
    add x0, x0, x20, lsl #8
    mov x1, sp
    stp x30, x1, [x0]
    stp x20, x21, [x0, #16]
    stp x22, x23, [x0, #32]
    mrs x1, vbar_el1
    stp x24, x1, [x0, #48]
    // Match the production CPU interface. Entry-only profiles keep delivery off.
    msr icc_ctlr_el1, xzr
    msr icc_bpr1_el1, xzr
    mov x1, #255
    msr icc_pmr_el1, x1
    isb
    mrs x1, icc_igrpen1_el1
    cbnz x1, identity_park
#ifdef SUNSTONE_TASK_WORKLOAD
    mov x1, #1
    msr icc_igrpen1_el1, x1
    isb
#endif
    mov x0, x20
    mov x1, x21
    mov x2, x22
    mov x3, x23
    mov x4, x24
    bl a733_cached_el1
    b identity_park
.size identity_pre_scheduler_el1,.-identity_pre_scheduler_el1

#ifdef SUNSTONE_TASK_WORKLOAD
.global identity_workload_complete
identity_workload_complete:
    // The frozen worker reaches this branch only after all 32 FP rounds.
    // Keep timer delivery running until every worker reaches this boundary.
    cmp x19, #8
    b.hs .Lworkload_park
    ldr x0, =probe_production_el1_stacks
    add x1, x0, #0x800
    add x1, x1, x19, lsl #8
    ldr x21, [x1, #24]
    ldr x22, [x1, #32]
.Lworkload_background:
    // FP completion alone does not imply that a lower-priority task ran.
    // Block this worker until its own background task has made progress.
    ldr x0, =probe_worker_results
    add x0, x0, x19, lsl #7
    ldr x1, [x0, #8]
    cbnz x1, .Lworkload_arrive
    isb
    mrs x1, cntpct_el0
    cmp x1, x22
    b.hs .Lworkload_park
    mov x0, #2
    bl vTaskDelay
    b .Lworkload_background
.Lworkload_arrive:
    ldr x0, =probe_production_el1_stacks
    add x0, x0, #0x400
    add x1, x0, x19, lsl #3
    stlr x21, [x1]
    mov x2, #0
.Lworkload_wait:
    add x1, x0, x2, lsl #3
    ldar x3, [x1]
    cmp x3, x21
    b.eq .Lworkload_next
    isb
    mrs x3, cntpct_el0
    cmp x3, x22
    b.hs .Lworkload_park
    b .Lworkload_wait
.Lworkload_next:
    add x2, x2, #1
    cmp x2, #8
    b.lo .Lworkload_wait
    mov x0, x19
    b identity_first_task_el0
.Lworkload_park:
    wfe
    b .Lworkload_park
#endif

identity_first_task_el0:
    svc #0x734
    b identity_first_task_el0

identity_first_task_svc:
    // Replay the displaced FreeRTOS instruction before using X0/X1. Ordinary
    // kernel SVCs retain their original context and continue at instruction two.
    stp x0, x1, [sp, #-16]!
    mrs x0, esr_el1
    ldr x1, =0x56000734
    cmp x0, x1
    b.eq .Lfirst_task
    b probe_swi_next
.Lfirst_task:
    msr daifset, #15
    mrs x20, mpidr_el1
    ubfx x20, x20, #8, #3
    mrs x2, spsr_el1
    and x3, x2, #15
    cbnz x3, identity_park
    mrs x3, elr_el1
    adr x4, identity_first_task_el0
    add x4, x4, #4
    cmp x3, x4
    b.ne identity_park
    mrs x4, sp_el0
    ldr x5, [sp]
#ifdef SUNSTONE_TASK_SWITCH
    cmp x5, x20
    b.ne identity_park
#else
    cbnz x5, identity_park
#endif
    ldr x6, =pxCurrentTCBs
    ldr x6, [x6, x20, lsl #3]
#ifdef SUNSTONE_TASK_SWITCH
    ldr x7, =probe_worker_tcbs
    mov x8, #168
    madd x7, x20, x8, x7
#else
    ldr x7, =xIdleTaskHandles
    ldr x7, [x7, x20, lsl #3]
#endif
    cmp x6, x7
    b.ne identity_park
    cbz x6, identity_park
#ifdef SUNSTONE_TASK_WORKLOAD
    // No more task execution on this CPU. Stop timer generation and delivery
    // before publishing the final writer-completion record.
    msr cntp_ctl_el0, xzr
    msr cntv_ctl_el0, xzr
    msr icc_igrpen1_el1, xzr
    dsb sy
    isb
#endif
    ldr x0, =probe_production_el1_stacks
    add x8, x0, #0x800
    add x8, x8, x20, lsl #8
    ldp x19, x9, [x8]
    ldp x20, x21, [x8, #16]
    ldp x22, x23, [x8, #32]
    ldp x24, x10, [x8, #48]
    mov sp, x9
    msr vbar_el1, x10
    isb
    add x0, x0, x20, lsl #6
    mov x1, #1
    stp x1, x3, [x0, #8]
    stp x4, x2, [x0, #24]
    stp x6, x5, [x0, #40]
    mrs x1, sctlr_el1
    str x1, [x0, #56]
    stlr x21, [x0]
    mov x29, #0
    mov x3, #0
.Lwait_core:
    ldr x0, =probe_production_el1_stacks
    add x0, x0, x3, lsl #6
.Lwait_done:
    ldar x1, [x0]
    cmp x1, x21
    b.eq .Ldone
    isb
    mrs x1, cntpct_el0
    cmp x1, x22
    b.hs identity_park
    b .Lwait_done
.Ldone:
    ldr x1, [x0, #8]
    lsl x1, x1, x3
    orr x29, x29, x1
    add x3, x3, #1
    cmp x3, #8
    b.lo .Lwait_core
#ifdef SUNSTONE_TASK_WORKLOAD
    // Every task/handler writer is now stopped. Drain only our SGI0/PPI30;
    // unrelated pending or active state prevents return.
    dsb sy
    add x0, x24, #0x10000
    ldr w1, =0x40000001
    str w1, [x0, #0x180]
    dsb sy
.Lworkload_rwp:
    ldr w2, [x24]
    tbz w2, #3, .Lworkload_clear
    isb
    mrs x2, cntpct_el0
    cmp x2, x22
    b.hs identity_park
    b .Lworkload_rwp
.Lworkload_clear:
    ldr w2, [x0, #0x300]
    cbnz w2, identity_park
    ldr w2, [x0, #0x200]
    bic w2, w2, w1
    cbnz w2, identity_park
    str w1, [x0, #0x280]
    dsb sy
    ldr w2, [x0, #0x200]
    cbnz w2, identity_park
    // Inspect stable results only after all writers stopped. Preserve counts
    // in the existing copied production-state page for host verification.
    ldr x0, =probe_worker_results
    mov x1, #0
.Lworkload_result:
    add x2, x0, x1, lsl #7
    ldr x3, [x2]
    cmp x3, #32
    b.ne identity_park
    ldp x3, x4, [x2, #8]
    cbz x3, identity_park
    cbz x4, identity_park
    ldr x3, [x2, #24]
    cbnz x3, identity_park
    add x1, x1, #1
    cmp x1, #8
    b.lo .Lworkload_result
    ldr x3, [x0, #32]
    cmp x3, #32
    b.lo identity_park
#endif
    ldr x0, =probe_scheduler_count
    ldr w27, [x0]
    mov x26, #1
    // No task, exception handler or callback can write Normal memory after
    // publication above. Include every .data/.bss page, not just observed writes.
    dsb sy
    ldr x0, =probe_writable_start
    ldr x2, =probe_writable_end
    mov x28, #0
.Lclean:
    dc civac, x0
    add x28, x28, #1
    add x0, x0, #64
    cmp x0, x2
    b.lo .Lclean
    dsb sy
identity_pre_scheduler_clean_done:
    mov x25, #1
    ret x19
.section .note.GNU-stack,"",%progbits
