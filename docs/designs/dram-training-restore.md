# Design: restoring DRAM training from a record on the microSD card

Status: **design; not implemented; independent review required before any
board run beyond the read-only captures.** The capture tooling, its
classifier and their checks exist; the restore hook, the record writer and
the storage-region change follow the review and the first capture analysis.

The user approved replacing boot0's 5.1 s DRAM training with a restore of
saved training results, kept on the microSD card. The
[DRAM fast-boot design](dram-fast-boot.md) derives boot0's training flow and
measures the cost; this design specifies the captures that identify the
training results, the restore sequence, its fallbacks, the record format,
its place on the card, periodic retraining and the qualification plan.
P-state 0 stays at 2040 MHz throughout. Addresses written `0x5xxxx` are
load addresses in the pinned boot0 `829ea8e3…`.

## Captures identify which registers training sets

[The capture runbook](../board-runbooks/dram-training-capture.md) runs the
read-only capture on both boards, twice in each cold-start lifetime, in
several lifetimes per board and later at different temperatures. Each
capture reads the parameter block, the chip ID and thermal calibration, the
controller and the PHY over boot0's own address extents (35,161 registers).
`tools/analyse-ddr-captures.py` classifies every register:

- **status**: two captures in one lifetime differ;
- **lifetime**: stable in a lifetime, different between lifetimes of a board;
- **board**: stable on a board, different between boards;
- **constant**: equal in every capture.

The restore set is the PHY registers of the lifetime and board classes. A
constant register is either fixed configuration, which boot0's own code
rewrites on every boot, or a training result that happened to repeat; the
captures at other temperatures separate the second case before the restore
set is frozen. Status registers are never restored. A lifetime register
whose spread across lifetimes exceeds a few codes also bounds how far a
restored value may sit from a fresh training, which the drift study
quantifies.

## The restore replaces only the training run, in boot0's order

boot0's LPDDR5 path, with `dram_tpr13` bit 2 clear (P-state 0 only), runs:

1. `init_DRAM` `0x49bdc`: DRAM supply (`0x4b618`), then `0x57b7c`.
2. `0x57a4c`: DRAM clock and reset setup; `0x578a4`: controller setup and
   `0x57238`, `0x570d4` per channel.
3. `0x55990`: PHY setup (`0x55654`, `0x4f57c`); P-state 0 clocks
   (`0x4f58c`, `0x4f500`); message block (`0x59d00`); firmware load
   (`0x4bbb8`, `0x4b97c`); firmware run (`0x4f47c`); per-P-state timing
   (`0x4bca8`, called at `0x56142`); final PHY stage (`0x4f46c`).
4. Post-training reads and settings (`0x57310`, `0x4b7ec`, `0x561a0`), size
   and boot0's simple test.

The restore changes three things and leaves the rest to boot0:

- **Firmware steps.** `dram_tpr10` bits 28:16 become `0x0001`, so the
  firmware performs only device initialization: DRAM reset and the
  mode-register writes that the message block describes. Whether the
  firmware accepts this value is the first prototype test; message 7 is
  required.
- **LPDDR5 mode-register state.** The record carries the message-block
  mode-register fields of the capture (stage 5 of the capture, at
  `0x0a9b0000`); the hook writes them into the message block after
  `0x59d00` and before the run, so device initialization programs the
  trained values. MR14 and MR15 also travel through boot0's own path:
  parameter words 50 and 51 with `dram_tpr1` bit 31 (`0x5a08e`). Any trained
  mode register that the message block does not carry is written after the
  run with boot0's mode-register write routine `0x4b554`, in the order the
  record lists.
- **Trained PHY registers.** The hook replaces the call at `0x56142`: it
  clears `MicroContMuxSel`, writes the restore set in record order (byte
  lanes, then master, address/command and the per-P-state block, the order
  boot0 programs them), sets `MicroContMuxSel` to 1 and then calls `0x4bca8`
  as boot0 would.

