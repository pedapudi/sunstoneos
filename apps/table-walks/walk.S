/* SPDX-License-Identifier: BSD-3-Clause */
/* Register-only measured interval. The root's MMIO and monitor blocks are
 * excluded; every load is inside the first 128 MiB of mapped DRAM. */
.text
.balign 64
.global table_walk_loop
.type table_walk_loop,%function
table_walk_loop:
    mov x2, #0
    mov x3, #0x40000000
    mov x4, #0
    dsb ish
    isb
    mrs x5, pmccntr_el0
    mrs x6, cntpct_el0
    mrs x7, pmevcntr4_el0
    mrs x8, pmevcntr5_el0
    mrs x9, pmevcntr3_el0
1:  tlbi vmalle1
    dsb ish
    isb
    add x10, x3, x2, lsl #21
    add x10, x10, x2, lsl #6
    ldr x11, [x10]
    eor x4, x4, x11
    add x2, x2, #1
    and x2, x2, #63
    cmp x2, #16
    cinc x2, x2, eq
    subs x1, x1, #1
    b.ne 1b
    dsb ish
    isb
    mrs x10, pmccntr_el0
    mrs x11, cntpct_el0
    mrs x12, pmevcntr4_el0
    mrs x13, pmevcntr5_el0
    mrs x14, pmevcntr3_el0
    sub x5, x10, x5
    sub x6, x11, x6
    sub w7, w12, w7
    sub w8, w13, w8
    sub w9, w14, w9
    stp x5, x6, [x0]
    stp x7, x8, [x0, #16]
    stp x9, x4, [x0, #32]
    ret
.size table_walk_loop, .-table_walk_loop
