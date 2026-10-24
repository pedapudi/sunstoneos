/* SPDX-License-Identifier: GPL-2.0-or-later */
/* void a733_dram_copy_2mib(void *destination, const void *source)
 * Copy load for the latency window's phase B (docs/dram.md): 2 MiB in 64-byte
 * steps of two LDP and two STP of Q registers. Every task saves Q0-Q31
 * (configUSE_TASK_FPU_SUPPORT 2). The C sources build with
 * -mgeneral-regs-only, so the loop lives here. */
.arch armv8-a
.text
.section .text.a733_dram_copy_2mib,"ax",%progbits
.global a733_dram_copy_2mib
.type a733_dram_copy_2mib,%function
.p2align 4
a733_dram_copy_2mib:
    mov x2, #0x200000
1:  ldp q0, q1, [x1], #32
    ldp q2, q3, [x1], #32
    stp q0, q1, [x0], #32
    stp q2, q3, [x0], #32
    subs x2, x2, #64
    b.ne 1b
    ret
.size a733_dram_copy_2mib, .-a733_dram_copy_2mib
