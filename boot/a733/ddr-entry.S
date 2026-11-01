.syntax unified
.arch armv8-a
.arm
.section .text, "ax"
.global _start
.global log_character
.global clock_ticks
.global clock_poll_limit

// RAM-only entry for the pinned Zero 3W boot0 image. The host places the
// first 12 KiB of boot0 at 0x8e000, preserving live FEL state at 0x47000.
_start:
    push {r4-r11, lr}
    mov r8, sp
    mrs r9, cpsr
    mrc p15, 0, r10, c1, c0, 0
    cpsid if
.ifdef DDR_RESTORE
    ldr sp, =0x9f000
.else
    ldr sp, =0x8d000
.endif
    ldr r11, =0x86000
    mov r0, #0
    mov r1, #32
    mov r2, r11
1:  str r0, [r2], #4
    subs r1, r1, #1
    bne 1b
    str r9, [r11, #16]
    str r10, [r11, #20]
    // The host must disable the MMU and caches before installing boot0.
    tst r10, #5
    bne bad_entry
    tst r10, #0x1000
    bne bad_entry
    mov r0, #1
    str r0, [r11, #4]
    bl swap_fel
    ldr r0, =0x82dd0
    ldr r1, =0x83c28
    mov r2, #0
2:  str r2, [r0], #4
    cmp r0, r1
    blo 2b
    // Enable the blob's formatted logging. Its UART output is redirected
    // to log_character by the host image preparer.
    mov r0, #1
    mov r1, #0
    ldr r3, =0x49899
    blx r3
    mov r0, #2
    str r0, [r11, #4]
    ldr r3, =0x48fad
    blx r3                         // PMIC, clocks, board DRAM profile.
    str r0, [r11, #12]
    cmp r0, #0
    bne finish
    // Copy the selected header parameters, bypassing storage-backed bootpara.
    ldr r0, =0x47038
    ldr r1, =0x831f8
    mov r2, #32
3:  ldr r3, [r0], #4
    str r3, [r1], #4
    subs r2, r2, #1
    bne 3b
.ifdef DDR_RESTORE
    ldr r3, =0x92000
    blx r3                         // Admit everything before editing parameters.
    cmp r0, #0
    bne restore_failed
    add r0, r11, #64
    stmia r0, {r4-r11}
    str sp, [r11, #96]
.endif
.ifdef DDR_PARAM_PATCH
    // Capture variants (docs/designs/dram-training-restore.md): clear
    // dram_tpr13 bit 2 to train P-state 0 only, and optionally replace the
    // training-step value in dram_tpr10 bits 28:16.
    ldr r1, =0x831f8
    ldr r3, [r1, #0x78]
    bic r3, r3, #4
    str r3, [r1, #0x78]
    ldr r3, [r1, #0x6c]
    ldr r0, =TPR10_CLEAR
    bic r3, r3, r0
    ldr r0, =TPR10_SET
    orr r3, r3, r0
    str r3, [r1, #0x6c]
.endif
    mov r0, #3
    str r0, [r11, #4]
    mov r0, #0
    ldr r1, =0x831f8
    ldr r3, =0x49bdd
    blx r3                         // init_DRAM(0, parameters), returns MiB.
    str r0, [r11, #8]
.ifdef DDR_RESTORE
    ldr r3, =0x92008
    blx r3
    cmp r0, #0
    bne restore_failed
.endif
    mov r0, #4
    str r0, [r11, #4]
finish:
    // boot0's board setup (stage 2 onward) leaves CPU-L at 1014 MHz and the
    // DSU at 780 MHz; the ROM, which a return to FEL does not re-enter, sets
    // both to 494 MHz. Put them back before FEL runs again.
    ldr r0, [r11, #4]
    cmp r0, #2
    blo 1f
    bl rom_clock_point
1:  dsb sy
    bl swap_fel
    mov r0, #5
    str r0, [r11, #4]
    ldr r0, =0x53524444            // "DDRS" in memory.
    str r0, [r11]
    b restore_context
bad_entry:
    mvn r0, #0
    str r0, [r11, #4]
restore_context:
    dsb sy
    mov sp, r8
    msr cpsr_cxsf, r9
    pop {r4-r11, pc}

.ifdef DDR_RESTORE
.global restore_hook
restore_hook:
    push {r0-r3, r12, lr}
    ldr r3, =0x92004
    blx r3
    cmp r0, #0
    bne restore_abort
    pop {r0-r3, r12, lr}
    ldr r12, =0x4bca9
    bx r12                         // Original Thumb call, with original arguments.
restore_abort:
    // Nested boot0 callees may own all callee-saved registers. Recover the
    // wrapper context from SRAM, then use its normal FEL swap and return.
    ldr r1, =0x86040
    ldmia r1, {r4-r11}
    ldr sp, [r11, #96]
restore_failed:
    str r0, [r11, #28]
    b finish
.endif

// CPU-L and the DSU back to the boot ROM's words (docs/vendor-firmware/
// boot-rom-and-boot0.md): PLL 0xf8802600 (N = 38), muxes 0x03010000 and
// 0x03010105 (PLL / 2). CPU-B keeps boot0's setting. The sequence is the CPU
// clock driver's (drivers/clock/a733_cpu_clock.c): park the mux on the
// oscillator, wait 20 us, write N, set the update bit, wait for it to clear
// (10 ms), require lock on three reads 3 us apart (70 ms), wait 20 us, write
// the divider, select the PLL and wait 20 us. CPU0 runs on CPU-L throughout,
// so the PLL changes only while parked. Mailbox word 15 is 0 on success, or
// 0x100 * domain (1 CPU-L, 3 DSU) + code: 1 PLL not enabled or busy (nothing
// written), 2 update timeout, 3 lock timeout, 4 counter not advancing, 5 mux
// did not park. Codes 2 and 3, and code 4 before the final mux write, leave
// that mux on the oscillator; a CPU-L failure leaves the DSU untouched.
.equ ROM_PLL, 0xf8802600
.equ PLL_ON, 0xf8000000
.equ PLL_UPDATE, 0x04000000
.equ PLL_LOCK, 0x10000000
.equ MUX_SOURCE, 0x07000000
.equ TICKS_3US, 72               // 24 MHz system counter
.equ TICKS_20US, 480
.equ TICKS_UPDATE, 240000        // 10 ms
.equ TICKS_LOCK, 1680000         // 70 ms
.equ POLL_LIMIT, 0x1000000       // Polls before a stopped counter is a fault.

rom_clock_point:
    push {r4, lr}
    ldr r0, =0x08871000
    ldr r1, =0x03010000
    bl rom_point
    movs r4, r0
    orrne r4, r4, #0x100
    bne 1f
    ldr r0, =0x08873000
    ldr r1, =0x03010105
    bl rom_point
    movs r4, r0
    orrne r4, r4, #0x300
1:  str r4, [r11, #60]
    pop {r4, pc}

// r0 = PLL word, r1 = final mux word; returns 0 or a code in r0.
rom_point:
    push {r4-r7, lr}
    mov r4, r0
    mov r5, r1
    ldr r6, [r4]
    ldr r7, [r4, #0x1c]
    ldr r2, =ROM_PLL
    cmp r6, r2
    cmpeq r7, r5
    moveq r0, #0
    beq 9f                         // Already at the ROM point.
    and r2, r6, #0xfc000000
    cmp r2, #PLL_ON
    movne r0, #1
    bne 9f
    bic r2, r7, #MUX_SOURCE
    str r2, [r4, #0x1c]            // Park on the oscillator.
    dsb sy
    ldr r2, [r4, #0x1c]
    tst r2, #MUX_SOURCE
    movne r0, #5
    bne 9f
    mov r0, #TICKS_20US
    bl clock_wait
    cmp r0, #0
    bne 9f
    bic r6, r6, #0xff00
    orr r6, r6, #(38 << 8)
    str r6, [r4]
    orr r6, r6, #PLL_UPDATE
    str r6, [r4]
    bl clock_ticks
    mov r7, r0
    ldr r12, clock_poll_limit
2:  ldr r2, [r4]
    tst r2, #PLL_UPDATE
    beq 3f
    bl clock_ticks
    sub r0, r0, r7
    ldr r1, =TICKS_UPDATE
    cmp r0, r1
    movhs r0, #2
    bhs 9f
    subs r12, r12, #1
    moveq r0, #4
    beq 9f
    b 2b
3:  bl clock_ticks
    mov r7, r0
    mov r6, #0                     // Consecutive lock reads.
    ldr r12, clock_poll_limit
4:  ldr r2, [r4]
    tst r2, #PLL_LOCK
    moveq r6, #0
    beq 5f
    add r6, r6, #1
    cmp r6, #3
    beq 6f
    mov r0, #TICKS_3US
    bl clock_wait
    cmp r0, #0
    bne 9f
5:  bl clock_ticks
    sub r0, r0, r7
    ldr r1, =TICKS_LOCK
    cmp r0, r1
    movhs r0, #3
    bhs 9f
    subs r12, r12, #1
    moveq r0, #4
    beq 9f
    b 4b
6:  mov r0, #TICKS_20US
    bl clock_wait
    cmp r0, #0
    bne 9f
    bic r2, r5, #MUX_SOURCE
    str r2, [r4, #0x1c]            // Divider first, still on the oscillator.
    str r5, [r4, #0x1c]            // Then the PLL.
    dsb sy
    mov r0, #TICKS_20US
    bl clock_wait
9:  pop {r4-r7, pc}

// Waits r0 counter ticks; returns 0, or 4 if the counter does not advance.
clock_wait:
    push {r4, r5, lr}
    mov r4, r0
    bl clock_ticks
    mov r5, r0
    ldr r3, clock_poll_limit
1:  bl clock_ticks
    sub r0, r0, r5
    cmp r0, r4
    movhs r0, #0
    bhs 2f
    subs r3, r3, #1
    bne 1b
    mov r0, #4
2:  pop {r4, r5, pc}

// Low word of the 24 MHz system counter, as boot0's timer reads it.
clock_ticks:
    isb
    mrrc p15, 0, r0, r1, c14
    bx lr
// Polls before a stopped counter is a fault; a host check lowers it.
clock_poll_limit:
    .word POLL_LIMIT
    .ltorg

swap_fel:
    ldr r0, =0x47000
    ldr r1, =0x8e000
    ldr r2, =0x3000
4:  ldr r3, [r0]
    ldr r12, [r1]
    str r12, [r0], #4
    str r3, [r1], #4
    subs r2, r2, #4
    bne 4b
    dsb sy
    isb
    bx lr

// AAPCS callback; r0 is the character. Saturation preserves the first log.
log_character:
    ldr r1, =0x86000
    ldr r2, [r1, #24]
    cmp r2, #0x4000
    bxhs lr
    ldr r3, =0xe0000
    strb r0, [r3, r2]
    add r2, r2, #1
    str r2, [r1, #24]
    bx lr
    .ltorg