After `init_DRAM` returns, the **quick check** writes and reads back a
64-byte pattern and its inverse at 64 sampled addresses: one per address
bit from bit 6 to bit 33 (both values of each bit), plus the first and last
line of each GiB. In the AArch32 boot0 context only addresses below 4 GiB
are reachable, so the check splits: part A in the boot0 hook covers the
first 3 GiB, and part B in the stage-2 monitor at EL3 covers the full
12,288 MiB before any slot is loaded. Budget: 20 ms for both parts.

## Every mismatch falls back to full training

The hook checks, before writing anything:

| Check | Source of the current value | Mismatch |
| --- | --- | --- |
| Record header and payload CRC-32 | The record | Full training |
| Record version and boot0 header checksum (word `0x0c`) | boot0's header at `0x47000` | Full training |
| Chip ID | SID `0x03006200`–`0x0300620c` | Full training |
| DRAM-select strap | Selected set index and CRC-32 of parameter words 0–31 after `0x4b2e8` | Full training |
| Temperature | THS DDR sensor (channel 1), calibrated as in [thermal.md](../thermal.md) | Full training when it differs from the capture by more than the record's limit, initially 15 °C |
| Retrain request | A SunstoneOS flag in an RTC general-purpose register that neither the ROM nor boot0 uses | Full training, flag cleared |

After the restore, a firmware result other than 7, a failed part A or a
size other than the record's sets the retrain flag and requests a warm
reset; the next boot trains fully. Part B failing in the stage-2 monitor
does the same. A full-training boot never reads the record's register set.
The temperature read needs the THS clock, reset and enable writes that
boot0 never makes; the review must accept them, or the first prototype uses
the capture-time limit only on the FEL path, where the host supplies the
temperature from a preceding thermal reading.

## The record format

Little-endian. The header occupies the first 256 bytes of sector 24,496,
the sector where boot0 looks for `bootpara`; the payload occupies sectors
24,432–24,495 and 24,497–24,499 as needed.

| Offset | Field |
| --- | --- |
| 0 | Magic `AS73DTRN` |
| 8, 12 | Version 1; header size 256 |
| 16, 20 | Payload length; payload CRC-32 |
| 24 | Generation, incremented by every record write |
| 32 | Chip ID, four words |
| 48 | boot0 header checksum, word `0x0c` of the image the record was made with |
| 52, 56 | Selected parameter-set index; CRC-32 of parameter words 0–31 |
| 60, 64 | Capture temperature in m°C; allowed difference in m°C |
| 68 | `dram_tpr10` training-step value of the capture's training |
| 72 | DRAM size in MiB |
| 76..103 | Payload section offsets and counts: PHY registers, message-block fields, mode-register writes, parameter words 32–51 |
| 104..251 | Reserved, zero |
| 252 | Header CRC-32 over bytes 0–251 |

Payload entries are `(u32 address, u16 value, u16 flags)`; mode-register
writes are `(u8 channel, u8 rank, u8 register, u8 value)`. 4,000 PHY
registers need 32,000 bytes, so the 34,560 bytes available hold the restore
set if it stays below about 4,300 registers; a larger set needs a second
extent and a format version. Writing the header with this magic makes
boot0's own `bootpara` check fail, which leaves boot0's normal behaviour
unchanged; boot0 never writes the card.

## The record's card region and its update

Sectors 24,432–24,499 lie in the boot area and in the 4 MiB unit
16,384–24,575 that also holds the slot B header. The vendor layout uses
sector 24,504 (SD parameter region) and 24,512–24,571 (tuning pattern) in
the same unit; the record avoids both. `tools/storage-update.py` gains a
`training-record` owned region `(24432, 68)` with these rules:

- the plan input is a training-record plan from a new host tool, separate
  from boot-slot plans, and every write must lie inside the region;
- a host backup of the region and of the slot B header precedes the write
  session, under [docs/goal.md](../goal.md) section 4;
