/* Entry and exception vectors of the virtual OS board: platform/qemu-smp/start.S
 * with the two breadcrumb supervisor calls of the A733 returning images
 * (include/sunstone_crumb.h): 0x5c0d records a stage and 0x5c0e records the
 * first board failure. Here they call the board's records in board.c. */
.section .text.boot, "ax"
.global _start
_start:
    msr daifset, #0xf
    mrs x0, CurrentEL
    cmp x0, #4
    b.ne .Lpark
    mrs x0, mpidr_el1
    and x1, x0, #0xff
    // This emulator target numbers its cores in one affinity group.
    lsr x0, x0, #8
    and x0, x0, #0xffff
    cbnz x0, .Lpark
    cmp x1, #8
    b.hs .Lpark
    msr tpidr_el1, x1
    msr spsel, #1
    // The stages before the kernel leave SP_EL0 at a stack of their own. On
    // the board the first launch found 0x41039f60, an address in the
    // read-only code block. Every core takes that kind of value here: a task
    // frame saved before the first task restore faults instead of landing in
    // writable RAM that happens to be valid.
    adr x2, _start + 0x100
    msr sp_el0, x2
    ldr x0, =__exception_stacks
    add x1, x1, #1
    add x0, x0, x1, lsl #15
    mov sp, x0
    ldr x0, =_freertos_vector_table
    msr vbar_el1, x0
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    mov x0, #3
    msr cntkctl_el1, x0
    isb
    mrs x0, tpidr_el1
    cbnz x0, .Lsecondary
    ldr x0, =__bss_start
    ldr x1, =__bss_end
.Lclear:
    cmp x0, x1
    b.hs .Lprimary
    str xzr, [x0], #8
    b .Lclear
.Lprimary:
    bl kernel_main
.Lpark:
    wfe
    b .Lpark
.Lsecondary:
    bl platform_secondary
    b .Lpark

// A task is running when the exception is taken from a t-mode (EL1t or EL0t,
// SPSR.M[0] clear), and the port's context save then pushes the frame onto
// SP_EL0. That register must lie in a writable stack, which is RAM below the
// exception stacks. A value left from the boot stages, such as an address in
// the code block, would make the save fault on the board. A violation ends
// the run with its own record. This is the board's check, not the kernel's.
.macro check_task_sp
    stp x0, x1, [sp, #-16]!
    mrs x0, spsr_el1
    tbnz x0, #0, 1f
    mrs x0, sp_el0
    tst x0, #15
    b.ne .Lsp_violation
    // A task stack lies in image RAM or in the upper memory that the OS maps for its application (0x48000000 to the end
    // of the installed 12 GiB, 0x340000000, but not the hole 0x79000000..0x80000000), where the main stack comes from the
    // ledger with a guard block below it. The ledger takes the smallest gap, which can lie beside a region above 2 GiB.
    ldr x1, =__ram_start
    cmp x0, x1
    b.lo .Lsp_violation
    ldr x1, =__exception_stacks
    cmp x0, x1
    b.lo 1f
    movz x1, #0x4800, lsl #16
    cmp x0, x1
    b.lo .Lsp_violation
    movz x1, #0x7900, lsl #16
    cmp x0, x1
    b.lo 1f
    movz x1, #0x8000, lsl #16
    cmp x0, x1
    b.lo .Lsp_violation
    movz x1, #0x4000, lsl #16
    movk x1, #0x3, lsl #32
    cmp x0, x1
    b.hs .Lsp_violation
1:  ldp x0, x1, [sp], #16
.endm

.section .text.vectors, "ax"
.balign 2048
.global _freertos_vector_table
_freertos_vector_table:
.macro slot handler
    b \handler
    .space 124
.endm
.rept 3
    slot synchronous_exception
    slot irq_entry
    slot platform_exception
    slot platform_exception
.endr
.rept 4
    slot platform_exception
.endr

.section .text, "ax"
irq_entry:
    check_task_sp
    b FreeRTOS_IRQ_Handler
.Lsp_violation:
    bl qemu_os_sp_violation
synchronous_exception:
    check_task_sp
    stp x0, x1, [sp, #-16]!
    mrs x0, esr_el1
    lsr x0, x0, #26
    cmp x0, #0x15
    b.ne .Lexception
    mrs x0, esr_el1
    and x0, x0, #0xffff
    cmp x0, #119
    b.eq .Lexit
    mov x1, #0x5c0d
    cmp x0, x1
    b.eq .Lcrumb
    mov x1, #0x5c0e
    cmp x0, x1
    b.eq .Lfail
    ldp x0, x1, [sp], #16
    b FreeRTOS_SWI_Handler
.Lcrumb:
    // sunstone_crumb(stage, value): x0, x1 saved on the stack; the caller's
    // registers other than the argument registers survive.
    sub sp, sp, #160
    stp x2, x3, [sp, #0]
    stp x4, x5, [sp, #16]
    stp x6, x7, [sp, #32]
    stp x8, x9, [sp, #48]
    stp x10, x11, [sp, #64]
    stp x12, x13, [sp, #80]
    stp x14, x15, [sp, #96]
    stp x16, x17, [sp, #112]
    stp x18, x30, [sp, #128]
    ldp x0, x1, [sp, #160]
    bl qemu_os_crumb
    b .Lcrumb_return
.Lfail:
    // sunstone_crumb_fail(site, value, caller): x2 is the caller.
    sub sp, sp, #160
    stp x2, x3, [sp, #0]
    stp x4, x5, [sp, #16]
    stp x6, x7, [sp, #32]
    stp x8, x9, [sp, #48]
    stp x10, x11, [sp, #64]
    stp x12, x13, [sp, #80]
    stp x14, x15, [sp, #96]
    stp x16, x17, [sp, #112]
    stp x18, x30, [sp, #128]
    ldp x0, x1, [sp, #160]
    bl qemu_os_crumb_fail
.Lcrumb_return:
    ldp x2, x3, [sp, #0]
    ldp x4, x5, [sp, #16]
    ldp x6, x7, [sp, #32]
    ldp x8, x9, [sp, #48]
    ldp x10, x11, [sp, #64]
    ldp x12, x13, [sp, #80]
    ldp x14, x15, [sp, #96]
    ldp x16, x17, [sp, #112]
    ldp x18, x30, [sp, #128]
    add sp, sp, #160
    ldp x0, x1, [sp], #16
    eret
.Lexit:
    ldp x0, x1, [sp], #16
    b platform_exit_privileged
.Lexception:
    ldp x0, x1, [sp], #16
    // An exception from an EL0 task that the failure return admits does not return here.
    bl os_cpu_return_el0_try
    b platform_exception
