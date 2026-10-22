/* SPDX-License-Identifier: BSD-3-Clause */
/* Register definitions and maintenance ordering adapted from Trusted Firmware-A.
 * Copyright (c) 2017-2026, Arm Limited and Contributors. All rights reserved.
 */
#include "a733_cpu_coherence.h"
.if (A733_CPU_COHERENCE_TCR >> 48)
.error "A733_CPU_COHERENCE_TCR needs a fourth move"
.endif
.section .text.a733_cpu_coherence,"ax"
.balign 4
/* Callback ABI: x0 unused, w1 register, x2 output/value. */
.macro read_case number, reg
    cmp w1, #\number
    b.ne 1f
    mrs x3, \reg
    str x3, [x2]
    mov w0, #1
    ret
1:
.endm
.global a733_cpu_coherence_reg_read
.type a733_cpu_coherence_reg_read,%function
a733_cpu_coherence_reg_read:
    mrs x3, CurrentEL
    cmp x3, #12
    b.ne .Lrefuse
    read_case 0, CurrentEL
    read_case 1, sctlr_el3
    read_case 2, sctlr_el1
    read_case 3, daif
    read_case 4, midr_el1
    read_case 5, mpidr_el1
    read_case 6, clidr_el1
    read_case 7, id_aa64mmfr0_el1
    read_case 8, S3_0_C15_C3_1
    read_case 9, S3_0_C15_C3_0
    read_case 10, S3_0_C15_C1_0
    read_case 11, S3_0_C15_C1_1
    read_case 12, S3_0_C15_C1_2
    read_case 13, S3_0_C15_C1_4
    read_case 14, S3_0_C15_C3_3
    b .Lrefuse
.size a733_cpu_coherence_reg_read,.-a733_cpu_coherence_reg_read
.macro write_case number, reg
    cmp w1, #\number
    b.ne 1f
    msr \reg, x2
    isb
    mov w0, #1
    ret
1:
.endm
.global a733_cpu_coherence_reg_write
.type a733_cpu_coherence_reg_write,%function
a733_cpu_coherence_reg_write:
    mrs x3, CurrentEL
    cmp x3, #12
    b.ne .Lrefuse
    mrs x3, daif
    and x3, x3, #0x3c0
    cmp x3, #0x3c0
    b.ne .Lrefuse
    mrs x3, sctlr_el3
    mov x4, #0x1005
    movk x4, #0x200, lsl #16
    tst x3, x4
    b.ne .Lrefuse
    write_case 10, S3_0_C15_C1_0
    write_case 11, S3_0_C15_C1_1
    write_case 12, S3_0_C15_C1_2
    write_case 13, S3_0_C15_C1_4
    cmp w1, #14
    b.ne 1f
    mrs x3, mpidr_el1
    and x3, x3, #0xff00
    cbnz x3, .Lrefuse
    msr S3_0_C15_C3_3, x2
    isb
    mov w0, #1
    ret
1:
    write_case 15, S3_6_C15_C8_0
    write_case 16, S3_6_C15_C8_2
    write_case 17, S3_6_C15_C8_3
    write_case 18, S3_6_C15_C8_1
    b .Lrefuse
.size a733_cpu_coherence_reg_write,.-a733_cpu_coherence_reg_write
/* Tick deadline in x2, previous sampled counter x14. */
.macro deadline_check
    isb
    mrs x15, cntpct_el0
    cmp x15, x14
    b.lo .Lrefuse
    cmp x15, x2
    b.hs .Lrefuse
    mov x14, x15
.endm
.global a733_cpu_coherence_publish
.type a733_cpu_coherence_publish,%function
a733_cpu_coherence_publish:
    mrs x3, CurrentEL
    cmp x3, #12
    b.ne .Lrefuse
    mrs x3, sctlr_el3
    mov x4, #0x1005
    movk x4, #0x200, lsl #16
    tst x3, x4
    b.ne .Lrefuse
    mrs x3, daif
    and x3, x3, #0x3c0
    cmp x3, #0x3c0
    b.ne .Lrefuse
    mov x3, #0x40000000
    cmp x0, x3
    b.lo .Lrefuse
    cbz x1, .Lrefuse
    adds x1, x0, x1
    b.cs .Lrefuse
    mov x3, #0x48000000
    cmp x1, x3
    b.hi .Lrefuse
    mrs x3, ctr_el0
    ubfx x3, x3, #16, #4
    cmp x3, #4 /* Both admitted CPU families use 64-byte data lines. */
    b.ne .Lrefuse
    mov x14, xzr
    deadline_check
    and x0, x0, #-64
