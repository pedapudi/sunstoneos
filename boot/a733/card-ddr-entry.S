/* Optional pinned boot0 hook. Each section fits after a zero slot-presence word. */
.syntax unified
.arch armv8-a
.thumb
.macro slot n
.section .slot\n,"ax",%progbits
.balign 4
.endm
slot 8
.global card_boot
.thumb_func
card_boot:
    push {r0,r1,r4-r11,lr}
    sub sp,sp,#4
    mov r4,sp
    ldr sp,=0x9f000
    ldr r5,=0x86000
    movs r0,#0
    movs r1,#32
1:  str r0,[r5],#4
    subs r1,#1
    bne 1b
    bl card_preflight
    cbz r0,card_fallback
    mrrc p15,0,r0,r1,c14
    ldr r2,=0x86040
    strd r0,r1,[r2]
    movs r0,#0
    bl card_load
    cbnz r0,2f
    movs r0,#1
    bl card_load
    cbz r0,card_fallback
2:  ldr r3,=0x92000
    blx r3
    cbz r0,card_fallback
    mov sp,r4
    ldr r0,[sp,#4]
    ldr r1,[sp,#8]
    bl vendor_init
    ldr sp,=0x9f000
    ldr r3,=0x92008
    blx r3
    mov sp,r4
    add sp,sp,#12
    pop {r4-r11,pc}
card_fallback:
    mov sp,r4
    add sp,sp,#4
    pop {r0,r1,r4-r11,lr}
    b.w vendor_init
.ltorg

slot 9
.thumb_func
card_load:
    push {r4-r7,lr}
    sub sp,sp,#4
    ldr r7,=card_config
    add.w r0,r7,r0,lsl #2
    ldr r4,[r0,#12]
    adds r4,#64
    ldr r5,=0x92000
    ldr r6,[r7,#20]
    addw r6,r6,#511
    lsrs r6,r6,#9
1:  mov r0,r4
    movs r1,#1
    mov r2,r5
    bl card_read
    adds r4,#1
    add.w r5,r5,#512
    subs r6,#1
    bne 1b
    ldr r0,=0x92000
    ldr r1,[r7,#20]
    bl card_crc
    ldr r1,[r7,#24]
    subs r0,r0,r1
    clz r0,r0
    lsrs r0,r0,#5
    add sp,sp,#4
    pop {r4-r7,pc}
.ltorg

slot 10
.global card_timer
.thumb_func
card_timer:
    ldr r0,=read_active
    ldr r0,[r0]
    cbz r0,1f
    push {r1,lr}
    bl card_deadline
    pop {r1,lr}
1:  mrrc p15,0,r2,r3,c14
    b.w vendor_timer_tail
.global card_deadline
.thumb_func
card_deadline:
    mrrc p15,0,r2,r3,c14
    ldr r0,=0x86040
    ldr r1,[r0]
    ldr r0,[r0,#4]
    subs r2,r2,r1
    sbcs r3,r3,r0
    cbnz r3,card_timeout
    ldr r0,=card_config
    ldr r0,[r0,#28]
    cmp r2,r0
    bhs card_timeout
    bx lr
card_timeout:
    movs r0,#12
    b.w card_stop
.ltorg
.global read_active
read_active: .word 0

slot 11
.global card_read
.thumb_func
card_read:
    push {r0-r7,lr}
    sub sp,sp,#4
    bl card_deadline
    ldr r4,=read_active
    movs r0,#1
    str r0,[r4]
    ldr r5,=0x60670
    ldrb r6,[r5]
    movs r0,#0
    strb r0,[r5]
    ldr r0,[sp,#4]
    ldr r1,[sp,#8]
    ldr r2,[sp,#12]
    bl vendor_read
    cbnz r0,card_read_error
    bl card_deadline
    movs r0,#0
    str r0,[r4]
    strb r6,[r5]
    add sp,sp,#4
    pop {r0-r7,pc}
card_read_error:
    movs r0,#13
    b.w card_stop
.ltorg

slot 12
.thumb_func
card_preflight:
    push {r4,lr}
    movs r4,#0
    ldr r0,=card_config
    ldr r0,[r0,#8]
    cbz r0,1f
    bl vendor_selector
    cbnz r0,1f
    ldr r0,=0x83c14
    ldr r0,[r0]
    cmp r0,#1
    bne 1f
    ldr r0,=0x83648
    ldr r0,[r0]
    ldr r1,=0x838f8
    cmp r0,r1
    bne 1f
    ldr r2,[r0,#0x20]
    ldr r3,=0x83858
    cmp r2,r3
    bne 1f
    ldr r2,[r3]
    ldr r1,=0x04020000
    cmp r2,r1
    bne 1f
    ldr r2,[r3,#0x14]
    add.w r1,r1,#512
    cmp r2,r1
    bne 1f
    ldr r2,[r0,#0xa8]
    ldr r3,=0x5b32d
    cmp r2,r3
    bne 1f
    ldr r2,[r0,#0x90]
    cmp r2,#512
    bne 1f
    ldr r2,[r0,#0xe8]
    cbz r2,1f
    bl card_capacity
    mov r4,r0
1:  mov r0,r4
    pop {r4,pc}
.ltorg

slot 13
.global card_timing
.thumb_func
card_timing:
    push {r0,r1,r4,lr}
    ldr r4,=0x86038
    ldr r4,[r4]
    cbz r4,1f
    ldr r3,=0x92004
    blx r3
    cmp r0,#0
    bne card_stop
1:  pop {r0,r1,r4,lr}
    b.w vendor_timing
.ltorg

.thumb_func
card_crc:
    mov.w r2,#-1
    ldr r12,=0xedb88320
1:  ldrb r3,[r0],#1
    eors r2,r3
    movs r3,#8
2:  lsrs r2,r2,#1
    it cs
    eorcs r2,r2,r12
    subs r3,#1
    bne 2b
    subs r1,#1
    bne 1b
    mvn r0,r2
    bx lr
.ltorg

slot 14
.global card_stop
.thumb_func
card_stop:
    ldr r1,=0x8601c
    str r0,[r1]
    cpsid if
    dsb sy
1:  wfe
    b 1b
.ltorg

slot 15
.thumb_func
card_capacity:
    ldr r2,[r0,#0xec]
    mrc p15,0,r0,c1,c0,0
    movw r1,#0x1005
    tst r0,r1
    bne 1f
    mrc p15,0,r0,c14,c0,0
    ldr r1,=24000000
    cmp r0,r1
    bne 1f
    ldr r0,=card_config
    ldr r1,[r0,#12]
    ldr r0,[r0,#16]
    cmp r0,r1
    it lo
    movlo r0,r1
    adds r0,#128
    bcs 1f
    cmp r2,r0
    ite hs
    movhs r0,#1
    movlo r0,#0
    bx lr
1:  movs r0,#0
    bx lr
.ltorg
.org 80
.global card_config
card_config:
    .word 0x44524343,1,EXPERIMENTAL,UNIT_A,UNIT_B,HELPER_BYTES,HELPER_CRC,240000000
