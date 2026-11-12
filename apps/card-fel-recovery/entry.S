#include "gate-info.h"
.section .text.entry,"ax"
.global _start
.type _start,%function
_start:
    msr daifset,#15
    mrs x0,CurrentEL
    cmp x0,#12
    b.ne entry_refuse
    mrs x0,mpidr_el1
    tst x0,#0xffff
    b.ne entry_refuse
    mrs x0,sctlr_el3
    tbnz x0,#0,entry_refuse
    tbnz x0,#2,entry_refuse
    tst x0,#0x1000
    b.ne entry_refuse
    ldr x8,=a733_card_fel_request
    ldr x10,[x8,#16]
    ldr x6,=0x07090100
    and w0,w10,#0xffff
    movk w0,#0x5c0d,lsl #16
    str w0,[x6]
    mov w11,#2                /* Request framing. */
    ldr x0,[x8]
    ldr x1,=0x3151524644524143
    cmp x0,x1
    b.ne refuse
    ldr x0,[x8,#8]
    cmp x0,#1
    b.ne refuse
    cbz x10,refuse
    ldr x0,[x8,#24]
    cmp x0,#0x41
    b.ne refuse
    mov w11,#3                /* Host watchdog contract. */
    ldr x6,=0x02050010
    ldr w0,[x6]
    uxth w0,w0
    cmp w0,#1
    b.ne refuse
    ldr w0,[x6,#4]
    uxth w0,w0
    cmp w0,#0x41
    b.ne refuse
    ldr w0,[x6,#8]
    uxth w0,w0
    cmp w0,#2
    b.ne refuse
    mov w11,#4                /* Runner-owned retained request. */
    ldr x6,=0x07090108
    ldr w0,[x6]
    ldr w1,=0x5aa5a55a
    cmp w0,w1
    b.ne refuse
    mov w11,#5                /* ROM software redirects. */
    ldr x5,=0x070501c0
    ldr w0,[x5]
    ldr w1,[x5,#4]
    orr w0,w0,w1
    ldr w1,[x5,#0x14]
    orr w0,w0,w1
    cbnz w0,refuse
    /* Read-only FEL admission: OFF, matching policy/status, no hotplug. */
    mov w9,#1
secondary:
    add w11,w9,#0x100         /* Core status, then request, policy, status. */
    ldr x5,=0x07050200
    ldr w0,[x5,x9,lsl #2]
    and w0,w0,#3
    cmp w0,#2
    b.ne refuse
    add w11,w9,#0x200
    add x5,x5,#0x20
    ldr w0,[x5,x9,lsl #2]
    tbnz w0,#0,refuse
    add w11,w9,#0x300
    ldr x5,=0x07052000
    add x5,x5,x9,lsl #12
    ldr w0,[x5]
    cbz w0,policy_valid
    cmp w0,#0x100
    b.ne refuse
policy_valid:
    add w11,w9,#0x400
    ldr w1,[x5,#8]
    cmp w0,w1
    b.ne refuse
    add w9,w9,#1
    cmp w9,#8
    b.ne secondary
    ldr x5,=a733_card_fel_report
    ldr x0,=0x3152454644524143
    mov x1,#1
    stp x0,x1,[x5]
    mov x0,#64
    stp x0,x10,[x5,#16]
    ldr x0,=0x5aa5a55a
    stp x1,x0,[x5,#32]
    mov x0,#0x41
    mov x1,#12
    stp x0,x1,[x5,#48]
    ldr x6,=0x07090100
    and w0,w10,#0xffff
    movk w0,#0x5c0d,lsl #16
    str w0,[x6]
    movz w0,#0x0c01,lsl #16
    str w0,[x6,#4]
    ldr x5,=0x41001000+CARD_FEL_TEST_OFFSET
    mov w0,#1
    str w0,[x5]
    dsb sy
    ldr w1,[x5]
    mov w11,#7                /* Gate observation-word publication. */
    cmp w0,w1
    b.ne refuse
    ldr x5,=0x08001004
    ldr x0,=0x41001000
    str w0,[x5]
    str wzr,[x5,#4]
    dsb sy
    mov x0,#2
    msr rmr_el3,x0
    isb
refuse:
    ldr x6,=0x07090104
    movz w0,#0x0c01,lsl #16
    orr w0,w0,w11
    str w0,[x6]
    dsb sy
entry_refuse:
    wfi
    b entry_refuse
    .ltorg
.size _start,.-_start

.section .card_gate,"ax"
.global a733_card_fel_gate_image
a733_card_fel_gate_image:
.incbin CARD_FEL_GATE_BINARY
.size a733_card_fel_gate_image,.-a733_card_fel_gate_image

.section .data,"aw"
.balign 8
.global a733_card_fel_request
.type a733_card_fel_request,%object
a733_card_fel_request: .zero 32
.size a733_card_fel_request,32

.section .result,"aw",%nobits
.balign 8
.global a733_card_fel_report
.type a733_card_fel_report,%object
a733_card_fel_report: .space 64
.size a733_card_fel_report,64
