/* Execute the frozen FreeRTOS IRQ save/restore path without selecting tasks.
 * Its application callback is replaced below. CPU0 alone owns the first and
 * last EL1 stack pages and the page containing the port's nesting arrays.
 * Finish verifies register patterns, restores borrowed FP state and cleans
 * every written page through its Nonsecure mapping before the SMC return. */
.section .text.primary_context,"ax"
.balign 4
.global primary_context_begin, primary_context_dispatch, primary_context_finish
.global primary_context_clean, primary_context_clean_done
.type primary_context_begin,%function
primary_context_begin:
    mrs x0, ctr_el0
    ubfx x0, x0, #16, #4
    cmp x0, #4
    b.ne primary_irq_park
    ldr x0, =probe_context_nesting
    ldr x1, [x0]
    cbnz x1, primary_irq_park
    ldr x0, =probe_context_yield
    ldr x1, [x0]
    cbnz x1, primary_irq_park
    ldr x0, =probe_production_el1_stacks
    ldp x1, x2, [x0]
    orr x1, x1, x2
    cbnz x1, primary_irq_park
    mrs x1, fpcr
    mrs x2, fpsr
    stp x1, x2, [x0, #32]
    mov x1, sp
    str x1, [x0, #48]
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    str q\n, [x0, #(256+16*\n)]
.endr
    msr fpcr, xzr
    msr fpsr, xzr
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    movi v\n\().16b, #(\n+1)
.endr
.irp n,4,5,6,7,8,9,10,11,18
    mov x\n, #(0x100+\n)
.endr
    ret
.size primary_context_begin,.-primary_context_begin

.type primary_context_dispatch,%function
primary_context_dispatch:
    cmp x0, #1020
    b.hs .Ldispatch_return
    ldr x1, =probe_context_nesting
    ldr x2, [x1]
    cmp x2, #1
    b.ne primary_irq_park
    ldr x1, =probe_context_yield
    ldr x2, [x1]
    cbnz x2, primary_irq_park
    ldr x1, =probe_production_el1_stacks
    cbz x0, .Lself
    cmp x0, #30
    b.ne primary_irq_park
    ldr x2, [x1]
    cbnz x2, primary_irq_park
    msr cntp_ctl_el0, xzr
    isb
    mov x2, #1
    str x2, [x1]
    b .Ldispatch_return
.Lself:
    ldr x2, [x1, #8]
    cbnz x2, primary_irq_park
    mov x2, #1
    str x2, [x1, #8]
.Ldispatch_return:
    ret
.size primary_context_dispatch,.-primary_context_dispatch

.type primary_context_finish,%function
primary_context_finish:
.irp n,4,5,6,7,8,9,10,11,18
    cmp x\n, #(0x100+\n)
    b.ne primary_irq_park
.endr
    ldr x0, =probe_production_el1_stacks
    ldr x1, [x0, #48]
    mov x2, sp
    cmp x1, x2
    b.ne primary_irq_park
    mrs x1, fpcr
    mrs x2, fpsr
    orr x1, x1, x2
    cbnz x1, primary_irq_park
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    mov x1, #0x0101010101010101
    mov x2, #(\n+1)
    mul x1, x1, x2
    umov x2, v\n\().d[0]
    cmp x1, x2
    b.ne primary_irq_park
    umov x2, v\n\().d[1]
    cmp x1, x2
    b.ne primary_irq_park
.endr
    ldr x0, =probe_context_nesting
    ldr x1, [x0]
    cbnz x1, primary_irq_park
    ldr x0, =probe_context_yield
    ldr x1, [x0]
    cbnz x1, primary_irq_park
    ldr x0, =probe_production_el1_stacks
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ldr q\n, [x0, #(256+16*\n)]
.endr
    ldp x1, x2, [x0, #32]
    msr fpcr, x1
    msr fpsr, x2
    // Every IRQ is inactive and disabled. No Normal-memory stores follow.
    dsb sy
    mov x23, #0
.macro clean_page
    add x2, x0, #4096
.Lclean\@:
    dc civac, x0
    add x23, x23, #1
    add x0, x0, #64
    cmp x0, x2
    b.lo .Lclean\@
.endm
primary_context_clean:
    isb
    mrs x1, cntpct_el0
    cmp x1, x22
    b.hs primary_irq_park
    clean_page
    ldr x0, =probe_production_el1_stacks
    add x0, x0, #0x7000
    clean_page
    adrp x0, probe_context_nesting
    clean_page
    dsb sy
primary_context_clean_done:
    mov x24, #1
    ret
.size primary_context_finish,.-primary_context_finish
.section .note.GNU-stack,"",%progbits
