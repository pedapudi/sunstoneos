.section .text.boot, "ax"
.global _start
.type _start, %function
_start:
    msr daifset, #0xf
    mrs x0, mpidr_el1
    and x0, x0, #0xff
    cbnz x0, .Lpark
    // The first platform expects a firmware handoff at EL1, MMU disabled.
    mrs x0, CurrentEL
    cmp x0, #4
    b.ne .Lpark
    msr spsel, #1
    ldr x0, =__exception_stack_top
    mov sp, x0
    ldr x0, =_freertos_vector_table
    msr vbar_el1, x0
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    isb
    ldr x0, =__bss_start
    ldr x1, =__bss_end
.Lclear:
    cmp x0, x1
    b.hs .Lmain
    str xzr, [x0], #8
    b .Lclear
.Lmain:
    bl kernel_main
.Lpark:
    wfe
    b .Lpark
.size _start, .-_start
