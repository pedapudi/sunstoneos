.syntax unified
.arch armv8-a
.arm
.section .text, "ax"
.global _start
.global log_character

// RAM-only entry for the pinned Zero 3W boot0 image. The host places the
// first 12 KiB of boot0 at 0x8e000, preserving live FEL state at 0x47000.
_start:
    push {r4-r11, lr}
    mov r8, sp
    mrs r9, cpsr
    mrc p15, 0, r10, c1, c0, 0
    cpsid if
    ldr sp, =0x8d000
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
    mov r0, #3
    str r0, [r11, #4]
    mov r0, #0
    ldr r1, =0x831f8
    ldr r3, =0x49bdd
    blx r3                         // init_DRAM(0, parameters), returns MiB.
    str r0, [r11, #8]
    mov r0, #4
    str r0, [r11, #4]
finish:
    dsb sy
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
