// SPDX-License-Identifier: Apache-2.0
// Leaf kernels of the memory qualification image. docs/memory-qualification.md
// gives the per-line instruction budgets that checks/memtest-emulate.py counts.
//
// Walk kernels take a struct memtest_walk (memtest.h) and keep its state in
// x19-x26 while they run:
//   x19 walk  x20 cursor  x21 end  x22 block step  x23 deadline
//   x24 renew_at  x25 progress pointer  x26 seed
// After each 2 MiB block they read the counter once, renew the secure timer
// with SMC 0x734 when renew_at has passed and stop at the deadline. The
// renewal handler (monitor_sync) returns zero in x0 and preserves x1-x30; the
// kernels keep no live value in x0-x3 across it.
#include "memtest.h"

#define BLOCK (1 << 21)

.macro walk_enter
    stp x19, x20, [sp, #-112]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp d8, d9, [sp, #80]
    stp d10, d11, [sp, #96]
    mov x19, x0
    ldp x20, x21, [x19, #MEMTEST_WALK_CURSOR]
    ldr x22, [x19, #MEMTEST_WALK_BLOCK_STEP]
    ldp x26, x23, [x19, #MEMTEST_WALK_SEED]
    ldr x24, [x19, #MEMTEST_WALK_RENEW_AT]
    ldr x25, [x19, #MEMTEST_WALK_PROGRESS]
.endm

// Return \code with cursor and renew_at written back.
.macro walk_leave code
    str x20, [x19, #MEMTEST_WALK_CURSOR]
    str x24, [x19, #MEMTEST_WALK_RENEW_AT]
    mov w0, #\code
    ldp d10, d11, [sp, #96]
    ldp d8, d9, [sp, #80]
    ldp x27, x28, [sp, #64]
    ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #112
    ret
.endm

// Counter read and renewal after a block; leaves the counter in x16.
.macro block_clock
    isb
    mrs x16, cntpct_el0
    cmp x16, x24
    b.lo 6f
    mov x0, #0x734
    smc #0
    ldr x0, [x19, #MEMTEST_WALK_RENEWALS]
    add x0, x0, #1
    str x0, [x19, #MEMTEST_WALK_RENEWALS]
    ldr x0, =MEMTEST_RENEW_TICKS
    add x24, x16, x0
6:
.endm

// Advance to the next block; stop at the deadline or at the end.
.macro block_next again, budget, done
    add x20, x20, x22
    cmp x16, x23
    b.hs \budget
    cmp x20, x21
    b.ne \again
    b \done
.endm

.section .text.memtest_kernels, "ax"

// Window scan: for each 4 KiB page load the first words of lines 31 and 63
// and stop when a value minus (K | recovery start) is below the recovery
// size. Sixteen loads of eight pages issue before the first compare.
.global memtest_scan
.type memtest_scan, %function
memtest_scan:
    walk_enter
    cmp x20, x21
    b.eq 9f
1:  str x20, [x25]
    ldp x7, x9, [x19, #MEMTEST_WALK_MATCH_BASE]
    mov x5, x20
    add x6, x20, #BLOCK
2:  ldr x0, [x5, #0x7c0]
    ldr x1, [x5, #0xfc0]
    ldr x2, [x5, #0x17c0]
    ldr x3, [x5, #0x1fc0]
    ldr x4, [x5, #0x27c0]
    ldr x8, [x5, #0x2fc0]
    ldr x10, [x5, #0x37c0]
    ldr x11, [x5, #0x3fc0]
    ldr x12, [x5, #0x47c0]
    ldr x13, [x5, #0x4fc0]
    ldr x14, [x5, #0x57c0]
    ldr x15, [x5, #0x5fc0]
    ldr x16, [x5, #0x67c0]
    ldr x17, [x5, #0x6fc0]
    ldr x27, [x5, #0x77c0]
    ldr x28, [x5, #0x7fc0]
    sub x0, x0, x7
    sub x1, x1, x7
    sub x2, x2, x7
    sub x3, x3, x7
    sub x4, x4, x7
    sub x8, x8, x7
    sub x10, x10, x7
    sub x11, x11, x7
    sub x12, x12, x7
    sub x13, x13, x7
    sub x14, x14, x7
    sub x15, x15, x7
    sub x16, x16, x7
    sub x17, x17, x7
    sub x27, x27, x7
    sub x28, x28, x7
    cmp x0, x9
    ccmp x1, x9, #0, hs
    ccmp x2, x9, #0, hs
    ccmp x3, x9, #0, hs
    ccmp x4, x9, #0, hs
    ccmp x8, x9, #0, hs
    ccmp x10, x9, #0, hs
    ccmp x11, x9, #0, hs
    ccmp x12, x9, #0, hs
    ccmp x13, x9, #0, hs
    ccmp x14, x9, #0, hs
    ccmp x15, x9, #0, hs
    ccmp x16, x9, #0, hs
    ccmp x17, x9, #0, hs
    ccmp x27, x9, #0, hs
    ccmp x28, x9, #0, hs
    b.lo 8f
    add x5, x5, #0x8000
    cmp x5, x6
    b.ne 2b
    block_clock
    block_next 1b, 7f, 9f
7:  walk_leave MEMTEST_WALK_BUDGET
8:  str x5, [x19, #MEMTEST_WALK_FOUND]
    walk_leave MEMTEST_WALK_HIT
9:  walk_leave MEMTEST_WALK_DONE
.size memtest_scan, .-memtest_scan

// Signature write pass: zero line 0 of each page without a linefill, store
// the page address XOR the seed and clean it to DRAM. DSB SY per block ties
// a denied write-back to the block that the progress word names.
.global memtest_sig_write
.type memtest_sig_write, %function
memtest_sig_write:
    walk_enter
    cmp x20, x21
    b.eq 9f
1:  str x20, [x25]
    mov x5, x20
    add x6, x20, #BLOCK
2:
.rept 4
    dc zva, x5
    eor x0, x5, x26
    str x0, [x5]
    dc civac, x5
    add x5, x5, #0x1000
.endr
    cmp x5, x6
    b.ne 2b
    dsb sy
    block_clock
    block_next 1b, 7f, 9f
7:  walk_leave MEMTEST_WALK_BUDGET
9:  walk_leave MEMTEST_WALK_DONE
.size memtest_sig_write, .-memtest_sig_write

// Signature read pass: eight pages per batch. Word 0 of page k of a 32 KiB
// batch at b XOR (b XOR seed) must equal k << 12.
.global memtest_sig_read
.type memtest_sig_read, %function
memtest_sig_read:
    walk_enter
    cmp x20, x21
    b.eq 9f
1:  str x20, [x25]
    mov x5, x20
    add x6, x20, #BLOCK
    mov x1, #0x1000
    mov x2, #0x2000
    mov x3, #0x3000
    mov x4, #0x4000
    mov x7, #0x5000
    mov x9, #0x6000
    mov x10, #0x7000
2:  ldr x11, [x5]
    ldr x12, [x5, #0x1000]
    ldr x13, [x5, #0x2000]
    ldr x14, [x5, #0x3000]
    ldr x15, [x5, #0x4000]
    ldr x16, [x5, #0x5000]
    ldr x17, [x5, #0x6000]
    ldr x27, [x5, #0x7000]
    eor x8, x5, x26
    eor x11, x11, x8
    eor x12, x12, x8
    eor x13, x13, x8
    eor x14, x14, x8
    eor x15, x15, x8
    eor x16, x16, x8
    eor x17, x17, x8
    eor x27, x27, x8
    cmp x11, #0
    ccmp x12, x1, #0, eq
    ccmp x13, x2, #0, eq
    ccmp x14, x3, #0, eq
    ccmp x15, x4, #0, eq
    ccmp x16, x7, #0, eq
    ccmp x17, x9, #0, eq
    ccmp x27, x10, #0, eq
    b.ne 8f
    add x5, x5, #0x8000
    cmp x5, x6
    b.ne 2b
    block_clock
    block_next 1b, 7f, 9f
7:  walk_leave MEMTEST_WALK_BUDGET
8:  str x5, [x19, #MEMTEST_WALK_FOUND]
    walk_leave MEMTEST_WALK_HIT
9:  walk_leave MEMTEST_WALK_DONE
.size memtest_sig_read, .-memtest_sig_read

// March setup for the block at x20. Line order follows the line step in x7:
// ascending from the block base, or descending from its last line. Sets x5
// (store pointer) and x6 (load pointer) to the first line, v16-v19 to its
// expected words P(a) = a + seed (or ~P when x27 is set), v20 to the
// per-line change of the expected words, and clears the accumulator v24.
.macro march_block
    str x20, [x25]
    mov x5, x20
    cmp x7, #0
    b.gt 2f
    add x5, x20, #BLOCK
    sub x5, x5, #64
2:  mov x6, x5
    add x3, x5, x26
    dup v29.2d, x3
    add v16.2d, v29.2d, v25.2d
    add v17.2d, v29.2d, v26.2d
    add v18.2d, v29.2d, v27.2d
    add v19.2d, v29.2d, v28.2d
    cbz x27, 3f
    mvn v16.16b, v16.16b
    mvn v17.16b, v17.16b
    mvn v18.16b, v18.16b
    mvn v19.16b, v19.16b
3:  movi v24.2d, #0
    // Prefetch 16 lines ahead of the line being checked: the load pointer
    // already points one line ahead. In a member's last block the last 16
    // lines prefetch the line the next load reads instead of reaching past
    // the member.
    lsl x8, x7, #4
    sub x8, x8, x7
.endm

.macro march_enter
    walk_enter
    ldr x7, [x19, #MEMTEST_WALK_LINE_STEP]
    ldr x27, [x19, #MEMTEST_WALK_COMPLEMENT]
    mov x3, #8
    movi v25.2d, #0
    mov v25.d[1], x3
    mov x3, #16
    dup v29.2d, x3
    add v26.2d, v25.2d, v29.2d
    add v27.2d, v26.2d, v29.2d
    add v28.2d, v27.2d, v29.2d
    // Expected words move by the line step, or against it for ~P.
    cmp x27, #0
    neg x3, x7
    csel x3, x7, x3, eq
    dup v20.2d, x3
.endm

// Last-block prefetch limit: x8 = 0 when the next block is the end.
.macro last_block_prefetch
    add x3, x20, x22
    cmp x3, x21
    csel x8, xzr, x8, eq
.endm

// One line of a read-and-write element: line n is in \d (loaded one line
// earlier), line n + 1 is loaded into \n.
.macro rw_line d0, d1, d2, d3, n0, n1, n2, n3, load
.if \load
    prfm pldl1strm, [x6, x8]
    ld1 {\n0\().2d, \n1\().2d, \n2\().2d, \n3\().2d}, [x6], x7
.endif
    eor v4.16b, \d0\().16b, v16.16b
    eor v5.16b, \d1\().16b, v17.16b
    eor v6.16b, \d2\().16b, v18.16b
    eor v7.16b, \d3\().16b, v19.16b
    orr v4.16b, v4.16b, v5.16b
    orr v6.16b, v6.16b, v7.16b
    orr v4.16b, v4.16b, v6.16b
    orr v24.16b, v24.16b, v4.16b
    mvn \d0\().16b, \d0\().16b
    mvn \d1\().16b, \d1\().16b
    mvn \d2\().16b, \d2\().16b
    mvn \d3\().16b, \d3\().16b
    st1 {\d0\().2d, \d1\().2d, \d2\().2d, \d3\().2d}, [x5], x7
    add v16.2d, v16.2d, v20.2d
    add v17.2d, v17.2d, v20.2d
    add v18.2d, v18.2d, v20.2d
    add v19.2d, v19.2d, v20.2d
.endm

.macro r_line d0, d1, d2, d3, n0, n1, n2, n3, load
.if \load
    prfm pldl1strm, [x6, x8]
    ld1 {\n0\().2d, \n1\().2d, \n2\().2d, \n3\().2d}, [x6], x7
.endif
    eor v4.16b, \d0\().16b, v16.16b
    eor v5.16b, \d1\().16b, v17.16b
    eor v6.16b, \d2\().16b, v18.16b
    eor v7.16b, \d3\().16b, v19.16b
    orr v4.16b, v4.16b, v5.16b
    orr v6.16b, v6.16b, v7.16b
    orr v4.16b, v4.16b, v6.16b
    orr v24.16b, v24.16b, v4.16b
    add v16.2d, v16.2d, v20.2d
    add v17.2d, v17.2d, v20.2d
    add v18.2d, v18.2d, v20.2d
    add v19.2d, v19.2d, v20.2d
.endm

// Eight lines of a checking element, alternating the two register sets. With
// last set, the eighth line loads nothing.
.macro eight line, last
    \line v0, v1, v2, v3, v8, v9, v10, v11, 1
    \line v8, v9, v10, v11, v0, v1, v2, v3, 1
    \line v0, v1, v2, v3, v8, v9, v10, v11, 1
    \line v8, v9, v10, v11, v0, v1, v2, v3, 1
    \line v0, v1, v2, v3, v8, v9, v10, v11, 1
    \line v8, v9, v10, v11, v0, v1, v2, v3, 1
    \line v0, v1, v2, v3, v8, v9, v10, v11, 1
.if \last
    \line v8, v9, v10, v11, v0, v1, v2, v3, 0
.else
    \line v8, v9, v10, v11, v0, v1, v2, v3, 1
.endif
.endm

// A checking march element over one block of 32768 lines: a prologue load,
// 4094 iterations of eight lines, then sixteen lines with no load after the
// last one. Lines 32752-32767 of a member's last block run with the
// prefetch limit.
.macro march_check line
    march_block
    ld1 {v0.2d, v1.2d, v2.2d, v3.2d}, [x6], x7
    mov x2, #4094
4:  eight \line, 0
    subs x2, x2, #1
    b.ne 4b
    last_block_prefetch
    eight \line, 0
    eight \line, 1
.endm

// Read-and-write element: read the expected words, write their complement.
.global memtest_march_rw
.type memtest_march_rw, %function
memtest_march_rw:
    march_enter
    cmp x20, x21
    b.eq 9f
1:  march_check rw_line
    block_clock
    umaxv s4, v24.4s
    fmov w0, s4
    cbnz w0, 8f
    block_next 1b, 7f, 9f
7:  walk_leave MEMTEST_WALK_BUDGET
8:  str x20, [x19, #MEMTEST_WALK_FOUND]
    add x20, x20, x22
    walk_leave MEMTEST_WALK_HIT
9:  walk_leave MEMTEST_WALK_DONE
.size memtest_march_rw, .-memtest_march_rw

// Read element: read and compare only.
.global memtest_march_r
.type memtest_march_r, %function
memtest_march_r:
    march_enter
    cmp x20, x21
    b.eq 9f
1:  march_check r_line
    block_clock
    umaxv s4, v24.4s
    fmov w0, s4
    cbnz w0, 8f
    block_next 1b, 7f, 9f
7:  walk_leave MEMTEST_WALK_BUDGET
8:  str x20, [x19, #MEMTEST_WALK_FOUND]
    add x20, x20, x22
    walk_leave MEMTEST_WALK_HIT
9:  walk_leave MEMTEST_WALK_DONE
.size memtest_march_r, .-memtest_march_r

// Write element: store the expected words of each line.
.global memtest_march_w
.type memtest_march_w, %function
memtest_march_w:
    march_enter
    cmp x20, x21
    b.eq 9f
1:  march_block
    mov x2, #4096
4:
.rept 8
    st1 {v16.2d, v17.2d, v18.2d, v19.2d}, [x5], x7
    add v16.2d, v16.2d, v20.2d
    add v17.2d, v17.2d, v20.2d
    add v18.2d, v18.2d, v20.2d
    add v19.2d, v19.2d, v20.2d
.endr
    subs x2, x2, #1
    b.ne 4b
    block_clock
    block_next 1b, 7f, 9f
7:  walk_leave MEMTEST_WALK_BUDGET
9:  walk_leave MEMTEST_WALK_DONE
.size memtest_march_w, .-memtest_march_w

// Harness kernels. Lengths are multiples of the unrolled step (512 bytes).
// Read: one LD1 of four Q registers per line, no PRFM.
.global memtest_read_up
.type memtest_read_up, %function
memtest_read_up:
    add x1, x0, x1
1:
.rept 4
    ld1 {v0.2d, v1.2d, v2.2d, v3.2d}, [x0], #64
.endr
    cmp x0, x1
    b.ne 1b
    ret
.size memtest_read_up, .-memtest_read_up

.global memtest_read_down
.type memtest_read_down, %function
memtest_read_down:
    sub x3, x0, #64
    add x0, x0, x1
    sub x0, x0, #64
    mov x2, #-64
1:
.rept 4
    ld1 {v0.2d, v1.2d, v2.2d, v3.2d}, [x0], x2
.endr
    cmp x0, x3
    b.ne 1b
    ret
.size memtest_read_down, .-memtest_read_down

// Write: two STP Q per whole line, so the A55 enters write streaming.
.global memtest_write
.type memtest_write, %function
memtest_write:
    add x1, x0, x1
    movi v0.2d, #0
    movi v1.2d, #0
    movi v2.2d, #0
    movi v3.2d, #0
1:
.irp offset, 0, 64, 128, 192
    stp q0, q1, [x0, #\offset]
    stp q2, q3, [x0, #(\offset + 32)]
.endr
    add x0, x0, #256
    cmp x0, x1
    b.ne 1b
    ret
.size memtest_write, .-memtest_write

// Copy: two LDP Q and two STP Q per line, eight lines per iteration.
.global memtest_copy
.type memtest_copy, %function
memtest_copy:
    add x2, x1, x2
1:
.irp offset, 0, 64, 128, 192, 256, 320, 384, 448
    ldp q0, q1, [x1, #\offset]
    ldp q2, q3, [x1, #(\offset + 32)]
    stp q0, q1, [x0, #\offset]
    stp q2, q3, [x0, #(\offset + 32)]
.endr
    add x0, x0, #512
    add x1, x1, #512
    cmp x1, x2
    b.ne 1b
    ret
.size memtest_copy, .-memtest_copy

// Dependent-load chase: each line's first word holds the next line address.
.global memtest_chase
.type memtest_chase, %function
memtest_chase:
1:
.rept 8
    ldr x0, [x0]
.endr
    subs x1, x1, #8
    b.ne 1b
    ret
.size memtest_chase, .-memtest_chase
.ltorg
