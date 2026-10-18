/* QEMU harness: all CPUs start from an immutable ROM branch at secure EL3.
 * It substitutes CPU release only; actual cached tables and enable helpers run.
 */
.section .text.a733_smp_boot,"ax"
.global a733_smp_entry
a733_smp_entry:
    msr daifset, #15
    mrs x20, mpidr_el1
    and x20, x20, #255
    adrp x0, fixture_el3_stacks
    add x0, x0, :lo12:fixture_el3_stacks
    add x1, x20, #1
    add x0, x0, x1, lsl #14
    mov sp, x0
    adrp x0, a733_cached_el1_vectors
    add x0, x0, :lo12:a733_cached_el1_vectors
    msr vbar_el3, x0
    msr vbar_el1, x0
    mov x0, #0x430
    msr scr_el3, x0
    mov x0, #0x0800
    movk x0, #0x30d0, lsl #16
    msr sctlr_el1, x0
    mov x0, x20
    bl fixture_el3_prepare
    adrp x0, a733_cached_el3_l1
    add x0, x0, :lo12:a733_cached_el3_l1
    mov x1, #0xffff
    movk x1, #0xffff, lsl #16
    bl a733_cpu_coherence_enable_el3
    cmp w0, #1
    b.ne fixture_exception
    mov x0, x20
    bl fixture_el3_mapped
    mov x0, #0x531
    msr scr_el3, x0
    mov x0, #0x80000000
    msr hcr_el2, x0
    msr cptr_el3, xzr
    mov x0, #0x33ff
    msr cptr_el2, x0
    mov x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    msr tpidr_el1, x20
    adrp x0, fixture_el1_stacks
    add x0, x0, :lo12:fixture_el1_stacks
    add x1, x20, #1
    add x0, x0, x1, lsl #15
    msr sp_el1, x0
    adr x0, .Lel1
    msr elr_el3, x0
    mov x0, #0x3c5
    msr spsr_el3, x0
    isb
    eret
.Lel1:
    adrp x0, a733_cached_el1_l1
    add x0, x0, :lo12:a733_cached_el1_l1
    mov x1, #0xffff
    movk x1, #0xffff, lsl #16
    bl a733_cpu_coherence_enable_el1
    cmp w0, #1
    b.ne fixture_exception
    mov x0, x20
    bl fixture_el1
    b fixture_exception
.balign 2048
.global a733_cached_el1_vectors
a733_cached_el1_vectors:
.rept 16
    b fixture_exception
    .balign 128
.endr
fixture_exception:
    mov x0, #1
    mov x1, #0xdead
    b fixture_exit
.global fixture_exit
fixture_exit:
    /* Semihost extended exit accepts status in the second 64-bit word. */
    sub sp, sp, #16
    mov x2, #0x26
    movk x2, #2, lsl #16
    cmp x0, #0
    cset x3, ne
    stp x2, x3, [sp]
    mov x1, sp
    mov x0, #0x20
    hlt #0xf000
1:  wfe
    b 1b
.section .note.GNU-stack,"",%progbits
