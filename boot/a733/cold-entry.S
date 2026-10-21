/* Boot0 monitor-only entry. This first stage captures the inherited state;
 * application admission and remote-writer exclusion are not established yet.
 * A733_COLD_STAGE2 selects the stage-2 slot loader build (boot/a733/stage2). */
#include "a733_cpu_entry.h"
.arch armv8-a
.section .text.cold_header,"ax"
.global a733_cold_image
.type a733_cold_image,%function
a733_cold_image:
    b a733_cold_entry
    .ascii "monitor\0"
    .space 0x2c-(.-a733_cold_image)
    .word 0x47000000             // Boot0 reads this, then replaces it with MiB.
    .space 0x108-(.-a733_cold_image)
.balign 16
.global a733_cold_entry
.type a733_cold_entry,%function
a733_cold_entry:
    // No stack or data access before privilege, topology and cache admission.
    mrs x19, CurrentEL
    cmp x19, #12
    b.ne a733_cold_park
    msr daifset, #15
    mrs x20, mpidr_el1
    mov x0, #0xc1000000
    bic x0, x20, x0
    cbnz x0, a733_cold_park
    mrs x21, sctlr_el3
    mov x0, #(A733_ENTRY_SCTLR_REJECT & 0xffff)
    movk x0, #(A733_ENTRY_SCTLR_REJECT >> 16), lsl #16
    tst x21, x0
    b.ne a733_cold_park
    mrs x22, midr_el1
    ldr x0, =0xff0ffff0
    and x0, x22, x0
    ldr x1, =0x410fd050           // Arm Cortex-A55; revision bits are recorded.
    cmp x0, x1
    b.ne a733_cold_park
    adrp x0, a733_cold_vectors
    add x0, x0, :lo12:a733_cold_vectors
    msr vbar_el3, x0
    isb
    adrp x23, a733_cold_report
    add x23, x23, :lo12:a733_cold_report
    mov x0, #0
1:  str xzr, [x23, x0]
    add x0, x0, #8
    cmp x0, #512
    b.lo 1b
    mov x0, #1
    str x0, [x23, #8]
    mov x0, #512
    str x0, [x23, #16]
    stp x19, x22, [x23, #40]
    stp x20, x21, [x23, #56]
    mrs x0, scr_el3
    mrs x1, cntfrq_el0
    stp x0, x1, [x23, #72]
    isb
    mrs x0, cntpct_el0
    mrs x1, rvbar_el3
    stp x0, x1, [x23, #88]
    mov x0, #264
    str x0, [x23, #104]
    adr x0, a733_cold_image
    add x1, x23, #192
    mov x2, #33
2:  ldr x3, [x0], #8
    str x3, [x1], #8
    subs x2, x2, #1
    b.ne 2b
    // These fields are loader metadata, not memory or hardware authority.
    adr x0, a733_cold_image
    ldr w1, [x0, #4]
    ldr w2, =0x696e6f6d
    cmp w1, w2
    b.ne .Lmetadata
    ldr w1, [x0, #8]
    ldr w2, =0x00726f74
    cmp w1, w2
    b.ne .Lmetadata
    ldr w1, [x0, #0x10]
    ldr w2, [x0, #0x18]
    orr w1, w1, w2
    cbnz w1, .Lmetadata
    ldr w1, [x0, #0x2c]
    mov w2, #0x47000000
    cmp w1, w2                 // Refuse the unmodified file loadword.
    b.eq .Lmetadata
    cmp w1, #128                // Contains this reservation below 0x48000000.
    b.lo .Lmetadata
    mov x0, #1                 // Entry captured; application is not admitted.
    b .Lpublish
.Lmetadata:
    mov x0, #2
.Lpublish:
    str x0, [x23, #24]
    ldr x0, =0x41533733434f4c44
    dsb sy
    str x0, [x23]
    dsb sy
#ifdef A733_COLD_STAGE2
    // Only an admitted capture continues to the stage-2 loader. Refused
    // metadata and exceptions, including stage-2 faults, park as before.
    ldr x0, [x23, #24]
    cmp x0, #1
    b.ne a733_cold_park
    ldr x0, =a733_stage2_stack_top
    mov sp, x0
    ldr x0, =a733_stage2_bss_start
    ldr x1, =a733_stage2_bss_end
3:  cmp x0, x1
    b.hs 4f
    str xzr, [x0], #8
    b 3b
4:  bl a733_stage2_main
#endif
.global a733_cold_park
a733_cold_park:
    wfe
    b a733_cold_park
.ltorg

.balign 2048
.global a733_cold_vectors
a733_cold_vectors:
.macro cold_vector number
    mov x18, #\number
    b a733_cold_exception
    .space 120
.endm
.set cold_slot, 0
.rept 16
    cold_vector cold_slot
    .set cold_slot, cold_slot+1
.endr
.global a733_cold_exception
a733_cold_exception:
    msr daifset, #15
    adrp x23, a733_cold_report
    add x23, x23, :lo12:a733_cold_report
    str xzr, [x23]
    mov x0, #1
    str x0, [x23, #8]
    mov x0, #512
    str x0, [x23, #16]
    mrs x0, esr_el3
    mrs x1, far_el3
    stp x0, x1, [x23, #128]
    mrs x0, elr_el3
    mrs x1, spsr_el3
    stp x0, x1, [x23, #144]
    str x18, [x23, #160]
    mov x0, #3
    b .Lpublish

.section .bss.cold_report,"aw",%nobits
.balign 64
.global a733_cold_report
.type a733_cold_report,%object
a733_cold_report:
    .space 512
.size a733_cold_report, .-a733_cold_report
.section .note.GNU-stack,"",%progbits
