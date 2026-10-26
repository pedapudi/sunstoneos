.syntax unified
.arch armv8-a
.arm
.section .text, "ax"
.global _start

// Table-driven register reader for the DRAM training capture
// (docs/designs/dram-fast-boot.md). The host runs it from FEL at 0x85000
// after the DDR wrapper has trained DRAM, once per stage. Each stage table
// lies at 0x43000000:
//   +0   magic 0x50414344
//   +4   version 1
//   +8   entry count, 1..512
//   +12  output address
//   +16  progress: index of the entry being read, written before its reads
//   +20  MicroContMuxSel value read before the first PHY entry
//   +24  UcclkHclkEnables value read before the first bit-9 entry
//   +28  0x454e4f44 written last, after the PHY controls are restored
//   +32  entries of 16 bytes: address, count (1..0x10000), flags, zero
// Flags bits 1:0 select 16-bit (1) or 32-bit (2) reads. Bit 8 makes the
// PHY registers readable by writing MicroContMuxSel = 0, as boot0's
// training save routine at 0x576e4 does; bit 9 also writes
// UcclkHclkEnables = 3, as NXP's trained-register save does. Both values
// are restored at the end. No other register is written. Each entry's
// output starts on a 4-byte boundary.

.ifdef TABLE_ADDR
.equ TABLE, TABLE_ADDR          // SRAM build for captures without trained DRAM.
.else
.equ TABLE, 0x43000000
.endif
.equ MAGIC, 0x50414344
.equ DONE, 0x454e4f44
.equ MUXSEL, 0x0aaa0000        // PHY CSR 0xd0000, MicroContMuxSel
.equ UCCLK, 0x0aa80100         // PHY CSR 0xc0080, UcclkHclkEnables
.equ MAX_ENTRIES, 512
.equ MAX_COUNT, 0x10000

_start:
    push {r4-r11, lr}
    mrs r12, cpsr
    push {r12}
    cpsid if
    ldr r4, =TABLE
    ldr r0, [r4]
    ldr r1, =MAGIC
    cmp r0, r1
    bne leave                    // Refuse without any register access.
    ldr r0, [r4, #4]
    cmp r0, #1
    bne leave
    ldr r5, [r4, #8]
    cmp r5, #0
    beq leave
    cmp r5, #MAX_ENTRIES
    bhi leave
    ldr r6, [r4, #12]
    add r7, r4, #32
    mov r8, #0                   // Entry index.
    mov r9, #0                   // Bit 0: MUXSEL cleared; bit 1: UCCLK set.
    ldr r10, =MUXSEL
    ldr r11, =UCCLK
next:
    cmp r8, r5
    beq complete
    str r8, [r4, #16]
    dsb sy
    ldr r0, [r7]
    ldr r1, [r7, #4]
    ldr r2, [r7, #8]
    cmp r1, #0
    beq refuse
    cmp r1, #MAX_COUNT
    bhi refuse
    and r3, r2, #3
    cmp r3, #1
    beq width_ok
    cmp r3, #2
    bne refuse
width_ok:
    tst r2, #0x100
    beq read_entry
    tst r9, #1
    bne 1f
    ldrh r3, [r10]
    str r3, [r4, #20]
    mov r3, #0
    strh r3, [r10]
    orr r9, r9, #1
    dsb sy
1:  tst r2, #0x200
    beq read_entry
    tst r9, #2
    bne read_entry
    ldrh r3, [r11]                // Read only once the mux is cleared, as NXP does.
    str r3, [r4, #24]
    mov r3, #3
    strh r3, [r11]
    orr r9, r9, #2
    dsb sy
read_entry:
    and r3, r2, #3
    cmp r3, #2
    beq 3f
2:  ldrh r3, [r0], #2
    strh r3, [r6], #2
    subs r1, r1, #1
    bne 2b
    b 4f
3:  ldr r3, [r0], #4
    str r3, [r6], #4
    subs r1, r1, #1
    bne 3b
4:  add r6, r6, #3
    bic r6, r6, #3
    add r7, r7, #16
    add r8, r8, #1
    b next
refuse:
    mov r12, #0
    b restore
complete:
    mov r12, #1
restore:
    tst r9, #2
    ldrne r3, [r4, #24]
    strhne r3, [r11]
    tst r9, #1
    ldrne r3, [r4, #20]
    strhne r3, [r10]
    dsb sy
    cmp r12, #1
    ldreq r0, =DONE
    streq r0, [r4, #28]
    dsb sy
leave:
    pop {r12}
    msr cpsr_cxsf, r12
    pop {r4-r11, pc}
    .ltorg
