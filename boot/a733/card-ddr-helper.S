.syntax unified
.arch armv8-a
.arm
.section .entry,"ax",%progbits
.global card_helper
card_helper:
    b card_select
    b restore_write
    b card_finish

.text
.global card_read
.type card_read,%function
card_read:
    ldr ip,=0x47879
    bx ip
.global card_stop
.type card_stop,%function
card_stop:
    ldr ip,=0x479f9
    bx ip
