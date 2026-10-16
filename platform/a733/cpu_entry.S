#include "a733_cpu_entry.h"
.section .text.a733_cpu_entry,"ax"
.balign 2048
.global a733_cpu_entry_image_start
.global a733_cpu_secondary_entry
.type a733_cpu_secondary_entry,%function
a733_cpu_entry_image_start:
a733_cpu_secondary_entry:
    mrs x0, CurrentEL
    cmp x0, #12
    b.ne .Linvalid_el
    msr daifset, #15
    // No memory or stack access before architecture and topology admission.
    mrs x22, sctlr_el3
    mov x0, #0x1005
    movk x0, #0x0200, lsl #16
    tst x22, x0
    b.ne .Lpark
    msr tpidr_el3, xzr
    adr x0, a733_cpu_entry_vectors
    msr vbar_el3, x0
    isb
    mrs x23, mpidr_el1
    mov x0, #0xc1000000
    orr x0, x0, #0x700
    bic x1, x23, x0
    cbnz x1, .Lpark
    ubfx x20, x23, #8, #3
    cbz x20, .Lpark
    adrp x21, a733_cpu_mailboxes
    add x21, x21, :lo12:a733_cpu_mailboxes
    add x21, x21, x20, lsl #8
    add x24, x21, #128
    ldr x0, [x21, #24]
    cmp x0, #A733_ENTRY_ARMED
    b.ne .Lpark
    dsb sy
    ldr x25, [x21, #8]
    cbz x25, .Lpark
    bl .Lvalidate_command
    cbnz x0, .Lpark
    // Reject every stale report byte before publishing a new arrival.
    mov x1, #0
.Lzero_report:
    ldr x0, [x24, x1]
    cbnz x0, .Lpark
    add x1, x1, #8
    cmp x1, #128
    b.ne .Lzero_report
    mrs x26, midr_el1
    str x25, [x24]
    str x20, [x24, #8]
    str x26, [x24, #16]
    str x23, [x24, #24]
    str x22, [x24, #32]
    dsb sy
    mov x0, #1
    msr tpidr_el3, x0
    str x0, [x24, #40]
    dsb sy
    sev
    sevl
.global a733_cpu_entry_wait
a733_cpu_entry_wait:
    wfe
    bl .Lvalidate_command
    cbnz x0, .Lprotocol_fault
    ldr x0, [x21, #24]
    cmp x0, #A733_ENTRY_ARMED
    b.eq a733_cpu_entry_wait
    cmp x0, #A733_ENTRY_RELEASE
    b.ne .Lprotocol_fault
    dsb sy
    // No power/control words are touched. Primary cleanup has authorized this.
    adrp x0, a733_cpu_entry_stacks
    add x0, x0, :lo12:a733_cpu_entry_stacks
    add x1, x20, #1
    add x0, x0, x1, lsl #12
    msr spsel, #1
    mov sp, x0
    mov x0, #A733_ENTRY_RELEASED
    str x0, [x24, #40]
    dsb sy
    sev
    mov x0, x20
    mov x1, x25
    mov x2, x24
    mov x3, x21
    adr x30, .Lreturned
    b a733_cpu_secondary_continue
.Lreturned:
    mov x27, #3
    b .Lexception_common
.Lprotocol_fault:
    mov x0, #1
.Lreport_fault:
    str x0, [x24, #80]
    dsb sy
    mov x0, #A733_ENTRY_FAULT
    str x0, [x24, #40]
    dsb sy
    sev
    msr tpidr_el3, xzr
.Lpark:
    wfe
    b .Lpark
.Linvalid_el:
    b .Linvalid_el
// x20 core, x21 command, x25 immutable observed epoch. Only x0..x2 clobbered.
.Lvalidate_command:
    ldr x0, [x21]
    mov x1, #0x5045
    movk x1, #0x5343, lsl #16
    cmp x0, x1
    b.ne .Lbad_command
    ldr x0, [x21, #8]
    cmp x0, x25
    b.ne .Lbad_command
    ldr x0, [x21, #16]
    cmp x0, x20
    b.ne .Lbad_command
    ldr x0, [x21, #32]
    mvn x1, x25
    cmp x0, x1
    b.ne .Lbad_command
    mov x2, #40
.Lreserved_command:
    ldr x0, [x21, x2]
    cbnz x0, .Lbad_command
    add x2, x2, #8
    cmp x2, #128
    b.ne .Lreserved_command
    mov x0, #0
    ret
.Lbad_command:
    mov x0, #1
    ret
.size a733_cpu_secondary_entry, .-a733_cpu_secondary_entry

.balign 2048
.global a733_cpu_entry_vectors
a733_cpu_entry_vectors:
.rept 16
    b a733_cpu_entry_exception
    .space 124
.endr
.global a733_cpu_entry_exception
a733_cpu_entry_exception:
    mov x27, #2
.Lexception_common:
    // No stack or interrupted general register is trusted. Disable reporting
    // first, so a fault during diagnostic stores parks without recursion.
    mrs x0, tpidr_el3
    msr tpidr_el3, xzr
    cmp x0, #1
    b.ne .Lpark
    mrs x0, sctlr_el3
    mov x1, #0x1005
    movk x1, #0x0200, lsl #16
    tst x0, x1
    b.ne .Lpark
    mrs x0, mpidr_el1
    mov x1, #0xc1000000
    orr x1, x1, #0x700
    bic x1, x0, x1
    cbnz x1, .Lpark
    ubfx x20, x0, #8, #3
    cbz x20, .Lpark
    adrp x21, a733_cpu_mailboxes
    add x21, x21, :lo12:a733_cpu_mailboxes
    add x21, x21, x20, lsl #8
    add x24, x21, #128
    ldr x25, [x24]
    cbz x25, .Lpark
    bl .Lvalidate_command
    cbnz x0, .Lpark
    ldr x0, [x24, #8]
    cmp x0, x20
    b.ne .Lpark
    mrs x0, esr_el3
    str x0, [x24, #48]
    mrs x0, far_el3
    str x0, [x24, #56]
    mrs x0, elr_el3
    str x0, [x24, #64]
    mrs x0, spsr_el3
    str x0, [x24, #72]
    mov x0, x27
    b .Lreport_fault
.global a733_cpu_entry_image_end
a733_cpu_entry_image_end:

.section .bss.a733_cpu_mailboxes,"aw",%nobits
.balign 128
.global a733_cpu_mailboxes
a733_cpu_mailboxes:
    .space 8*A733_ENTRY_SLOT_BYTES
.section .bss.a733_cpu_entry_stacks,"aw",%nobits
.balign 4096
.global a733_cpu_entry_stacks
a733_cpu_entry_stacks:
    .space 8*A733_ENTRY_STACK_BYTES
.section .note.GNU-stack,"",%progbits
