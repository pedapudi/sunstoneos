// Upload digest helper: SHA-256 of DRAM ranges the runner has written over FEL, so that each byte crosses the USB link
// once (docs/board-runner.md, "Verify uploads by digest").
//
// The ROM runs it with the FEL execute command, in A32 state with the MMU and data cache off. It is position independent:
// every address it uses is relative to _start, so the runner's check (checks/upload-digest.py) runs the same bytes under
// QEMU at another address. The request block sits PARAMS bytes after _start:
//   +0  magic 'UDG1'              written by the host
//   +4  count of ranges           written by the host, at most MAX_RANGES
//   +8  status                    written by the helper: STATUS_DONE or one of the STATUS_* refusals below
//   +12 ID_ISAR5                  written by the helper
//   +16 count x {address, bytes}  written by the host; each address a multiple of 4
//   then count x 32 bytes         written by the helper: the SHA-256 of each range, in digest byte order
// Loads use 32-bit elements on 4-byte aligned addresses, because a data access with the MMU off is Device memory, where
// an unaligned access faults. The helper uses the ROM's stack (40 bytes of saved registers and a 128-byte padding block),
// enables the floating-point and Advanced SIMD unit for the SHA-256 instructions, and restores CPACR and FPEXC before it
// returns. It uses q0-q3 and q8-q15 only, so the callee-saved d8-d15 are untouched.
    .syntax unified
    .arch armv8-a
    .fpu crypto-neon-fp-armv8
    .arm
    .text
    .global _start

    .equ PARAMS, 0x800
    .equ PARAMS_END, 0xd80
    .equ MAX_RANGES, (PARAMS_END - PARAMS - 16) / 40
    .equ MAGIC, 0x31474455              // 'UDG1'
    .equ STATUS_DONE, 0x454e4f44        // 'DONE'
    .equ STATUS_MAGIC, 0xe0000001       // the request block does not start with MAGIC
    .equ STATUS_NO_SHA2, 0xe0000002     // ID_ISAR5.SHA2 is zero: this core has no SHA-256 instructions
    .equ STATUS_COUNT, 0xe0000003       // more ranges than the request block holds
    .equ STATUS_ALIGN, 0xe0000004       // a range address is not a multiple of 4

