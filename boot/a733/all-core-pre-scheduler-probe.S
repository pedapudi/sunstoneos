/* Call the frozen production pre-scheduler on all eight cores. Each participant
 * owns its final EL1 stack page. The first CPU0 stack page holds completion
 * records; the production function owns one separate shared data page. All
 * writers finish before any participant cleans these three pages and returns.
 * Interrupt delivery remains disabled; no task is selected or dispatched. */
.section .text.identity_pre_scheduler,"ax"
.balign 4
.global identity_pre_scheduler_el1, identity_pre_scheduler_clean_done
.type identity_pre_scheduler_el1,%function
identity_pre_scheduler_el1:
    mov x19, x30
    mrs x0, ctr_el0
    ubfx x0, x0, #16, #4
    cmp x0, #4
    b.ne identity_park
    cmp x20, #8
    b.hs identity_park
    ldr x24, =probe_production_el1_stacks
    add x0, x24, x20, lsl #6
    ldp x1, x2, [x0]
    orr x1, x1, x2
    cbnz x1, identity_park
    mov x0, x20
    mov x1, x21
    mov x2, #24
    udiv x2, x22, x2
    bl probe_production_pre_scheduler
    // A completed failure also counts as a stopped writer. The report preserves
    // its zero result; host validation must never turn that into success.
    cmp w0, #1
    b.hi identity_park
    mov w26, w0
    add x0, x24, x20, lsl #6
    str x26, [x0, #8]
    stlr x21, [x0]
    mov x29, #0
    mov x3, #0
.Lwait_core:
    add x0, x24, x3, lsl #6
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
    ldr x0, =probe_scheduler_count
    ldr w27, [x0]
    // No Normal-memory stores follow, including on the active EL1 stack.
    dsb sy
    mov x28, #0
.macro clean_page
    add x2, x0, #4096
.Lclean\@:
    dc civac, x0
    add x28, x28, #1
    add x0, x0, #64
    cmp x0, x2
    b.lo .Lclean\@
.endm
    mov x0, x24
    clean_page
    add x0, x24, x20, lsl #15
    add x0, x0, #0x7000
    clean_page
    ldr x0, =probe_scheduler_state
    clean_page
    dsb sy
identity_pre_scheduler_clean_done:
    mov x25, #1
    ret x19
.size identity_pre_scheduler_el1,.-identity_pre_scheduler_el1
.section .note.GNU-stack,"",%progbits
