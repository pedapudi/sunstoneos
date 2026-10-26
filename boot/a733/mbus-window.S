// The gated MBUS window of include/a733_el3_mbus.h, run from SRAM.
//
// Every SunstoneOS image executes from DRAM, and during this window MBUS is
// gated and the MSI_LITE0 bridge held in reset. The routine is therefore
// copied to SRAM (A733_MBUS_SRAM) and runs there: it is position-independent,
// keeps everything in registers and uses no stack, so no instruction fetch or
// data access reaches DRAM until it returns with MBUS running again.
// checks/el3-mbus.py assembles this file and requires the bytes to equal
// a733_mbus_window_code[] in the header, then executes those bytes against a
// register model.
//
// The sequence is the boot ROM's MBUS routine (a8e31821 @0x2084) with
// boot0's source 1 (PERI0_600M, 829ea8e3 @0x57b1e-0x57b28):
//   1. gate off with the update bit; wait for the update bit to clear;
//   2. MSI_LITE0 MBUS reset asserted (CCU +0x594 bit 17); 10 us;
//   3. divider 1 with the update bit; 10 us; wait;
//   4. source 1 with the update bit; 10 us; wait;
//   5. MSI_LITE0 MBUS reset released (the caller's word);
//   6. gate on with the update bit; wait.
// Each wait reads MBUS_CLK until bit 27 reads clear, for at most 240,000
// counter ticks (10 ms at 24 MHz, the ROM's bound @0x2158); a read that
// completes before the deadline passes. On a timeout the routine releases
// the bridge, sets the gate with the update bit and returns the step.
//
// In: w0 = MSI_LITE0_BGR as read, with its gate and both resets released.
// Out: w0 = 0, or the step (1, 3, 4 or 6) whose wait timed out.
// Clobbers x1-x6, x9-x15 and the flags; x30 is preserved through x15.
    .text
    .balign 4
    .global a733_mbus_window
a733_mbus_window:
    mov     x15, x30
    mov     w12, w0
    movz    x9, #0x2588
    movk    x9, #0x0200, lsl #16        // MBUS_CLK_REG
    movz    x10, #0x2594
    movk    x10, #0x0200, lsl #16       // MSI_LITE0_BGR_REG
    // 1. Gate off with the update bit.
    mov     w13, #1
    ldr     w1, [x9]
    bic     w1, w1, #0x80000000
    orr     w1, w1, #0x08000000
    str     w1, [x9]
    dsb     sy
    bl      .Lwait
    cbnz    w2, .Lfail
    // 2. MSI_LITE0's MBUS reset asserted, then 10 us.
    mov     w13, #2
    bic     w1, w12, #0x20000
    str     w1, [x10]
    dsb     sy
    bl      .Lsettle
    // 3. Divider 1 with the update bit, 10 us, wait.
    mov     w13, #3
    ldr     w1, [x9]
    bic     w1, w1, #0x1f
    orr     w1, w1, #0x08000000
    str     w1, [x9]
    dsb     sy
    bl      .Lsettle
    bl      .Lwait
    cbnz    w2, .Lfail
    // 4. Source 1 (PERI0_600M) with the update bit, 10 us, wait.
    mov     w13, #4
    ldr     w1, [x9]
    bic     w1, w1, #0x07000000
    orr     w1, w1, #0x01000000
    orr     w1, w1, #0x08000000
    str     w1, [x9]
    dsb     sy
    bl      .Lsettle
    bl      .Lwait
    cbnz    w2, .Lfail
    // 5. MSI_LITE0's MBUS reset released.
    mov     w13, #5
    str     w12, [x10]
    dsb     sy
    // 6. Gate on with the update bit, wait.
    mov     w13, #6
    ldr     w1, [x9]
    orr     w1, w1, #0x80000000
    orr     w1, w1, #0x08000000
    str     w1, [x9]
    dsb     sy
    bl      .Lwait
    cbnz    w2, .Lfail
    mov     w0, #0
    mov     x30, x15
    ret
.Lfail:
    str     w12, [x10]
    dsb     sy
    ldr     w1, [x9]
    orr     w1, w1, #0x80000000
    orr     w1, w1, #0x08000000
    str     w1, [x9]
    dsb     sy
    mov     w0, w13
    mov     x30, x15
    ret
// w2 = 0 once MBUS_CLK bit 27 reads clear, 1 after 240,000 ticks.
.Lwait:
    isb
    mrs     x3, cntpct_el0
1:  ldr     w5, [x9]
    isb
    mrs     x4, cntpct_el0
    tbz     w5, #27, 2f
    sub     x4, x4, x3
    movz    x6, #0xa980
    movk    x6, #0x3, lsl #16           // 240,000
    cmp     x4, x6
    b.lo    1b
    mov     w2, #1
    ret
2:  mov     w2, #0
    ret
// At least 240 ticks, 10 us at 24 MHz.
.Lsettle:
    isb
    mrs     x3, cntpct_el0
1:  isb
    mrs     x4, cntpct_el0
    sub     x4, x4, x3
    cmp     x4, #240
    b.lo    1b
    ret
    .global a733_mbus_window_end
a733_mbus_window_end:
