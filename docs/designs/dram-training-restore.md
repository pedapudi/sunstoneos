# Design: restoring DRAM training from a record on the microSD card

Status: **design, reviewed once; restore not implemented.** The independent
review of 2026-10-25 found two blocking and six major issues, and this text
includes the changes it proposed; it needs a second review before any board
run beyond the read-only captures. Implemented and host-checked: the capture
tooling and its classifier, the record format (`tools/training-record.py`)
and the card region in the boot-slot allocation and `tools/storage-update.py`
(`checks/training-record.py`). The restore hook follows that review and the
first capture analysis.

The user approved replacing boot0's 5.1 s DRAM training with a restore of
saved training results kept on the microSD card. The
[DRAM fast-boot design](dram-fast-boot.md) derives boot0's training flow and
measures the cost. This design specifies how the training results are
identified, the restore sequence, its fallbacks, the record and its card
region, periodic retraining and the qualification plan. P-state 0 stays at
2040 MHz throughout. Addresses written `0x5xxxx` are load addresses in the
pinned boot0 `829ea8e3…`.

## The restore set comes from three kinds of capture

The read-only capture ([runbook](../board-runbooks/dram-training-capture.md))
reads the parameter block, the SID chip ID and thermal calibration, the
controller, the PHY over boot0's own address extents, the firmware message
block and, last, PHY registers `0x0000`–`0xbfff`, which boot0 never
accesses but where the published PHY keeps address/command delays.
`tools/analyse-ddr-captures.py` classifies each register as status,
lifetime, board or constant. Three kinds of capture feed the restore set:

1. **Four-P-state captures** after boot0's normal training, two per
   lifetime, several lifetimes per board, both boards. They give chip and
   lifetime variation and the status class. They are not the restore
   reference, because the restore runs boot0's P-state-0-only path, which
   differs: message byte 4 is `0x30` (`0x59de6`), `0x4bca8` branches on
   `dram_tpr13` bit 2 (`0x4bcb2`), and the block at `0x55a98` and the call
   `0x581b4` (`0x57a08`) do not run.
2. **P-state-0-only captures**, taken after a training with `dram_tpr13`
   bit 2 clear (run 3 of the fast-boot plan, here qualification step 0).
   These are the reference values.
3. **Device-initialization-only captures**, taken after a run with the
   training-step value `0x0001`; this is also the first restore-prototype
   test. Every PHY register that differs between kinds 2 and 3 was set by
   training, including values that repeat in every training, which the
   classifier alone would call constant. The restore set is those
   registers, minus the status class, each with the bit mask of the bits
   that differ.

The P-state-0-only and initialization-only kinds use DDR wrapper variants
(`tools/prepare-ddr.py --variant pstate0` or `init-only`), frozen with the
captures and gated on the second review.

**The first lifetime.** Two four-P-state captures of one port 3-3 lifetime
(`board-session-20261025-21`) agree on 84,095 registers and differ on 274
status registers: 207 in the APB-only block, 64 byte-lane registers at
offsets `0xe0`, `0xe2`, `0xe8` and `0x1c4` of the eight lanes of byte blocks
0 and 1 only, two master registers (`0x0a940176`, `0x0a940178`) and one
DRTUB register (`0x0aa80206`). The status lane registers appear only in
blocks 0 and 1, which suggests that one channel's tracking runs between the
captures. All 49,152 registers of `0x0000`–`0xbfff` read zero, so the
address/command delays are not where the published PHY keeps them;
the restore set must come from the blocks boot0 accesses or from the
kind 2 and kind 3 diff. `UcclkHclkEnables` read 0 after training.
`analyse-ddr-captures.py` output:
`~/.cache/sunstoneos/evidence/dram-training-analysis-20261025/port-3-3-lifetime-1-blocks.json`.

The capture must also record the LPDDR5 mode registers. boot0's stored
parameter set has `dram_tpr10` bit 29 set, so every boot runs the save
routine `0x576e4`: parameter words 32–49 hold the receive Vref values and
words 50 and 51 MR14 and MR15 of each channel and rank, read with MR16 set
to 5 (`0x57796`). Every capture therefore already carries them (confirmed in
the first port 3-3 capture). MR12 is not saved; a capture variant of the
DDR wrapper would read it through boot0's mode-register read `0x4b5ac`
after `init_DRAM`, in the same MR16 state. That variant runs boot0 code, so
it waits for the second review.

## The hook wraps init_DRAM and writes the PHY registers inside it

boot0's P-state-0-only LPDDR5 path runs `init_DRAM` (`0x49bdc`): DRAM
supply (`0x4b618`), clock and reset setup (`0x57a4c`), controller setup
(`0x578a4`, `0x57238`, `0x570d4`), then `0x55990`: PHY setup (`0x55654`,
`0x4f57c`), P-state 0 clocks (`0x4f58c`, `0x4f500`), message block
(`0x59d00`, which reads the training-step value at `0x59d6a`), firmware load
(`0x4bbb8`, `0x4b97c`), firmware run (`0x4f47c`), per-P-state timing
(`0x4bca8`, called at `0x56142`) and the final PHY stage (`0x4f46c`), then
post-training settings (`0x57310`, `0x4b7ec`, `0x561a0`). A firmware result
other than 7 returns 0 from `0x55990` (`0x5612e`) and `main` then stops
without a reset (`0x5e21e`), so every check and fallback must live around
the `init_DRAM` call. The restore uses two patch sites:

- **Around `init_DRAM`** (the call at `0x5e216` on a card boot; the DDR
  wrapper on the FEL path). Before the call: compute the strap CRC over
  parameter words 0–31 as boot0 selected them, run every record check
  below, and only then edit the parameter block: clear `dram_tpr13` bit 2,
  set `dram_tpr10` bits 28:16 to `0x0001`, set `dram_tpr1` bit 31 and words
  50–51 to the recorded MR14 and MR15 values (boot0's own path, `0x5a08e`).
  After the call: require a nonzero size equal to the record's, write the
  remaining recorded mode registers with `0x4b554` in recorded order, read
  the restore set back, and run quick-check part A. On any failure, restore
  the saved parameter words and call `0x49bdc` again for a full training in
  the same boot.
- **The call at `0x56142`**: clear `MicroContMuxSel`, write the restore set
  under its masks in record order (byte lanes, master, address/command, the
  per-P-state block, then the `0x0000`–`0xbfff` range), set
  `MicroContMuxSel` to 1 and call `0x4bca8` as boot0 would.

Whether the firmware accepts the device-initialization-only value, and
whether a second `init_DRAM` call after a failed restore trains correctly,
are the first two prototype tests on the FEL path.

**Quick check.** Part A, in the AArch32 hook below 4 GiB: a short burst
test per channel, rank and byte lane with walking-ones and pseudo-random
patterns over 1 MiB per rank, written and read with cached `LDM`/`STM` bursts
after the cache setup that boot0's parser uses. Part B, in the stage-2
monitor at EL3: the same pattern over the 12,288 MiB map, one 64 KiB window
per 256 MiB, excluding the monitor's own image. Part B failing sets a retrain
flag in an RTC general-purpose register other than indexes 1 and 2, which
boot0 uses (`0x4a940`, `0x4a95c`), and requests a warm reset. The check
catches gross restore errors only. Marginal timing shows only in the
qualification runs, and the safety of the restore rests on them.

## Every mismatch falls back to full training

| Check, before any parameter edit | Mismatch |
| --- | --- |
| Record magic, version, header CRC and payload CRC | Full training |
| Pinned boot0 checksum, a constant in the hook, equal to the record's | Full training |
| Chip ID, SID `0x03006200`–`0x0300620c` | Full training |
| Strap: selected set index and CRC of parameter words 0–31 | Full training |
| Temperature: THS DDR sensor (channel 1), calibrated as in [thermal.md](../thermal.md), within the record's limit of the capture | Full training |
| Retrain flag set by part B | Full training, flag cleared |

After the call, a firmware result other than 7, a size mismatch, a restore
read-back mismatch or a part A failure falls back in place, as above. On
the FEL path a warm reset would boot from the card instead of returning to
FEL, so the FEL prototype reports part B failures to the host instead. The
temperature check needs THS clock, reset and enable writes that boot0 never
makes. The FEL prototype takes the temperature from the host's thermal
reading; the card variant adds the THS read once the drift data set the
limit.

## The record format

Little-endian, in one contiguous card extent.

| Offset | Field |
| --- | --- |
| 0 | Magic `AS73DTRN` |
| 8, 12 | Version 1; header size 128 |
| 16, 20 | Payload length; payload CRC-32 |
| 24 | Generation, incremented by every record write |
| 32 | Chip ID, four words |
| 48 | Pinned boot0 checksum (eGON word `0x0c` of `829ea8e3…`) |
| 52, 56 | Selected parameter-set index; CRC-32 of parameter words 0–31 |
| 60, 64 | Capture temperature in m°C; allowed difference in m°C |
| 68, 72 | Training-step value of the reference training; DRAM size in MiB |
| 76..99 | Section offsets and counts: register runs, masks, mode-register writes |
| 100..123 | Reserved, zero |
| 124 | Header CRC-32 over bytes 0–123 |

Registers are stored as runs: base address, count, then 16-bit values,
padded to 4 bytes, with a separate list of `(address, u16 mask, u16 zero)`
for registers restored under a mask. Mode-register writes are `(channel,
rank, register, value)` in write order, including MR16. At about 2 bytes per
register, 64 KiB holds over 30,000 registers. On the card the header fills
the unit's first sector and the payload starts at the second.

## The record has its own 4 MiB unit on the card

The coordinator proposed the `bootpara` region at sectors 24,496–24,499 or
beside it. That unit, 16,384–24,575, holds the slot B header, which
[boot-slots.md](../boot-slots.md#card-layout) keeps in a 4 MiB unit apart
from other writes, and the vendor's SD parameter sector 24,504 and tuning
pattern 24,512–24,571. The record therefore gets one 4 MiB unit at or
beyond sector 40,960, assigned by the boot-slot allocation record like the
slot regions (`boot-slots.py allocate --training-record`).
`tools/storage-update.py` takes the `training-record` owned region from that
allocation. Its plan input is a plan from `tools/training-record.py plan` or
`invalidate`, every write lies inside the region, a host backup of
the region precedes the write session under [docs/goal.md](../goal.md)
section 4, and the payload is written before the header, so an interrupted
write leaves a header whose CRC fails and boot0 trains fully. Slot fallback
is unaffected, because the record shares no unit with a slot header. Each
card's backup must show the unit unallocated by partitions and zero or
holding an earlier record. No card write happens before the qualification
below passes.

## Card boots need a host-built boot0 variant

boot0 trains during a card boot before any SunstoneOS code runs, so a card
restore needs boot0 changed. A host tool builds a variant from the pinned
image: the hook in the empty parameter slots 8–15 (`0x476f4`–`0x47af3`,
1,024 bytes; the strap always selects slot 5), the calls at `0x5e216` and
`0x56142` redirected to it, and the eGON checksum recomputed. The hook
reads the record with one multi-sector call to boot0's card read
`0x5db08`. The pinned image is not committed, as today. This changes the
[microSD package](../microsd-boot-package.md)'s property that boot0 is
unmodified and needs the coordinator's decision. The boards boot the
unsigned `eGON.BT0` format, so the ROM checks only the additive checksum
(inferred; secure-boot fuses are unconfirmed, as in
[boot-rom-and-boot0.md](../vendor-firmware/boot-rom-and-boot0.md)). If the
1,024 bytes do not hold both patch sites, the hook loads its body from the
record's extent into SRAM. The FEL path needs no variant: the DDR wrapper
patches boot0 in RAM, as `tools/prepare-ddr.py` already patches its UART
output, and the host supplies the record.

## Periodic retraining every N boots

A boot counter must survive power removal without a card write on every
boot. The OS keeps the count in its data partition, which section 4 permits
once the card has a host backup and the image confines its writes there.
When the count reaches N, the OS clears the record header through the
backup-first path, and the next boot trains fully; a capture of the restore
set after that training produces the new record. Until a native
backup-first writer is qualified, the coordinator does both steps from the
host. N comes from the drift data: the number of boots, or the time and
temperature span, over which restored values stay within the spread of
fresh trainings.

## Qualification before any card write

0. **P-state 0 only**, both boards: full training of P-state 0 alone, its
   timing, the March C- run of the
   [memory qualification image](../memory-qualification.md), and the
   reference captures.
1. **Captures** of all three kinds on both boards, several lifetimes each
   and two temperatures, analysed; the restore set, masks and mode-register
   list frozen and reviewed.
2. **FEL restore prototype**, port 3-3 then 3-1: the device-initialization
   run, then restored boots, each followed by the full March C- run and a
   stress run, hot (after sustained CPU and memory load) and cold (after a
   long power-off at room temperature or below). Any bit error stops the
   work.
3. **Fallback cases** on the FEL path: a wrong CRC, another board's chip ID,
   a changed strap CRC, a temperature limit of zero, a forced read-back
   mismatch and a forced part A failure each end in a full training in the
   same boot.
4. **Boot time**: ten restored and ten fully trained boots per board,
   comparing boot0's `DRAM BOOT DRIVE INFO` to `Actual DRAM SIZE` interval.
5. **Card**: host backup, record write through the new region, then the
   card-boot variant repeats steps 2–4.

## Budgets

| Quantity | Budget | Basis |
| --- | --- | --- |
| DRAM interval with restore | 500 ms; prediction 200–500 ms | *Estimated*: the 151 ms P-state 1 run includes PHY setup and firmware load |
| Record read | 10 ms for 64 KiB in one multi-sector read | *Estimated* from boot0's card transfer mode |
| CRC over the record | Measured in the FEL prototype | Depends on boot0's clock and cache state |
| Temperature read | 5 ms | Unmeasured; card variant only |
| Quick check part A and B | 20 ms | *Estimated* |
| Hook code in boot0 | 1,024 bytes, or a loader stub plus a body in the record | Empty parameter slots |
| Fallback cost | One full training in the same boot, about 5.1 s | Measured training time |

A restored boot counts only with a passing quick check, and the restore
path counts only after qualification step 2 passes on both boards.

## Open questions for the second review

- Whether the firmware accepts the device-initialization-only value, and
  whether a second `init_DRAM` call after a failed restore trains correctly.
- Whether the restore set needs the controller registers as well, which the
  kind 2 and kind 3 diff also shows.
- The RTC general-purpose register for part B's flag, and whether it
  survives the warm reset.
- The coordinator's decision on a modified boot0 for card boots, and on the
  record's own 4 MiB unit in place of the `bootpara` sectors.
