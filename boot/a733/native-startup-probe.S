// Fixed native-image checkpoints. The original startup owns stack and BSS.
.arch armv8-a
.section .text,"ax"
.global boot_checkpoint, cached_checkpoint, stop_checkpoint, park_checkpoint, fault_checkpoint
.global write_checkpoint, failed_checkpoint
boot_checkpoint:
    ldr x16, =0x42002000
    mov x17, #1
    str x17, [x16, #32]
    mov x17, sp
    str x17, [x16, #40]
    sub sp, sp, #0xb0           // Displaced native instruction.
    ldr x17, =0x410000d4
    br x17
cached_checkpoint:
    ldr x16, =0x42002000
    mov x17, #2
    b publish
stop_checkpoint:
    ldr x16, =0x42002000
    str x0, [x16, #96]
    str x1, [x16, #104]
    mov x17, #3
    b publish
park_checkpoint:
    ldr x16, =0x42002000
    mov x17, #4
    b publish
fault_checkpoint:
    ldr x16, =0x42002000
    str x18, [x16, #112]
    mrs x17, esr_el3
    str x17, [x16, #120]
    mrs x17, elr_el3
    str x17, [x16, #128]
    mrs x17, far_el3
    str x17, [x16, #136]
    mov x17, #5
    b publish
write_checkpoint:
    ldr x16, =0x42002000
    str x1, [x16, #96]          // Register number at the untouched callback entry.
    str x2, [x16, #104]         // Proposed value; the write never executes.
    mov x17, #6
    b publish
failed_checkpoint:
    ldr x16, =0x42002000
    mov x17, #7
publish:
    str x17, [x16, #24]
    mrs x17, CurrentEL
    str x17, [x16, #48]
    cmp x17, #12
    b.ne park                  // A wrong EL cannot use the EL3 return.
    mrs x17, mpidr_el1
    str x17, [x16, #56]
    mrs x17, sctlr_el3
    str x17, [x16, #64]
    mrs x17, sctlr_el1
    str x17, [x16, #72]
    mrs x17, cntfrq_el0
    str x17, [x16, #80]
    isb
    mrs x17, cntpct_el0
    str x17, [x16, #88]
    mov x17, sp
    str x17, [x16, #144]
    mov x17, #1
    str x17, [x16, #8]
    mov x17, #256
    str x17, [x16, #16]
    ldr x17, =0x4137335354415254
    str x17, [x16]
    dsb sy
    ldr x0, =0x08001004
    mov w1, #0x20
    str w1, [x0]
    str wzr, [x0, #4]
    dsb sy
    mov x0, #2
    msr rmr_el3, x0
    isb
park:
    wfi
    b park
.ltorg
.section .note.GNU-stack,"",%progbits