1:
    dc cvac, x0
    deadline_check
    add x0, x0, #64
    cmp x0, x1
    b.lo 1b
    dsb sy
    deadline_check
    mov w0, #1
    ret
.size a733_cpu_coherence_publish,.-a733_cpu_coherence_publish

/* EL3 or EL1 with translation and caches off; DAIF masked; owned DRAM range.
 * Clean and invalidate never discards data, so the loop needs no per-line
 * deadline check to keep a refusal harmless. */
.global a733_cpu_coherence_flush
.type a733_cpu_coherence_flush,%function
a733_cpu_coherence_flush:
    mrs x3, CurrentEL
    cmp x3, #12
    b.ne 1f
    mrs x4, sctlr_el3
    b 2f
1:
    cmp x3, #4
    b.ne .Lrefuse
    mrs x4, sctlr_el1
2:
    mov x3, #0x1005
    movk x3, #0x200, lsl #16
    tst x4, x3
    b.ne .Lrefuse
    mrs x3, daif
    and x3, x3, #0x3c0
    cmp x3, #0x3c0
    b.ne .Lrefuse
    mov x3, #0x40000000
    cmp x0, x3
    b.lo .Lrefuse
    cbz x1, .Lrefuse
    adds x1, x0, x1
    b.cs .Lrefuse
    mov x3, #0x48000000
    cmp x1, x3
    b.hi .Lrefuse
    mrs x3, ctr_el0
    ubfx x3, x3, #16, #4
    cmp x3, #4 /* Both admitted CPU families use 64-byte data lines. */
    b.ne .Lrefuse
    mov x14, xzr
    deadline_check
    and x0, x0, #-64
1:
    dc civac, x0
    add x0, x0, #64
    cmp x0, x1
    b.lo 1b
    dsb sy
    deadline_check
    mov w0, #1
    ret
.size a733_cpu_coherence_flush,.-a733_cpu_coherence_flush

/* EL1 entry admission shared by the table check and the cache enable.
 * In: x0 TTBR, x1 deadline. Out: x2 deadline, x4 SCTLR_EL1, x14 counter. */
.macro el1_admit
    mov x2, x1 /* Shared deadline macro ABI. */
    mrs x3, CurrentEL
    cmp x3, #4
    b.ne .Lrefuse
    mrs x3, daif
    and x3, x3, #0x3c0
    cmp x3, #0x3c0
    b.ne .Lrefuse
    mrs x4, sctlr_el1
    mov x3, #0x1005
    movk x3, #0x200, lsl #16
    tst x4, x3
    b.ne .Lrefuse
    tst x0, #4095
    b.ne .Lrefuse
    mov x3, #0x42000000
    cmp x0, x3
    b.lo .Lrefuse
    mov x3, #0x42200000
    cmp x0, x3
    b.hs .Lrefuse
    mov x14, xzr
    deadline_check
.endm
/* DC CIVAC over the 4 KiB page at \page, four lines per pass. Clobbers x5, x6. */
.macro flush_page page
    mov x5, \page
    add x6, \page, #4096
71:
    dc civac, x5
    add x5, x5, #64
    dc civac, x5
    add x5, x5, #64
    dc civac, x5
    add x5, x5, #64
    dc civac, x5
    add x5, x5, #64
    cmp x5, x6
    b.lo 71b
.endm
/* Add the 512 words of the 4 KiB page at \page to x5, one line per pass.
 * Clobbers x6, x7, x9-x13, x15-x17. */
.macro sum_page page
    mov x6, \page
    add x7, \page, #4096