- slot A must hold a valid header before the session, so that a damaged
  unit still leaves a selectable slot; after the session the host reads the
  slot B header back and compares it with the backup, restoring it through
  its own session on any difference;
- the payload is written before the header, so an interrupted update leaves
  a header whose CRC fails, and boot0 trains fully.

No card write happens until restored boots have passed the qualification
below. Each card's backup must also show sectors 24,432–24,499 zero or
holding only an earlier record; any other content stops the plan.

## Card boots need a host-built boot0 variant

boot0 always trains during a card boot, before any SunstoneOS code runs.
A card restore therefore needs boot0 changed: a host tool builds a variant
from the pinned image with the hook in the empty parameter slots 8–15
(`0x476f4`–`0x47af3`, 1,024 bytes; the strap always selects slot 5), the
call at `0x56142` redirected to it and the eGON checksum recomputed. The
pinned image itself is not committed, as today. This changes the
[microSD package](../microsd-boot-package.md)'s property that boot0 is
unmodified, and it needs the coordinator's decision. The boards boot the
unsigned `eGON.BT0` format, so the ROM checks only the additive checksum
(inferred; secure-boot fuses are unconfirmed, as in
[boot-rom-and-boot0.md](../vendor-firmware/boot-rom-and-boot0.md)). The FEL
path needs none of this: the DDR wrapper patches boot0 in RAM, as
`tools/prepare-ddr.py` already patches its UART output, and the host
supplies the record.

## Periodic retraining every N boots

A boot counter must survive power removal without a card write on every
boot. The OS keeps the count in its data partition, which section 4 permits
once the card has a host backup and the image confines its writes there.
When the count reaches N, the OS clears the record header through the
backup-first path, one sector, and the next boot trains fully; the stage-2
monitor then captures the restore set into DRAM and the OS writes a new
record. Until a native backup-first writer is qualified, the coordinator
does both writes from the host. N comes from the drift data: the number of
boots, or the time and temperature span, over which restored values stay
within the spread of fresh trainings.

## Qualification before any card write

1. **Captures** on both boards, several lifetimes each and two
   temperatures, analysed; the restore set and the mode-register list
   frozen and reviewed.
2. **FEL restore prototype**, port 3-3 then 3-1: each restored boot followed
   by the full March C- run of the
   [memory qualification image](../memory-qualification.md) and a stress
   run, hot (after a sustained CPU and memory load) and cold (after a long
   power-off at room temperature or below). Any bit error stops the work.
3. **Fallback cases** on the FEL path: a record with a wrong CRC, another
   board's chip ID, a changed strap hash, a temperature limit of zero and a
   forced quick-check failure must each train fully.
4. **Boot time**: ten restored and ten fully trained boots per board,
   comparing boot0's `DRAM BOOT DRIVE INFO` to `Actual DRAM SIZE` interval.
5. **Card**: host backup, record write through the new region, then the
   card-boot variant repeats steps 2–4.

## Budgets

| Quantity | Budget | Today |
| --- | --- | --- |
| DRAM interval with restore | 500 ms (prediction 200–500 ms, *estimated*) | 5,094 ms |
| Record read and CRC | 10 ms | none |
| Temperature read | 5 ms | none |
| Quick check, parts A and B | 20 ms | 7 ms simple test |
| Hook code in boot0 | 1,024 bytes | none |
| Fallback cost | One warm reset plus full training, about 6 s | none |

The measurement is the boot0 interval above, from boot0's own log. A
restored boot counts only with a passing quick check, and the restore path
counts only after step 2 above passes on both boards.

## Open questions for the review

- Whether the firmware accepts a device-initialization-only step value, and
  which trained mode registers it does not carry in the message block.
- Whether constant-class registers include training results that repeat at
  one temperature; the temperature captures decide it.
- The RTC general-purpose register for the retrain flag, and whether its
  value survives the warm reset that the fallback uses.
- The coordinator's decision on a modified boot0 for card boots.