_start:
    push    {r3-r11, lr}
    adr     r10, _start
    add     r10, r10, #PARAMS
    ldr     r0, [r10]
    movw    r1, #(MAGIC & 0xffff)
    movt    r1, #(MAGIC >> 16)
    cmp     r0, r1
    movw    r7, #(STATUS_MAGIC & 0xffff)
    movt    r7, #(STATUS_MAGIC >> 16)
    bne     finish
    mrc     p15, 0, r0, c0, c2, 5       // ID_ISAR5
    str     r0, [r10, #12]
    ubfx    r1, r0, #12, #4             // SHA2 field
    cmp     r1, #0
    movw    r7, #(STATUS_NO_SHA2 & 0xffff)
    movt    r7, #(STATUS_NO_SHA2 >> 16)
    beq     finish
    ldr     r11, [r10, #4]
    cmp     r11, #MAX_RANGES
    movw    r7, #(STATUS_COUNT & 0xffff)
    movt    r7, #(STATUS_COUNT >> 16)
    bhi     finish
    mrc     p15, 0, r8, c1, c0, 2       // CPACR, restored before return
    orr     r0, r8, #(0xf << 20)        // cp10 and cp11 full access
    mcr     p15, 0, r0, c1, c0, 2
    isb
    vmrs    r9, fpexc                   // FPEXC, restored before return
    orr     r0, r9, #(1 << 30)          // EN
    vmsr    fpexc, r0
    isb
    add     r4, r10, #16                // next range
    add     r5, r4, r11, lsl #3         // next digest
    adr     r6, round_constants
    movw    r7, #(STATUS_DONE & 0xffff)
    movt    r7, #(STATUS_DONE >> 16)
next_range:
    cmp     r11, #0
    beq     restore
    ldm     r4!, {r0, r1}               // address, bytes
    tst     r0, #3
    movwne  r7, #(STATUS_ALIGN & 0xffff)
    movtne  r7, #(STATUS_ALIGN >> 16)
    bne     restore
    adr     r2, initial_state
    vld1.32 {q0, q1}, [r2]
    lsrs    r3, r1, #6                  // whole blocks
    beq     tail
blocks:
    vld1.32 {q8, q9}, [r0]!
    vld1.32 {q10, q11}, [r0]!
    bl      compress
    subs    r3, r3, #1
    bne     blocks
tail:
    // The last 0..63 bytes, the 0x80 marker and the 64-bit bit count, in one or two blocks on the stack.
    sub     sp, sp, #128
    vmov.i32 q12, #0
    vmov.i32 q13, #0
    mov     r2, sp
    vst1.32 {q12, q13}, [r2]!
    vst1.32 {q12, q13}, [r2]!
    vst1.32 {q12, q13}, [r2]!
    vst1.32 {q12, q13}, [r2]!
    and     r3, r1, #63
    mov     r2, #0
1:  cmp     r2, r3
    beq     2f
    ldrb    r12, [r0, r2]
    strb    r12, [sp, r2]
    add     r2, r2, #1
    b       1b
2:  mov     r12, #0x80
    strb    r12, [sp, r3]
    cmp     r3, #56
    movlo   r3, #56                     // one block: the count fills bytes 56..63
    movhs   r3, #120                    // two blocks: bytes 120..127
    lsr     r12, r1, #29                // bit count, high word
    rev     r12, r12
    str     r12, [sp, r3]
    add     r3, r3, #4
    lsl     r12, r1, #3                 // bit count, low word
    rev     r12, r12
    str     r12, [sp, r3]
    mov     r0, sp
    vld1.32 {q8, q9}, [r0]!
    vld1.32 {q10, q11}, [r0]!
    bl      compress
    cmp     r3, #124
    bne     3f
    vld1.32 {q8, q9}, [r0]!
    vld1.32 {q10, q11}, [r0]!
    bl      compress
3:  add     sp, sp, #128
    vrev32.8 q0, q0
    vrev32.8 q1, q1
    vst1.32 {q0, q1}, [r5]!
    sub     r11, r11, #1
    b       next_range
restore:
    vmsr    fpexc, r9
    mcr     p15, 0, r8, c1, c0, 2
    isb
finish:
    str     r7, [r10, #8]
    dsb     sy
    pop     {r3-r11, pc}

// One SHA-256 compression: the block is in q8-q11 as little-endian words, the state a..d in q0 and e..h in q1.
// Clobbers q2, q8-q15 and r2.
    .macro  quad words, constant
    vld1.32 {q12}, [r2]!
    vadd.i32 q13, \words, q12
    vmov    q2, q0
    sha256h.32  q0, q1, q13
    sha256h2.32 q1, q2, q13
    .endm
    .macro  schedule words, next, third, fourth
    sha256su0.32 \words, \next
    sha256su1.32 \words, \third, \fourth
    .endm
compress:
    vmov    q14, q0
    vmov    q15, q1
    vrev32.8 q8, q8
    vrev32.8 q9, q9
    vrev32.8 q10, q10
    vrev32.8 q11, q11
    mov     r2, r6
    quad    q8
    quad    q9
    quad    q10
    quad    q11
    .rept 3
    schedule q8, q9, q10, q11
    quad    q8
    schedule q9, q10, q11, q8
    quad    q9
    schedule q10, q11, q8, q9
    quad    q10
    schedule q11, q8, q9, q10
    quad    q11
    .endr
    vadd.i32 q0, q0, q14
    vadd.i32 q1, q1, q15
    bx      lr

    .balign 16
initial_state:
    .word 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a
    .word 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
round_constants:
    .word 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5
    .word 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5
    .word 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3
    .word 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174
    .word 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc
    .word 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da
    .word 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7
    .word 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967
    .word 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13
    .word 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85
    .word 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3
    .word 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070
    .word 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5
    .word 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3
    .word 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208
    .word 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
