#include "gate-info.h"
#include "refusal.h"
.section .text.entry,"ax"
.global _start
.type _start,%function
_start:
    msr daifset,#15
    mov x5,#0
    mov w11,#CARD_FEL_REFUSE_EL
    mrs x0,CurrentEL
    cmp x0,#12
    b.ne refuse
    mov w11,#CARD_FEL_REFUSE_MPIDR
    mrs x0,mpidr_el1
    tst x0,#0xffff
    b.ne refuse
    mrs x0,sctlr_el3
    mov w11,#CARD_FEL_REFUSE_TRANSLATION
    tbnz x0,#0,refuse
    mov w11,#CARD_FEL_REFUSE_DATA_CACHE
    tbnz x0,#2,refuse
    mov w11,#CARD_FEL_REFUSE_INSTRUCTION_CACHE
    tst x0,#0x1000
    b.ne refuse
    ldr x8,=a733_card_fel_request
    ldr x19,[x8,#16]          /* AAPCS64 preserves the nonce across C admission. */
    ldr x6,=0x07090100
    and w0,w19,#0xffff
    movk w0,#0x5c0d,lsl #16
    str w0,[x6]
    mov w11,#CARD_FEL_REFUSE_REQUEST_MAGIC
    mov x5,x8
    ldr x0,[x5]
    ldr x1,=0x3151524644524143
    cmp x0,x1
    b.ne refuse
    mov w11,#CARD_FEL_REFUSE_REQUEST_VERSION
    add x5,x8,#8
    ldr x0,[x5]
    cmp x0,#1
    b.ne refuse
    mov w11,#CARD_FEL_REFUSE_NONCE
    add x5,x8,#16
    mov x0,x19
    cbz x0,refuse
    mov w11,#CARD_FEL_REFUSE_REQUEST_WATCHDOG
    add x5,x8,#24
    ldr x0,[x5]
    cmp x0,#0x41
    b.ne refuse
    mov w11,#CARD_FEL_REFUSE_WATCHDOG_CONFIG
    ldr x5,=0x02050010
    ldr w0,[x5]
    uxth w1,w0
    cmp w1,#1
    b.ne refuse
    mov w11,#CARD_FEL_REFUSE_WATCHDOG_MODE
    add x5,x5,#4
    ldr w0,[x5]
    uxth w1,w0
    cmp w1,#0x41
    b.ne refuse
    mov w11,#CARD_FEL_REFUSE_WATCHDOG_OUTPUT
    add x5,x5,#4
    ldr w0,[x5]
    uxth w1,w0
    cmp w1,#2
    b.ne refuse
    mov w11,#CARD_FEL_REFUSE_RTC_REQUEST
    ldr x5,=0x07090108
    ldr w0,[x5]
    ldr w1,=0x5aa5a55a
    cmp w0,w1
    b.ne refuse
    mov w11,#CARD_FEL_REFUSE_REDIRECT_C0
    ldr x5,=0x070501c0
    ldr w0,[x5]
    cbnz w0,refuse
    mov w11,#CARD_FEL_REFUSE_REDIRECT_C4
    add x5,x5,#4
    ldr w0,[x5]
    cbnz w0,refuse
    mov w11,#CARD_FEL_REFUSE_REDIRECT_D4
    add x5,x5,#0x10
    ldr w0,[x5]
    cbnz w0,refuse
    ldr x0,=a733_card_fel_stack_top
    mov sp,x0
    bl card_fel_check_secondaries
    ldr x5,=a733_card_fel_report
    ldr x0,=0x3152454644524143
    mov x1,#1
    stp x0,x1,[x5]
    mov x0,#64
    stp x0,x19,[x5,#16]
    ldr x0,=0x5aa5a55a
    stp x1,x0,[x5,#32]
    mov x0,#0x41
    mov x1,#12
    stp x0,x1,[x5,#48]
    ldr x6,=0x07090100
    and w0,w19,#0xffff
    movk w0,#0x5c0d,lsl #16
    str w0,[x6]
    movz w0,#0x0c01,lsl #16
    str w0,[x6,#4]
    ldr x5,=0x41001000+CARD_FEL_TEST_OFFSET
    mov w1,#1
    str w1,[x5]
    dsb sy
    ldr w0,[x5]
    mov w11,#CARD_FEL_REFUSE_GATE_PUBLICATION
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
    mov w11,#CARD_FEL_REFUSE_RESET_REQUEST
    mov x5,#0
    mrs x0,rmr_el3
refuse:
    mov x2,x0                /* One retained raw sample; no diagnostic reread. */
    mov w0,w11
    mov x1,x5
.global card_fel_refuse
.type card_fel_refuse,%function
card_fel_refuse:              /* AAPCS64: code, source address (0 for sysreg), sample. */
    ldr x6,=0x07090100
    ldr w3,[x6,#0x10]
    cbnz w3,refuse_publish    /* A retained fault owns ESR/ELR/FAR. */
    str w2,[x6,#0x14]
    lsr x3,x2,#32
    str w3,[x6,#0x18]
    orr w1,w1,#0x80000000     /* Refusal sample format; all sources are below 2 GiB. */
    str w1,[x6,#0x1c]
refuse_publish:
    ldr x4,=a733_card_fel_request
    ldr w3,[x4,#16]
    and w3,w3,#0xffff
    movk w3,#0x5c0d,lsl #16
    str w3,[x6]
    mrs x3,cntpct_el0
    lsr x3,x3,#10
    and w3,w3,#0x1fffffff
    mrs x4,mpidr_el1
    ubfx w4,w4,#8,#3
    orr w3,w3,w4,lsl #29
    str w3,[x6,#0x0c]
    dsb sy
    movk w0,#0x0c01,lsl #16
    str w0,[x6,#4]            /* Publish LAST only after the sampled payload. */
    dsb sy
entry_refuse:
    wfi
    b entry_refuse
.size card_fel_refuse,.-card_fel_refuse
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
