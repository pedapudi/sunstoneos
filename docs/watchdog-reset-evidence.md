# Reset observations do not yet identify a retained watchdog cause

The inspected A733 sources do not establish a retained register that identifies
watchdog expiry after ROM reconnects. The prepared [expiry diagnostic](watchdog-expiry.md)
therefore keeps its existing acceptance limits. A successful run can establish
protected reset and a new live ROM session under the declared conditions;
conclusive reset-source attribution and recovery from a stalled peripheral
transaction remain unqualified.

The physical ROM does maintain a software progress record at `0x07090304`.
Its reset entry updates that record before CPU redirection. This provides a
candidate for supplementary observations of ROM execution, but it is not a
hardware reset-cause latch. Do not write this word as a diagnostic marker or
interpret its bits as a watchdog result.

## The reference watchdog registers do not supply the missing cause

The inspected bootloader revision is
`b791be842935b27268ae3d00e943a9075495f30a`; its source identity is pinned in
[sources.lock.json](../sources.lock.json). The relevant header is
`arch/arm/include/asm/arch-sunxi/watchdog.h`, under `CONFIG_SUNXI_WDT_V2`.

| Address | Source-defined function | Consequence for reset evidence |
| --- | --- | --- |
| `0x02050000` | Interrupt enable | An enabled interrupt does not identify a completed reset. |
| `0x02050004` | Interrupt status | The user manual defines bit 0 as interval pending, cleared by writing one. It does not specify retention across reset. Do not treat it as retained reset cause. |
| `0x02050008` | Software reset (`srst`) | This is a reset command in the reference implementation. It must not be mistaken for a reset-status register. |
| `0x0205000c` | Feed/control | The reviewed diagnostic feeds once after enabling. It does not feed inside the expiry wait. |
| `0x02050010`, `0x02050014` | Reset configuration and timer mode | Readback establishes programming before the wait; post-reset values alone do not identify what caused reset. |
| `0x02050018` | Reset-output configuration | The bootloader selects `2` before its software-reset command. Reset scope and retention still require physical evidence. |

The A733 board reset implementation in
`arch/arm/mach-sunxi/board_sun60iw2.c` writes the keyed software-reset register.
Its reset is therefore not reference evidence that the countdown expired.
The native expiry diagnostic instead enables the timer through the mode register
and enters an interrupt-masked branch without a software-reset write.

The pinned board-kernel `sunxi_wdt.c` selects the `wdt-v103` register layout.
Its probe and restart paths do not read a reset-source latch or report a retained
watchdog boot status. The device tree supplies the main watchdog address and
interrupt, but no clock or retention contract. This inspection does not prove
that the silicon lacks a cause latch; it establishes that these sources do not
define one.

A733 datasheet version 0.93, section 3.9.10, lists watchdogs in both CPUX and
CPUS domains, programmable intervals, interrupts, and reset generation.
It does not specify which watchdog clocks and reset paths survive a stalled
interconnect. The presence of the second watchdog is not sufficient evidence
of that independence.

The pinned A733 User Manual V1.00 adds register semantics in section 8.3.6
(printed pages 378–381). Configuration bit 8 selects RTC 32 kHz or DCXO/750;
mode bits 7:4 select the interval. The output register sets reset pulse duration.
These definitions corroborate the prepared countdown mechanism, but do not
establish retention of its interrupt status or operation during a stalled bus.

Section 3.1.4 (printed pages 14–17) states that a low FEL input selects the
mandatory upgrade path before boot-media selection. This agrees with the
captured ROM branch and supports the pending BOOT/GND procedure. It does not
replace physical confirmation of that input. Section 9.1.7.16 describes the
DCXO control register's system-reset status as current assertion/deassertion,
without a retained cause definition.

The manual is pinned as `a733-user-manual-1.00` in
[sources.lock.json](../sources.lock.json). Its cached PDF and layout-preserving
text are under `~/.cache/sunstoneos/sources/camera-research/`.
This documentation review introduces no new register access or test rerun.

## ROM writes identify a progress record

The authoritative ROM input is the captured 69,632-byte board image with SHA-256
`a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa`.
The [ROM capture record](watchdog.md#captured-board-rom-and-reset-ownership)
describes its acquisition and the difference from the published revision.

At ROM address `0x7c`, the reset path loads `0x07090304`. It inspects bit 15 and
selects one of two writes before continuing at `0x114`:

- If bit 15 was clear, ROM preserves bits 16–30, clears bit 31, and writes
  `0x8001` into the low half.
- If bit 15 was set, ROM preserves bits 0–14, clears bit 15, and writes
  `0x8001` into the high half.

The stores occur at `0xa8` or `0xc4`. CPU0 redirect dispatch occurs later, at
`0x164` through `0x178`. Entering forced FEL directly at `0x20` bypasses this
reset-path update before reaching the common initialization at `0x17c`.

The ROM helper at `0xd14` also writes `0x07090304`. It selects the active half
using bit 15 and updates progress/error fields from its arguments. For example,
arguments `7, 2` produce `0x8702` in the selected half. These are software writes,
so observing a changed word cannot independently identify a watchdog event.

The focused binary check executes the reset prefix and this helper. Five cases
passed: both initial selector states, direct forced-FEL entry, and both active
halves in the helper. No instructions are skipped. The check stops before FEL
initialization, uses synthetic MMIO, and does not model reset retention.
The capture also contains transfers to addresses beyond its end, including
`0x11000` and `0x17000`; it does not establish every later ROM-side write.

```sh
"$HOME/.cache/sunstoneos/venv/bin/python" checks/watchdog-rom-reset-log.py \
  "$HOME/.cache/sunstoneos/evidence/rom-copy-20261016-084055/physical-rom.bin"
```

The check and source hashes are retained under
`~/.cache/sunstoneos/evidence/watchdog-reset-evidence-20261016/`.
The existing expiry, ROM-dispatch, and hardware configuration checks were not
repeated. No device access or physical measurement occurred.

## Keep the next physical decisions separate

The queued enable/disarm and expiry proposals retain their current prerequisites
and acceptance conditions. They require explicit confirmation of the stable
BOOT/GND connection and the same-instance physical input inspection. Neither
a software check nor an elapsed interval replaces that confirmation.

A future coordinator-reviewed diagnostic may record `0x07090304` before and
after reset as supplementary data. First confirm readable access and the effect
of subsequent FEL initialization; the current evidence does not justify an
automatic pass rule based on the selected half. This research adds no register
access to the existing harness and does not change either pending proposal.

Before adding a reset-cause criterion, obtain an A733-specific definition of the
cause register, bit meanings, clearing semantics, reset retention, and ROM's
handling of it. An interrupt-status name or a similar SoC's register map is
insufficient. A result marker written before the wait would prove only that
execution reached its writer, even if the marker survived reset.

The bounded four-second trial already tests reset while CPU execution remains
in an interrupt-masked branch. It does not stall the interconnect. Qualifying
recovery from a bus stall additionally requires an identified fault mechanism,
a watchdog clock/reset path that remains active during that fault, and observed
ROM return while the fault is present.

Do not create an unspecified hang by reading an arbitrary unclocked peripheral.
Do not restore the earlier erroneous UFS access order merely to provoke a fault.
After protected reset is qualified, the corrected UFS probe can proceed under
the watchdog. A successful UFS probe establishes that access path; it does not
establish recovery from a stalled transaction. A documented interconnect timeout
or controlled fault-injection mechanism remains the prerequisite for that
separate recovery claim.
