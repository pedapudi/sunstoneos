/* Stackless, position-independent first-stage gate, also entered by FEL exe.
 * Vendor sequence and limits: docs/designs/card-fel-recovery.md. */
#include "a733_card_fel_gate.h"
.syntax unified
.arch armv8-a
.arm
.section .text,"ax"
.global a733_card_fel_gate
.global a733_card_fel_gate_end
.type a733_card_fel_gate,%function
.macro progress value
    movw r2,#\value
    movt r2,#A733_CARD_FEL_STAGE
    str r2,[r6]
    dsb sy
.endm
a733_card_fel_gate:
    cpsid ifa
    ldr r0,=0x07090108
    ldr r1,[r0]
    ldr r2,=0x5aa5a55a
    cmp r1,r2
    beq requested
    /* Cold media entry keeps every RTC record; boot0 supplies its own stack. */
    ldr r0,=0x47b00
    bx r0
requested:
    ldr r6,=0x07090104
    progress A733_CARD_FEL_ENTRY
    mov r7,#1                 /* Local translation/cache entry. */
    mrc p15,0,r1,c1,c0,0
    tst r1,#5
    bne refuse
    /* Both card ROM and FEL can enter with I-cache enabled. */
    bic r1,r1,#0x1000
    mcr p15,0,r1,c1,c0,0
    isb
    mov r1,#0
    mcr p15,0,r1,c7,c5,0
    dsb sy
    isb
    mov r7,#2                 /* ROM redirect state. */
    ldr r3,=0x070501c0
    ldr r1,[r3]
    ldr r2,[r3,#4]
    orrs r1,r1,r2
    bne refuse
    ldr r1,[r3,#0x14]
    cmp r1,#0
    bne refuse
    mov r7,#3                 /* Watchdog mode, configuration and output. */
    /* The WDT0 policy word follows the code: zero disarms (card first stage);
     * a nonzero mode re-arms WDT0 and restarts its interval (FEL proof). */
    ldr r3,a733_card_fel_gate_end
    uxth r3,r3
    ldr r5,=0x02050014
    ldr r1,[r5]
    uxth r1,r1
    cmp r1,#0
    cmpne r1,#1
    cmpne r1,#0x41
    cmpne r1,#0xb1
    bne refuse
    orrs r2,r1,r3             /* Nothing armed and nothing to arm. */
    beq mode_checked
    ldr r2,[r5,#-4]
    uxth r2,r2
    cmp r2,#1
    bne refuse
    ldr r2,[r5,#4]
    uxth r2,r2
    cmp r2,#2
    bne refuse
mode_checked:
    progress A733_CARD_FEL_MODE_CHECKED
    mov r7,#6                 /* Apply the policy while the request remains pending. */
    ldr r2,=0x16aa0000
    orr r2,r2,r3
    str r2,[r5]
    ldr r2,=0x14af            /* WDT_CTRL restart key (A733 manual 8.3.6.4). */
    cmp r3,#0
    strne r2,[r5,#-8]
    dsb sy
    ldr r2,[r5]
    uxth r2,r2
    cmp r2,r3
    bne refuse
    progress A733_CARD_FEL_WATCHDOG_SET
    mov r1,#0
    str r1,[r0]
    dsb sy
    isb
    ldr r2,[r0]
    cmp r2,#0
    bne clear_failed
    progress A733_CARD_FEL_REQUEST_CLEARED
    mov r7,#0
    b enter_rom
clear_failed:
    /* With WDT0 disabled, return to FEL with a retained failure. */
    progress 0x84
    mov r7,#4
enter_rom:
    mov r0,#0
    mcr p15,0,r0,c7,c5,6
    dsb sy
    isb
    mov r0,#0x20
    cmp r7,#0
    bne branch_rom
    progress A733_CARD_FEL_BEFORE_ROM
branch_rom:
    bx r0
refuse:
    movw r2,#0x80
    orr r2,r2,r7
    movt r2,#A733_CARD_FEL_STAGE
    str r2,[r6]
    dsb sy
parked:
    wfi
    b parked
    .ltorg
.balign 4
a733_card_fel_gate_end:            /* The policy word is appended here. */
.size a733_card_fel_gate,.-a733_card_fel_gate
