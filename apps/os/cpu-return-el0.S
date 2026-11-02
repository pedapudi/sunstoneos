/* Admission of a synchronous exception from an EL0 task to the failure return
 * (docs/designs/failure-return.md).
 *
 * An EL1 vector for a synchronous exception that is not a supervisor call
 * calls os_cpu_return_el0_try with the exception state still in the system
 * registers. The routine accepts the exception only when it came from an AArch64
 * EL0 task that was open to the scheduler (SPSR_EL1.I clear and ICC_PMR_EL1
 * at or above 0xf0, above any API mask; the unmasked value 0xff reads back as
 * 0xf8 or 0xf0 when the controller implements fewer priority bits) and its class is
 * an undefined instruction (0x00), an instruction abort (0x20), a PC alignment
 * fault (0x22), a data abort (0x24) or an SP alignment fault (0x26).
 *
 * Accepting abandons the faulting context without using any stack: the task
 * returns to EL0 in os_cpu_return_el0_fault (apps/os/cpu-return.c) with SP_EL0
 * set to one of eight reserved stacks and ESR_EL1, FAR_EL1 and ELR_EL1 in x0 to
 * x2. That function claims the failure return as ss_fail does and suspends the
 * task. SPSR_EL1 is zero (EL0t, interrupts open). On acceptance the routine
 * does not return. Otherwise, including when all stacks are claimed, it
 * returns to its caller and changes only x16, x17 and x4 to x6.
 *
 * The branch history clearing loop is the one in
 * platform/a733-smp-cached/continue.S. */
.section .text.os_cpu_return_el0_try, "ax", %progbits
.balign 4
.global os_cpu_return_el0_try
.type os_cpu_return_el0_try, %function
os_cpu_return_el0_try:
    mrs x16, spsr_el1
    and x17, x16, #0x1f
    cbnz x17, .Lrefuse
    tbnz x16, #7, .Lrefuse
    mrs x17, icc_pmr_el1
    cmp x17, #0xf0
    b.lo .Lrefuse
    mrs x17, esr_el1
    lsr x16, x17, #26
    cbz x16, .Laccept
    cmp x16, #0x20
    b.eq .Laccept
    cmp x16, #0x22
    b.eq .Laccept
    cmp x16, #0x24
    b.eq .Laccept
    cmp x16, #0x26
    b.ne .Lrefuse
.Laccept:
    mov x16, #24
.Lhistory:
    b .Lhistory_next
.Lhistory_next:
    subs x16, x16, #1
    b.ne .Lhistory
    dsb sy
    isb
    adrp x16, os_cpu_return_el0_slots
    add x16, x16, :lo12:os_cpu_return_el0_slots
.Lslot:
    ldaxr w4, [x16]
    cmp w4, #8
    b.hs .Lrefuse
    add w5, w4, #1
    stlxr w6, w5, [x16]
    cbnz w6, .Lslot
    adrp x6, os_cpu_return_el0_stacks
    add x6, x6, :lo12:os_cpu_return_el0_stacks
    add x6, x6, x5, lsl #11
    msr sp_el0, x6
    mov x0, x17
    mrs x1, far_el1
    mrs x2, elr_el1
    adrp x3, os_cpu_return_el0_fault
    add x3, x3, :lo12:os_cpu_return_el0_fault
    msr elr_el1, x3
    msr spsr_el1, xzr
    eret
.Lrefuse:
    ret
.size os_cpu_return_el0_try, .-os_cpu_return_el0_try

/* Eight stacks of 2048 bytes: a supervisor-call context save (general
 * registers and floating-point state) and vTaskSuspend fit in one. The slot
 * count is claimed with an exclusive pair; slots are not released, because the
 * run ends with the first failure. */
.section .bss.os_cpu_return_el0_slots, "aw", %nobits
.balign 64
.global os_cpu_return_el0_slots
.type os_cpu_return_el0_slots, %object
os_cpu_return_el0_slots:
    .space 4
.size os_cpu_return_el0_slots, 4
.section .bss.os_cpu_return_el0_stacks, "aw", %nobits
.balign 64
.global os_cpu_return_el0_stacks
.type os_cpu_return_el0_stacks, %object
os_cpu_return_el0_stacks:
    .space 8 * 2048
.size os_cpu_return_el0_stacks, 8 * 2048
.section .note.GNU-stack, "", %progbits
