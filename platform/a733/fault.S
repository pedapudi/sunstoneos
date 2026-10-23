/* EL3 capture of one core's fault record (docs/fault-capture.md). The SMP
 * cached EL3 fault handler calls this leaf after its state and core checks,
 * for an SMC from EL1 only, with EL3 translation on. It uses no stack, lock
 * or MMIO and writes only the core's 128-byte slot, which EL3 maps as Secure
 * Device memory: every slot access is an aligned 64-bit store.
 *
 * In:  x0 core (0..7), x1 status, x2 detail, x3 EL1 marker (0x100 | vector
 *      slot for a vector fault, 0 for a deliberate exit), x4 contract epoch.
 * Out: nothing. Clobbers x0..x17 and the flags; preserves x18..x30 apart
 *      from x30, which bl sets. */
#include "a733_fault.h"
.arch armv8-a+crc
.section .text.a733_fault_capture,"ax"
.balign 4
.global a733_fault_capture
.type a733_fault_capture,%function
a733_fault_capture:
    mov x5, #(A733_FAULT_SLOTS_ADDRESS & 0xffff)
    movk x5, #(A733_FAULT_SLOTS_ADDRESS >> 16), lsl #16
    add x5, x5, x0, lsl #7
    // The open marker identifies a capture that a second fault interrupted.
    mov x6, #(A733_FAULT_SLOT_OPEN & 0xffff)
    movk x6, #((A733_FAULT_SLOT_OPEN >> 16) & 0xffff), lsl #16
    movk x6, #((A733_FAULT_SLOT_OPEN >> 32) & 0xffff), lsl #32
    movk x6, #((A733_FAULT_SLOT_OPEN >> 48) & 0xffff), lsl #48
    str x6, [x5]
    mov w7, #-1
    // Word 2: kind, vector slot and core. Only marker bit 8 selects a fault.
    and x8, x3, #0xff
    mov x6, #A733_FAULT_KIND_VECTOR
    orr x6, x6, x8, lsl #8
    mov x9, #A733_FAULT_KIND_EXIT
    tst x3, #0x100
    csel x6, x6, x9, ne
    orr x6, x6, x0, lsl #16
    str x6, [x5, #16]
    crc32cx w7, w7, x6
    str x1, [x5, #24]
    crc32cx w7, w7, x1
    str x2, [x5, #32]
    crc32cx w7, w7, x2
    isb
    mrs x6, cntpct_el0
    str x6, [x5, #40]
    crc32cx w7, w7, x6
    str x4, [x5, #48]
    crc32cx w7, w7, x4
    mov x8, xzr
    mov x9, xzr
    mov x10, xzr
    mov x11, xzr
    mov x12, xzr
    mov x13, xzr
    mov x14, xzr
    tbz x3, #8, 2f
    // The SMC left the EL1 syndrome, return address, fault address and
    // saved state of the faulting context unchanged.
    mrs x8, esr_el1
    mrs x9, elr_el1
    mrs x10, far_el1
    mrs x11, spsr_el1
    // EL3 reads EL1 .bss and the task through its Nonsecure write-back alias
    // of image RAM, which is coherent with EL1.
    adrp x15, pxCurrentTCBs
    add x15, x15, :lo12:pxCurrentTCBs
    ldr x12, [x15, x0, lsl #3]
    cbz x12, 2f
    tst x12, #7
    b.ne 2f
    adrp x15, a733_fault_task_name_offset
    ldr x15, [x15, :lo12:a733_fault_task_name_offset]
    add x15, x12, x15
    // a733_extent_within(0x41200000, 0xe00000, name, 16): EL1 image RAM.
    mov x16, #0x41200000
    subs x16, x15, x16
    b.lo 2f
    mov x17, #0xfff0
    movk x17, #0xdf, lsl #16
    cmp x16, x17
    b.hi 2f
    ldp x13, x14, [x15]
2:  str x8, [x5, #56]
    crc32cx w7, w7, x8
    str x9, [x5, #64]
    crc32cx w7, w7, x9
    str x10, [x5, #72]
    crc32cx w7, w7, x10
    str x11, [x5, #80]
    crc32cx w7, w7, x11
    str x12, [x5, #88]
    crc32cx w7, w7, x12
    str x13, [x5, #96]
    crc32cx w7, w7, x13
    str x14, [x5, #104]
    crc32cx w7, w7, x14
    str xzr, [x5, #112]
    crc32cx w7, w7, xzr
    str xzr, [x5, #120]
    crc32cx w7, w7, xzr
    mvn w7, w7
    str x7, [x5, #8]
    // The fields and CRC become visible before the magic number.
    dmb oshst
    mov x6, #(A733_FAULT_SLOT_MAGIC & 0xffff)
    movk x6, #((A733_FAULT_SLOT_MAGIC >> 16) & 0xffff), lsl #16
    movk x6, #((A733_FAULT_SLOT_MAGIC >> 32) & 0xffff), lsl #32
    movk x6, #((A733_FAULT_SLOT_MAGIC >> 48) & 0xffff), lsl #48
    str x6, [x5]
    // Complete before the caller parks in WFE.
    dsb sy
    ret
.size a733_fault_capture,.-a733_fault_capture
.section .note.GNU-stack,"",%progbits
