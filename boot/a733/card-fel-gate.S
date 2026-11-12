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
    ldr r4,=A733_CARD_FEL_RTC_MARKER
    ldr r6,=0x07090104
    mov r8,#0                 /* Final value of GP2: zero clears the request. */
    cmp r1,r2
    beq requested
    /* An earlier gate record keeps its cold-path flag for this power epoch. */
    lsr r2,r1,#16
    movw r3,#A733_CARD_FEL_STAGE
    cmp r2,r3
    andeq r8,r1,#A733_CARD_FEL_RECORD_COLD
    ldr r2,[r4]
    uxth r2,r2
    cmp r2,#0
    beq cold
warm:
    /* The marker is set or unknown: every warm or unknown reset selects FEL.
     * Progress goes to the record word, so LAST keeps the image's breadcrumb. */
    orr r8,r8,#A733_CARD_FEL_RECORD_WARM
    movt r8,#A733_CARD_FEL_STAGE
    mov r6,r0
requested:
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
    cmp r8,#0
    andne r2,r1,#0xff
    orrne r8,r8,r2
    /* A disarm is safe in every WDT0 state; after a watchdog reset the
     * configuration and output words are unmeasured. */
    cmp r3,#0
    beq mode_checked
    cmp r1,#0
    cmpne r1,#1
    cmpne r1,#0x41
    cmpne r1,#0xb1
    bne watchdog_refused
    ldr r2,[r5,#-4]
    uxth r2,r2
    cmp r2,#1
    bne watchdog_refused
    ldr r2,[r5,#4]
    uxth r2,r2
    cmp r2,#2
    bne watchdog_refused
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
    bne watchdog_refused
    progress A733_CARD_FEL_WATCHDOG_SET
    mov r7,#0
clear:
    /* Clear the request, or store the warm record, with readback. */
    str r8,[r0]
    dsb sy
    isb
    ldr r2,[r0]
    cmp r2,r8
    bne clear_failed
    orrs r2,r7,r8             /* Only an unrefused request records more progress. */
    bne enter_rom
    progress A733_CARD_FEL_REQUEST_CLEARED
    b enter_rom
watchdog_refused:
    /* WDT0 is left as found; FEL keeps the board reachable over USB. */
    movw r2,#0x80
    orr r2,r2,r7
    movt r2,#A733_CARD_FEL_STAGE
    str r2,[r6]
    dsb sy
    cmp r8,#0
    orrne r8,r8,r7,lsl #8
    b clear
clear_failed:
    progress 0x84
    mov r7,#4
enter_rom:
    mov r0,#0
    mcr p15,0,r0,c7,c5,6
    dsb sy
    isb
    mov r0,#0x20
    orrs r2,r7,r8
    bne branch_rom
    progress A733_CARD_FEL_BEFORE_ROM
branch_rom:
    bx r0
refuse:
    /* Translation or caches on, or a ROM redirect armed: ROM 0x20 is unsafe. */
    movw r2,#0x80
    orr r2,r2,r7
    movt r2,#A733_CARD_FEL_STAGE
    str r2,[r6]
    dsb sy
parked:
    wfi
    b parked
cold:
    /* The RTC supply was lost: mark this power epoch, then continue to boot0. */
    ldr r2,=A733_CARD_FEL_MARKER_KEYED
    str r2,[r4]
    dsb sy
    ldr r2,[r4]
    uxth r2,r2
    movw r3,#A733_CARD_FEL_MARKER_SET
    cmp r2,r3
    orrne r8,r8,#(A733_CARD_FEL_RECORD_COLD|A733_CARD_FEL_RECORD_UNMARKED)
    bne warm
    ldr r5,=0x02050014
    ldr r1,[r5]
    and r1,r1,#0xff
    orr r8,r1,#A733_CARD_FEL_RECORD_COLD
    movt r8,#A733_CARD_FEL_STAGE
    str r8,[r0]
    /* Arm WDT0 in the runner's measured order: output, system reset, mode,
     * restart (docs/watchdog.md; A733 manual 8.3.6.4-8.3.6.7). */
    ldr r2,=0x16aa0002
    str r2,[r5,#4]
    ldr r2,=0x16aa0001
    str r2,[r5,#-4]
    ldr r2,=0x16aa0000|A733_CARD_FEL_COLD_WATCHDOG
    str r2,[r5]
    ldr r2,=0x14af
    str r2,[r5,#-8]
    dsb sy
    /* boot0's reset stub sets its own mode, SCTLR and stack (0x47b00). */
    ldr r0,=0x47b00
    bx r0
    .ltorg
.balign 4
a733_card_fel_gate_end:            /* The policy word is appended here. */
.size a733_card_fel_gate,.-a733_card_fel_gate
