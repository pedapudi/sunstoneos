// WDT0 arming replay for the runner's `--rearm-expiry stage2` (docs/board-runner.md, "WDT0 re-arm expiry test").
//
// Two entry points, each run once with the FEL execute command, in A32 state with the MMU and caches off:
//   _start (+0x00)   the WDT0 writes of the card first stage's cold path (boot/a733/card-fel-gate.S, label cold):
//                    the mode read, then mode 0, output 2, configuration 1, mode 0xb1 and the restart key, with the
//                    gate's barriers and literal loads. The gate's GP2 record store and readback between the mode read
//                    and the disarm are left out, so no RTC word is written here.
//   stage2 (+0x100)  stage 2's arm() with the gate's arming consumed (boot/a733/stage2/loader.c, arm, and
//                    a733_stage2_watchdog_state in admission.c): the same register reads, compares, writes and
//                    early refusals, in the same order, each access between `dsb sy` barriers as rd() and wr() place
//                    them, the counter read with its isb, and the mark(0x0e00) store to LAST.
// The values read are stored in the result block at +0x400 (layout below) after each access; the runner reads the block
// back. Both entry points use only the ROM's stack (36 bytes of saved registers) and return to the caller.
//
// Result block, 32-bit words from +0x400:
//   +0x00 gate: mode read at entry          +0x04..+0x0c gate: mode, configuration, output read after the writes
//   +0x10 gate: GATE_DONE
//   +0x20 stage2: status (STATUS_*)         +0x24..+0x2c redirect flag, entry, alternate flag   +0x30 GP2
//   +0x34..+0x3c mode, configuration, output read by the watchdog-state check
//   +0x40 mode after the disarm             +0x44 output, +0x48 configuration read after their writes
//   +0x4c mode read with them               +0x50/+0x54 counter (low, high) at the arming
//   +0x58..+0x60 mode, configuration, output read after the arming        +0x64/+0x68 counter at the final check
    .syntax unified
    .arch armv8-a
    .arm
    .text
    .global _start
    .global a733_rearm_stage2

    .equ RESULT, 0x400
    .equ WDT_MODE, 0x02050014           // WDT_RESTART is -8, WDT_CONFIG -4 and WDT_OUTPUT +4 from it
    .equ RTC_GP2, 0x07090108
    .equ RTC_LAST, 0x07090104
    .equ REDIRECT_FLAG, 0x070501c0      // the entry is at +4 and the alternate flag at +0x14
    .equ GATE_DONE, 0x45544147          // 'GATE'
    .equ STATUS_ARMED, 1                // arm() returned 1
    .equ STATUS_STATE, 0x11             // a733_stage2_watchdog_state refused: no WDT0 write
    .equ STATUS_DISARM, 0x12            // the mode still read enabled after the disarm
    .equ STATUS_SETUP, 0x13             // output, configuration or mode differed after the disarm: WDT0 is left disarmed
    .equ STATUS_FINAL, 0x14             // the final readback or the 1 ms timing check failed: WDT0 holds 0x41
    .equ LAST_ARMED, 0x0c030e00         // mark(0x0e00): A733_MARK_STEP | 0x0e00 (boot/a733/stage2/marks.h)
    .equ ONE_MS, 24000                  // A733_SMP_COUNTER_HZ / 1000

