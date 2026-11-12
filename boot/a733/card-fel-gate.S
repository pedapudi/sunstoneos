/* Stackless, position-independent gate before the pinned boot0 reset stub.
 * Vendor sequence and limits: docs/designs/card-fel-recovery.md. */
.syntax unified
.arch armv8-a
.arm
.section .text,"ax"
.global a733_card_fel_gate
.global a733_card_fel_gate_state
.global a733_card_fel_gate_test
.global a733_card_fel_gate_end
.type a733_card_fel_gate,%function
a733_card_fel_gate:
    adr r4,a733_card_fel_gate
    mov r7,#0
    cpsid ifa
    ldr r0,=0x07090108
    ldr r1,[r0]
    ldr r2,=0x5aa5a55a
    cmp r1,r2
    beq requested
    ldr r1,[r4,#(a733_card_fel_gate_test-a733_card_fel_gate)]
    cmp r1,#0
    bne refuse
    /* Cold media entry: boot0 supplies its own stack and initialization. */
    ldr r0,=0x47b00
    bx r0
requested:
    mov r7,#1                 /* Local translation/cache entry. */
    mrc p15,0,r1,c1,c0,0
    tst r1,#5
    bne refuse
    /* ROM media entry enables I-cache. Match boot0's local I cleanup. */
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
    ldr r5,=0x02050014
    ldr r1,[r5]
    uxth r1,r1
    cmp r1,#0
    beq consume
    cmp r1,#1                  /* The recorded watchdog-reset remnant. */
    cmpne r1,#0x41             /* Four-second host/card watchdog. */
    cmpne r1,#0xb1             /* Sixteen-second returning watchdog. */
    bne refuse
    ldr r2,[r5,#-4]
    uxth r2,r2
    cmp r2,#1
    bne refuse
    ldr r2,[r5,#4]
    uxth r2,r2
    cmp r2,#2
    bne refuse
consume:
    mov r7,#6                 /* Verified disarm while request is pending. */
    /* Keep the FEL request pending until the watchdog is known disabled. */
    ldr r2,=0x16aa0000
    str r2,[r5]
    dsb sy
    ldr r2,[r5]
    uxth r2,r2
    cmp r2,#0
    bne refuse
    mov r1,#0
    str r1,[r0]
    dsb sy
    isb
    ldr r2,[r0]
    cmp r2,#0
    /* With WDT0 off, a failed clear returns to FEL with no success state. */
    bne observation
    ldr r2,=0x46454c31
    str r2,[r4,#(a733_card_fel_gate_state-a733_card_fel_gate)]
observation:
    ldr r2,[r4,#(a733_card_fel_gate_test-a733_card_fel_gate)]
    cmp r2,#0
    beq enter_rom
    /* Only the RAM fixture replaces its progress; a card preserves crumbs. */
    ldr r2,=0x05030000
    str r2,[r0,#-4]
    mrrc p15,0,r2,r3,c14        /* CNTPCT: the same crumb time encoding. */
    lsr r2,r2,#10
    orr r2,r2,r3,lsl #22
    bic r2,r2,#0xe0000000
    str r2,[r0,#4]             /* TIME at RTC+0x10c; preserve FAR at+0x11c. */
enter_rom:
    mov r0,#0
    mcr p15,0,r0,c7,c5,6
    dsb sy
    isb
    mov r0,#0x20
    bx r0
refuse:
    ldr r2,[r4,#(a733_card_fel_gate_test-a733_card_fel_gate)]
    cmp r2,#0
    beq parked
    ldr r2,=0x0c010080
    orr r2,r2,r7
    ldr r3,=0x07090104
    str r2,[r3]
    dsb sy
parked:
    wfi
    b parked
    .ltorg
.balign 4
a733_card_fel_gate_state: .word 0
a733_card_fel_gate_test: .word 0
a733_card_fel_gate_end:
.size a733_card_fel_gate,.-a733_card_fel_gate