72:
    ldp x9, x10, [x6], #64
    ldp x11, x12, [x6, #-48]
    ldp x13, x15, [x6, #-32]
    ldp x16, x17, [x6, #-16]
    add x5, x5, x9
    add x5, x5, x10
    add x5, x5, x11
    add x5, x5, x12
    add x5, x5, x13
    add x5, x5, x15
    add x5, x5, x16
    add x5, x5, x17
    cmp x6, x7
    b.lo 72b
.endm
/* With the MMU off, EL1 addresses the Nonsecure space the EL1 walker reads.
 * Remove every line of the root and of the level-2 page that root entry 1
 * names, then sum both pages with Device loads and compare the sum with the
 * value core 0 recorded at EL3. Root entry 1 must be exactly a table
 * descriptor to another monitor page. Needs x0, x2, x14 from el1_admit;
 * leaves x0, x2, x4 intact and clobbers x5-x13, x15-x17. */
.macro el1_tables_check
    mrs x5, ctr_el0
    ubfx x5, x5, #16, #4
    cmp x5, #4
    b.ne .Lrefuse
    flush_page x0
    dsb sy
    ldr x8, [x0, #8]
    and x5, x8, #4095
    cmp x5, #3
    b.ne .Lrefuse
    sub x8, x8, #3
    mov x5, #0x42000000
    cmp x8, x5
    b.lo .Lrefuse
    mov x5, #0x42200000
    cmp x8, x5
    b.hs .Lrefuse
    cmp x8, x0
    b.eq .Lrefuse
    flush_page x8
    dsb sy
    deadline_check
    mov x5, xzr
    sum_page x0
    sum_page x8
    adrp x6, a733_cached_el1_table_sum
    ldr x6, [x6, :lo12:a733_cached_el1_table_sum]
    cmp x5, x6
    b.ne .Lrefuse
    deadline_check
.endm

.global a733_cpu_coherence_check_el1
.type a733_cpu_coherence_check_el1,%function
a733_cpu_coherence_check_el1:
    el1_admit
    el1_tables_check
    mov w0, #1
    ret
.size a733_cpu_coherence_check_el1,.-a733_cpu_coherence_check_el1

.global a733_cpu_coherence_enable_el1
.type a733_cpu_coherence_enable_el1,%function
a733_cpu_coherence_enable_el1:
    el1_admit
    el1_tables_check
    /* Preserve every unrelated SCTLR bit. */
    dsb sy
    ic iallu
    dsb sy
    isb
    mov x3, #0xff00
    movk x3, #0x44, lsl #16
    msr mair_el1, x3
    mov x3, #(A733_CPU_COHERENCE_TCR & 0xffff)
    movk x3, #((A733_CPU_COHERENCE_TCR >> 16) & 0xffff), lsl #16
    movk x3, #((A733_CPU_COHERENCE_TCR >> 32) & 0xffff), lsl #32
    msr tcr_el1, x3
    msr ttbr0_el1, x0
    msr ttbr1_el1, xzr
    isb
    /* Duplicate local invalidation also covers A76 1286807. */
    tlbi vmalle1
    dsb sy
    tlbi vmalle1
    dsb sy
    isb
    deadline_check
    orr x4, x4, #1
    orr x4, x4, #4
    orr x4, x4, #0x1000
    msr sctlr_el1, x4
    isb
    deadline_check
    mrs x3, sctlr_el1
    cmp x3, x4
    b.ne .Lrefuse
    mrs x3, ttbr0_el1
    cmp x3, x0
    b.ne .Lrefuse
    mov w0, #1
    ret
.size a733_cpu_coherence_enable_el1,.-a733_cpu_coherence_enable_el1
.global a733_cpu_coherence_enable_el3
.type a733_cpu_coherence_enable_el3,%function
a733_cpu_coherence_enable_el3:
    mov x2, x1 /* Shared deadline macro ABI. */
    mrs x3, CurrentEL
    cmp x3, #12
    b.ne .Lrefuse
    mrs x3, daif
    and x3, x3, #0x3c0
    cmp x3, #0x3c0
    b.ne .Lrefuse
    mrs x4, sctlr_el3
    mov x3, #0x1005
    movk x3, #0x200, lsl #16
    tst x4, x3
    b.ne .Lrefuse
    tst x0, #4095
    b.ne .Lrefuse
    mov x3, #0x42000000
    cmp x0, x3
    b.lo .Lrefuse
    mov x3, #0x42200000
    cmp x0, x3
    b.hs .Lrefuse
    mov x14, xzr
    deadline_check
    /* Tables were published before arrival; preserve every unrelated SCTLR bit. */
    dsb sy
    ic iallu
    dsb sy
    isb
    mov x3, #0xff00
    movk x3, #0x44, lsl #16
    msr mair_el3, x3
    mov x3, #0x3019
    movk x3, #0x8082, lsl #16
    msr tcr_el3, x3
    msr ttbr0_el3, x0
    isb
    /* Duplicate local invalidation also covers A76 1286807. */
    tlbi alle3
    dsb sy
    tlbi alle3
    dsb sy
    isb
    deadline_check
    orr x4, x4, #1
    orr x4, x4, #4
    orr x4, x4, #0x1000
    msr sctlr_el3, x4
    isb
    deadline_check
    mrs x3, sctlr_el3
    cmp x3, x4
    b.ne .Lrefuse
    mrs x3, ttbr0_el3
    cmp x3, x0
    b.ne .Lrefuse
    mov w0, #1
    ret
.size a733_cpu_coherence_enable_el3,.-a733_cpu_coherence_enable_el3
.Lrefuse:
    mov w0, #-1
    ret
.section .note.GNU-stack,"",%progbits