// rd(): dsb sy, load, dsb sy, then the value into the result block.
    .macro rd reg, base, offset, slot
    dsb     sy
    ldr     \reg, [\base, #\offset]
    dsb     sy
    str     \reg, [r10, #\slot]
    .endm
// wr(): dsb sy, store, dsb sy.
    .macro wr value, base, offset
    ldr     r1, =\value
    dsb     sy
    str     r1, [\base, #\offset]
    dsb     sy
    .endm

_start:
    push    {r4-r11, lr}
    adr     r10, _start
    add     r10, r10, #RESULT
    ldr     r5, =WDT_MODE
    ldr     r1, [r5]
    str     r1, [r10, #0x00]
    ldr     r2, =0x16aa0000
    str     r2, [r5]
    dsb     sy
    ldr     r2, =0x16aa0002
    str     r2, [r5, #4]
    ldr     r2, =0x16aa0001
    str     r2, [r5, #-4]
    ldr     r2, =0x16aa00b1
    str     r2, [r5]
    ldr     r2, =0x14af
    str     r2, [r5, #-8]
    dsb     sy
    ldr     r1, [r5]
    str     r1, [r10, #0x04]
    ldr     r1, [r5, #-4]
    str     r1, [r10, #0x08]
    ldr     r1, [r5, #4]
    str     r1, [r10, #0x0c]
    ldr     r1, =GATE_DONE
    str     r1, [r10, #0x10]
    pop     {r4-r11, pc}
    .ltorg

    .org    0x100
a733_rearm_stage2:
    push    {r4-r11, lr}
    adr     r10, _start
    add     r10, r10, #RESULT
    mov     r11, #STATUS_STATE
    // a733_stage2_watchdog_state(1): redirects_clear(), GP2, then mode 0xb1, configuration 1 and output 2.
    ldr     r4, =REDIRECT_FLAG
    rd      r0, r4, 0x00, 0x24
    cmp     r0, #0
    bne     done
    rd      r0, r4, 0x04, 0x28
    cmp     r0, #0
    bne     done
    rd      r0, r4, 0x14, 0x2c
    cmp     r0, #0
    bne     done
    ldr     r4, =RTC_GP2
    rd      r0, r4, 0, 0x30
    cmp     r0, #0
    bne     done
    ldr     r5, =WDT_MODE
    rd      r0, r5, 0, 0x34
    uxth    r0, r0
    cmp     r0, #0xb1
    bne     done
    rd      r0, r5, -4, 0x38
    uxth    r0, r0
    cmp     r0, #1
    bne     done
    rd      r0, r5, 4, 0x3c
    uxth    r0, r0
    cmp     r0, #2
    bne     done
    // if (gated) { wr(mode, 0x16aa0000); if (rd(mode) & 1) return 0; }
    mov     r11, #STATUS_DISARM
    wr      0x16aa0000, r5, 0
    rd      r0, r5, 0, 0x40
    tst     r0, #1
    bne     done
    // wr(output, 0x16aa0002); wr(configuration, 0x16aa0001); then the three reads of the condition.
    mov     r11, #STATUS_SETUP
    wr      0x16aa0002, r5, 4
    wr      0x16aa0001, r5, -4
    rd      r0, r5, 4, 0x44
    uxth    r0, r0
    cmp     r0, #2
    bne     done
    rd      r0, r5, -4, 0x48
    uxth    r0, r0
    cmp     r0, #1
    bne     done
    rd      r0, r5, 0, 0x4c                 // (rd(mode) & 1) && !gated: read, then discarded for a gated arming
    // *armed = counter(); wr(mode, 0x16aa0041); wr(restart, 0x14af); mark(0x0e00).
    isb
    mrrc    p15, 0, r6, r7, c14
    str     r6, [r10, #0x50]
    str     r7, [r10, #0x54]
    wr      0x16aa0041, r5, 0
    wr      0x14af, r5, -8
    ldr     r4, =RTC_LAST
    wr      LAST_ARMED, r4, 0
    // return mode == 0x41 && configuration == 1 && output == 2 && counter() - armed < 1 ms.
    mov     r11, #STATUS_FINAL
    rd      r0, r5, 0, 0x58
    uxth    r0, r0
    cmp     r0, #0x41
    bne     done
    rd      r0, r5, -4, 0x5c
    uxth    r0, r0
    cmp     r0, #1
    bne     done
    rd      r0, r5, 4, 0x60
    uxth    r0, r0
    cmp     r0, #2
    bne     done
    isb
    mrrc    p15, 0, r0, r1, c14
    str     r0, [r10, #0x64]
    str     r1, [r10, #0x68]
    subs    r0, r0, r6
    sbc     r1, r1, r7
    cmp     r1, #0
    bne     done
    ldr     r2, =ONE_MS
    cmp     r0, r2
    bhs     done
    mov     r11, #STATUS_ARMED
done:
    str     r11, [r10, #0x20]
    pop     {r4-r11, pc}
    .ltorg
