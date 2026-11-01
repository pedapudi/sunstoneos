# Design: restoring DRAM training from a record on the microSD card

Status: **RAM-only restore implemented and host-checked; no physical restore
has run.** The capture tools, fixed restore set, record builder, SRAM
admission and restore helper are implemented. On the FEL path the restore
runs under WDT0: a failed or hung restore resets the board to a new FEL
instance and the host trains fully once. The card allocation and record
planner support legacy single-unit records
and paired records in separate units. Physical card integration remains
unqualified. Independent code review and physical prerequisites are recorded in the [first-test runbook](../board-runbooks/dram-restore-test-1.md).

Training restore improves boot time; the existing full-training path
continues to provide DRAM initialization. The user approved replacing boot0's
5.1 s DRAM training with a restore of
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
   `dram_tpr13` bit 2 (`0x4bcb2`), and the call `0x581b4` (`0x57a08`) does
   not run. (The block at `0x55a98` does not run on either path on this
   board, because `dram_tpr10` bit 29 is set during training, `0x55a9e`.)
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

**The first P-state-0-only lifetime** (`board-session-20261025-25`) and the
four-P-state lifetime differ in 13,717 registers
(`dram-training-analysis-20261025/port-3-3-two-lifetimes.json`). They fall
into two groups:

- **Programmed differently for the two kinds, about 13,500:** every one of
  the four `0xa0000` copies (about 3,300 each), 163 address/command
  sequencer words that move between positions, twelve master registers
  shifted by exactly `0x38`, the multi-P-state message-block fields at
  `0x0a9b00f0`–`0x0a9b041a` (zero after the P-state-0-only run), byte 4 of
  the message block (`0xa0` against `0x30`, as predicted) and seven
  controller words. boot0 writes these itself on the restore's own path, so
  they are configuration and stay out of the restore set; the `0xa0000`
  block behaves like sequence memory loaded per kind.
- **Training results, about 250:** 219 byte-lane registers, 186 of which
  moved by at most 4 codes (lane offsets `0x20`–`0x26`, `0x40`–`0x42`,
  `0x4c`–`0x4e`, `0x9c`–`0x9e` and `0x1a4`–`0x1be`), the message-block result
  fields at `0x0a9b0036`–`0x0a9b0058` and `0x0a9b00d0`–`0x0a9b00de`, three
  master registers (`0x0a940058`–`0x0a940060`, `0x82f` against `0x833`), six
  `0x30000` registers, controller word `0x0a020040` (`0x2f2f` against
  `0x2e2e`, the MR14 pattern) and parameter words 32–51.

The training-step value at `0x0a9b0010` read `0x125f` in both kinds.

**Ten lifetimes on both boards** (the [runbook status](../board-runbooks/dram-training-capture.md)
lists them) give the first restore sets:

- **The firmware accepts the initialization-only step value.** It reported
  result 7 after 36 ms on both boards, and `init_DRAM` reached the size line
  after 40 ms; the simple test then failed, because DRAM was untrained.
- **The `0xa0000` block is left out of the set.** Between two
  P-state-0-only lifetimes of one board, 3,474 of the 3,575 words of its
  P-state 0 copy changed, mostly by one or two bits, including words that
  boot0 writes with fixed values. That behaviour is neither configuration
  nor a training result; its function needs the vendor reference, and the
  restore relies on boot0 rewriting the block on its own path.
- **The restore sets agree.** Without that block, the P-state-0-only minus
  initialization-only diff holds 1,026 registers on 3-3 and 1,052 on 3-1,
  1,008 of them on both. The set is derived by one recorded command over
  all twenty captures, per board and united, with status pooled over both
  boards and with firmware controls, firmware inputs and controller
  read-result words left out.
- **Byte-lane status is decided per register offset.** The byte lanes share
  one register map, so an offset that is status in any lane is left out in
  every lane: `0xe0`, `0xe2`, `0xe8` and `0x1c4`, status in the lanes of
  byte blocks 0 and 1. Offset `0xe6` (register `0x73`) is left out in every
  lane because it does not hold a written value: on port 3-1 the restore
  wrote 6 to lane 0 and read 0 in all bits, after nineteen registers of the
  same lane had read back their written values. Its trained value is the
  same in every trained capture of both boards, a property of hardware
  status ([runbook](../board-runbooks/dram-restore-test-1.md#the-first-board-run-stopped-at-a-byte-lane-register-that-does-not-hold-a-write)).
- **Registers the PHY owns are left out** because a write survey on port
  3-1 showed that they do not keep an APB write at the hook
  ([runbook](../board-runbooks/dram-restore-test-1.md#the-survey-run-separated-wide-memory-words-from-registers-the-phy-owns)):
  - byte-lane offset `0x11e` in every lane, a copy that the PHY updates from
    offset `0x4c` (lane 0) or `0x4e` (lane 1 from lane 0);
  - byte-lane offsets `0x1aa`–`0x1be` and `0x130`, values the PHY produces,
    which differ between lifetimes of the same kind, also after
    initialization only;
  - offsets `0x15e`–`0x17c` of the two blocks at CSR `0x30000` and
    `0x31000` (`0x0a960000`, `0x0a962000`), and `0x0a940014`, values of the
    same kind.
- **The set has 899 registers:** 402 byte-lane registers, 445
  address/command sequencer (ACSM) words, 10 registers of the two
  `0x30000`-region blocks, 20 message-block fields, 20 parameter words and
  two others. The
  [first test's runbook](../board-runbooks/dram-restore-test-1.md) lists the
  exclusions and their reasons.
- **The chip IDs differ in their last word only** (`0x40892413` on 3-3,
  `0x40792413` on 3-1). Trained byte-lane values differ between the boards
  by a median of 2 codes, so a record belongs to one board, as the chip-ID
  check requires.

The last reconnect of 2026-10-25 added a four-P-state lifetime per board,
which confirms the status class (`0x0a51022c`, already status on 3-1) and shows
that the `0xa0000` block is mostly stable after four-P-state training but
not after P-state-0-only training. No further lifetime is available, so
the set is fixed from these lifetimes; [the first test's runbook](../board-runbooks/dram-restore-test-1.md)
gives the set's derivation, the build it needs, the predictions, the
qualification ladder and the residual risk. Until cold and warm lifetimes
exist, a record is valid only for room-temperature cold starts.

**Projected DRAM time with a restore:** 40 ms for boot0's
initialization-only path, plus about 1 ms for 879 16-bit PHY writes and
their read-back, the 20 parameter words being set before the call (*estimated* at 1 µs per APB write), plus the
quick check (*estimated* at 30 ms for 131,196 uncached word accesses): about 70 ms against 5,093 ms of training today, a saving
of about 5.0 s per cold boot.

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

The sequence below describes the intended qualified path, including future
in-place fallback. The implemented first experiment uses the narrower
[RAM-only contract](#ram-only-implementation-contract): it returns a failure
to FEL and never calls `init_DRAM` twice in one lifetime.

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
  under its masks in record order (byte lanes, master, address/command and
  the per-P-state block; the `0x0000`–`0xbfff` range read zero and is
  included only if the kind 2 and kind 3 diff shows values there), set
  `MicroContMuxSel` to 1 and call `0x4bca8` as boot0 would.

The first physical prototype must establish whether the firmware accepts the
device-initialization-only value. Retrying `init_DRAM` after partial restore
is outside the accepted implementation and qualification plan.

**Future qualification proposal, not implemented.** The wider Part A/Part B
checks and RTC reset policy below need separate design and physical evidence.
The accepted implementation uses the uncached 8 KiB check described under
RAM-only restore and terminal failure behavior; it does not implement this
reset proposal.

Part A, in the AArch32 hook below 4 GiB: a short burst
test per channel, rank and byte lane with walking-ones and pseudo-random
patterns over 1 MiB per rank, written with cached `LDM`/`STM` bursts after
the cache setup that boot0's parser uses, then cleaned and invalidated by
address (`DCCIMVAC`) before the read phase, so that the reads reach DRAM
and not the cluster caches. Part B, in the stage-2 monitor at EL3: the same
pattern and cache maintenance (`DC CIVAC`) over the 12,288 MiB map, one
64 KiB window per 256 MiB, excluding the monitor's own image. After a
restored boot, part B failing sets a retrain flag in an RTC general-purpose
register other than indexes 1 and 2, which boot0 uses (`0x4a940`,
`0x4a95c`), and requests a warm reset. After a full-training boot, part B
failing stops with a report and no reset, so a bad full training cannot
loop. The flag is cleared only when part B passes. The check
catches gross restore errors only. Marginal timing shows only in the
qualification runs, and the safety of the restore rests on them.

## Pre-admission rejection trains fully; post-edit failure resets on the FEL path and stops on the card

| Check, before any parameter edit | Mismatch |
| --- | --- |
| Record magic, version, header CRC and payload CRC | Full training |
| Pinned boot0 checksum, a constant in the hook, equal to the record's | Full training |
| Chip ID, SID `0x03006200`–`0x0300620c` | Full training |
| Strap: selected set index and CRC of parameter words 0–31 | Full training |
| Temperature: THS DDR sensor (channel 1), calibrated as in [thermal.md](../thermal.md), within the record's limit of the capture | Full training |
| Future Part B retrain flag (not implemented) | Proposed full training; requires a qualified reset/flag contract |

The accepted SRAM implementation rejects an invalid record before changing any
parameter. Once restore changes parameters or PHY registers, readback, size
and quick-check failures stop the attempt. Neither path retrains in place,
because the PHY state after a partial restore is unknown and retraining
within one lifetime has never run on these boards.

- **FEL path.** The wrapper reports the error through its SRAM mailbox and
  returns through the normal FEL swap. The host then resets the board with
  WDT0 and runs the plain four-P-state wrapper once on the new FEL instance,
  as described under [the FEL runner](#the-fel-runner-falls-back-through-one-watchdog-reset).
- **Card path.** The hook stops in SRAM. A warm reset after a card boot has
  no qualified recovery destination.

Runtime temperature admission remains unimplemented. Current captures contain
unknown temperature and no qualified range. A default card image therefore
uses full training; the experimental image described below requires an explicit
host build option. Future THS admission requires capture data establishing the
allowed drift before the card hook can enable it.

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

This is the legacy planner format. The paired-copy host tools below extend
this allocation; physical card work remains unqualified.

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

Status: host-built and instruction-checked; physical card restore remains
unqualified. `tools/prepare-card-ddr.py` derives an optional image from the
pinned 245,760-byte boot0 and leaves the default microSD packager input unchanged. It recomputes the eGON
additive checksum and publishes the original and derived identities. This
does not establish ROM acceptance, secure-boot fuse state or physical boot.

The source audit corrects the earlier multi-sector read proposal:

| Pinned boot0 address | Derived contract |
| --- | --- |
| `0x5db08` | Thumb function: `r0` sector, `r1 = 1`, `r2` SRAM destination. Returns zero on success; rejects any count other than one. |
| `0x5baa8` | Lower reader: controller, sector, count, destination in `r0..r3`; returns the completed count or zero. It is not the pre-training hook's interface. |
| `0x5b88c..0x5b896` | Transfers above 512 bytes enter the DMA branch; `0x5d5e8` places its descriptor buffer at untrained DRAM `0x42000000`. Single-sector reads select PIO. |
| `0x49ab4..0x49ade` | boot0 initializes storage and performs four single-sector reads into SRAM before `init_DRAM`. The initialized flag is at `0x83c14`. |
| `0x5da64` | Returns the boot storage selector; the hook admits only selector zero, SD0. |
| `0x4b372..0x4b37e` | A parameter slot's first word determines whether it is populated. Code in empty slots must preserve those words as zero. |
| `0x47b1c..0x47b34` | Card entry disables translation/data cache/instruction cache and starts its stack at `0x8d000`. |
| `0x5e216`, `0x56142` | Calls to `init_DRAM` and per-P-state timing; the two redirected existing calls. The timer entry at `0x48d6c` is also patched as described below. |

The audit uses boot0 SHA-256
`829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3`.
Raw instruction excerpts stay in
`~/.cache/sunstoneos/evidence/dram-card-restore-20261026/`.

### The bootstrap loads a separately reserved helper

Only 1,024 bytes in parameter slots 8–15 (`0x476f4..0x47af4`) are
available. A small Thumb bootstrap occupies these slots while leaving the
first four bytes of every slot zero. Branches skip those words. The selected
set 5 at `0x47574` remains untouched. A separate timing trampoline calls
boot0's original `0x4bca8` whenever restore mode is inactive, preserving all
four-P-state training calls after a pre-admission rejection.

Each paired 4 MiB record unit reserves its first 49 sectors for the record
and sectors 64–127 for a maximum 32 KiB helper. The record format remains
version 1. A dedicated helper plan writes these two helper extents through
the existing backup-first storage sessions. It writes no record header,
payload, boot slot or partition data. The derived boot0 embeds the unit
addresses, helper length and CRC. A stale helper cannot execute merely
because a record is valid. Helper and boot0 update ordering can temporarily
force full training without invalidating either saved record.

The bootstrap uses the proven one-sector PIO interface to load a helper
into `0x92000..0x9a000`. It checks the complete helper CRC before entering
it. A successful read with a bad helper CRC may try the other reserved
copy; a card-read error or deadline expiry stores an SRAM error and terminal-stops.
It does not retry storage or enter full training with uncertain controller state. The host producer must
validate paired allocation geometry against an ordinary-file backup and
must never infer a physical reservation from that file.

The bootstrap saves `r0`, `r1`, callee-saved registers, return address and the
original stack pointer before calling storage or the helper. It restores that
context before the one original `init_DRAM` call on fallback, including the
original two arguments. The inactive timing trampoline resides in boot0, so it
remains usable even when no helper passes CRC.

### Restore reads have one full-width deadline

The vendor timer at `0x48d6c` divides only the low 32 bits of the 24 MHz
physical counter. Its result wraps about every 179 seconds. PIO also refreshes
its timeout after every word. Neither mechanism bounds the complete transfer.
The optional image redirects this timer entry to a small bootstrap shim.
An active flag starts at zero in loaded boot0 bytes, outside every slot's
presence word. Inactive calls reproduce the original counter instruction and
continue at `0x48d70`, preserving ordinary boot timing behavior.

Before its first sector read, restore captures the full 64-bit counter. Every
subsequent read shares a 240,000,000-tick deadline: ten seconds at the required
24 MHz counter frequency. The wrapper checks elapsed time before and after
each read. While a read is active, each vendor timer call checks the same
full-width elapsed value, which FIFO progress cannot refresh. Expiry records
an SRAM error and stops with interrupts masked. This bounds a blocking read
by the remaining attempt budget plus one finite poll iteration, assuming the
counter advances and MMIO accesses return. It does not claim protection from
a stalled bus or stopped CPU. The wrapper requires the vendor zero-success result before returning.

The focused call graph is `0x5db08` → `0x5baa8` → `0x5b9cc`/`0x5ba1c` →
`0x5b32c`. The single-sector branch skips DMA descriptor preparation and the
multi-sector stop/status loop. PIO FIFO, command completion, data completion, controller-reset and error-path
clock-reset waits all call `0x48d6c`;
the last is in `0x5d0d8`. The shared busy-response loop also calls the timer,
although single-sector CMD17 does not request that response. Before reading,
admission checks the initialized SD0
controller, 512-byte block size, nonzero chunk limit, pinned host function
pointer, SD0 register/FIFO addresses, card capacity and 32-bit LBA arithmetic. Those checks prevent malformed metadata from selecting DMA or a
zero-progress outer loop.

UART output has a separate unbounded wait. The wrapper saves and clears the
existing log-enable byte at `0x60670` during each read, then restores it on
success. Both reachable log functions, `0x498e4` and `0x4993c`, return before
formatting or UART access when that byte is zero. The timer's active flag is
also cleared on return. Focused evidence is preserved in `boot0-pio-focused.txt`,
`boot0-read-waits-focused.txt` and `boot0-read-callgraph.txt` in the audit cache.
No additional SD reader or ordinary-boot timeout policy is introduced.

The helper uses a private stack at `0x9b000..0x9f000`. Record A and B
buffers occupy `0xa0000..0xa7000` and `0xa7000..0xae000`; each has room for
a 24 KiB record and a final padded sector. The original card-boot stack,
boot0 body/BSS, mailbox at `0x86000` and ROM scratch intervals remain
disjoint. The build rejects allocated sections outside those intervals or
uninitialized sections that the raw loader would not initialize.

### Admission, fallback and temperature limits

The helper checks both copies without editing parameters, using the existing
fixed address/mask profile, actual SID, original firmware identity, selected
parameter CRC and highest nonzero generation, with A first on a tie. Only
the selected record is applied. The record continues to name the original
boot0 checksum; the derivative's checksum necessarily differs. The host
producer pins every unchanged byte and the allowed call and timer patches. A
fixed configuration block at `0x47ac8` supplies the experimental temperature
policy, unit sectors, helper length/CRC and deadline to the executing code.
The packager checks those bytes against the manifest and the helper artifact;
a JSON policy edit cannot relabel an enabled image as the default variant.

A missing, corrupt, stale, wrong-board or otherwise ineligible record
calls the original `init_DRAM` once with the original parameters. The
inactive timing trampoline preserves boot0's normal training path. After
restore starts, a readback, size or sampled-memory failure stores its error
in SRAM and stops without returning to boot0 or retraining. No warm-reset
or autonomous recovery destination is assumed.

Current records have unknown capture temperature and no qualified thermal
range. The default derived image therefore falls back to full training.
An explicitly requested experimental build may exercise the existing
room-temperature restore profile, with false physical/thermal qualification
fields in its manifest. It has no runtime temperature measurement. That
build is an offline prototype, not permission to boot a board; physical
use still requires the supervised cold-start and memory-test prerequisites.
Runtime temperature admission needs new capture data before a production
card-restore policy can be enabled.

The host interface takes `--boot0`, `--restore-set`, `--allocation`, `--backup`
and a new `--output` directory. The allocation must bind the complete ordinary
backup's hash and capacity and contain two disjoint reserved record units.
`--experimental-unknown-temperature` enables the unqualified restore policy;
omitting it produces a zero-read, full-training image. The output includes
`boot0-card-restore.bin`, `card-ddr-helper.bin`, `manifest.json` and a
`helper-plan/plan.json` accepted by `tools/storage-update.py plan`.
`tools/prepare-microsd-boot.py --restore-variant manifest.json` explicitly selects
the derivative while still requiring the original pinned `--boot0` input.

Helper updates occupy only their two reserved subextents. Each has its own
backup-first write session. The old or new boot0 executes only a helper whose
complete CRC matches its embedded configuration; an interrupted helper write
can cause full training after successful reads. A transport error stops.
Existing record headers and payloads are not part of the helper plan. These
ordinary-file plans establish neither physical reservation nor card-write
qualification.

The current offline evidence is in
`~/.cache/sunstoneos/evidence/dram-restore-1031b/` (`card-restore`,
`card-default` and their check directories). The experimental
boot0 SHA-256 is
`0a25e1ceaa79242702eff2b9758070f1db8022348d29d8c2cd7094e1a7d5ea11`;
the default image is
`78d5056f3313ceadd3323010daf67fc1f4e88dba190888d1a76c57ac2ccf5182`.
Both load the same 10,120-byte helper, SHA-256
`9ff8be424035314edd0e9da9a784cd1916ae4483a1c57347cbd636433d7ac08e`, which
includes the shared quick check.
Its single initialized section occupies `0x92000..0x94a3c`. Bootstrap sections
use 120, 76, 72, 64, 124, 80, 20 and 112 bytes in slots 8–15. Every section
fits after its zero presence word; no raw-loader BSS is admitted.

`checks/card-ddr.py` passed twenty experimental cases and the default fallback
case. It executes selection, fallback and terminal failures with
modeled boot0 training and MMIO. It checks original arguments, stack and
callee-saved registers, four fallback timing calls, helper/record corruption,
wrong-board rejection and stop-before-continuation after partial restore.
`checks/card-ddr-pio.py` executes the pinned PIO wait instructions and the
actual timer shim: thirteen cases include inactive-timer equivalence,
low-word wrap, slow FIFO progress, command/data/reset/clock-reset expiry and
a direct busy-loop check. `checks/card-ddr-host.py` passed nineteen cases
covering the actual producer, package/parser, exact helper ranges, stale/torn
resume refusal and preserved backup. The default instruction case performs
no restore reads. These checks model MMIO and stop at the terminal `WFE`;
they do not establish physical timing, ROM acceptance or DRAM margins.

For the actual record, the first valid helper uses 20 sectors and the two
records use 50 sectors: 35,840 bytes read. A bad first helper followed by a
valid second uses 46,080 bytes. The format caps both-helper/both-record reads
at 226 sectors, or 115,712 bytes. One non-refreshable ten-second counter
budget covers the attempt. Restore performs 879 masked PHY writes and up to
89 rewrites of other wide-memory halfwords, a readback pass, and a separate
879-read final pass. The quick
check writes and reads 262,392 bytes each way. The instruction fixture caps each
case at five million instructions; the largest observed case uses 4,541,947.
This count is not a hardware performance measure.

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
   mismatch and a forced quick-check failure. Pre-admission rejection must leave parameters
   unchanged and run full training once. Post-edit readback and memory failures
   must stop before caller continuation, with no in-place retraining; on the
   FEL path the runner then resets through WDT0 and trains fully once.
4. **Boot time**: ten restored and ten fully trained boots per board,
   comparing boot0's `DRAM BOOT DRIVE INFO` to `Actual DRAM SIZE` interval.
5. **Card**: host backup, record write through the new region, then the
   card-boot variant repeats steps 2–4.

## Budgets

| Quantity | Budget | Basis |
| --- | --- | --- |
| DRAM interval with restore | 500 ms; prediction about 70 ms | 40 ms measured for the initialization-only path on both boards, plus *estimated* writes and the quick check |
| Helper and record reads | At most 115,712 bytes; one ten-second attempt deadline | Single-sector PIO; physical latency unmeasured |
| CRC over the record | Bounded by the 24 KiB record limit | Instruction-checked; physical cost unmeasured |
| Temperature admission | Default full-training fallback | Runtime measurement and allowed range unqualified |
| Quick check | 31 address-line words and sixteen 4 KiB windows below 4 GiB; 262,392 bytes written and read each way; *estimated* 30 ms | Measured on the board by mailbox words 25 and 26; wider Part A/B checks remain future proposals |
| FEL fallback | One WDT0 reset (about 6 s, measured) and one four-P-state training (about 5.1 s) | Only after a failed or hung restore; 14.4 s from launch to the passing fallback on port 3-1 after a readback failure (2026-10-31) |
| Hook code | 668 bytes in 1,024 bytes of slots; separate 10,120-byte helper | Eight zero presence words; reserved helper extents |
| Pre-admission fallback | One full training in the same boot, about 5.1 s plus any successful reads | Measured full-training time; read latency unmeasured |

A restored boot counts only with a passing quick check, and the restore
path counts only after qualification step 2 passes on both boards.

## Open questions

The first answered: the firmware accepts the device-initialization-only
step value (result 7 after 36 ms on both boards). These remain:

- The RTC general-purpose register for part B's flag, and whether it
  survives the warm reset.
- The coordinator's decision on a modified boot0 for card boots, and on the
  record's own 4 MiB unit in place of the `bootpara` sectors.

## RAM-only implementation contract

The first implementation admits only the fixed 899-entry restore set and
its recorded P-state-0 captures. The builder binds the analysis file by
SHA-256, checks the capture chip ID, training kind and stage tables, and
uses the pinned boot0 parameter set 5 for the strap CRC. Capture words
32–51 are 32-bit values: the record encodes their 40 little-endian
halfwords as a run at `0x83278`, without truncating their upper halves.
The remaining 879 entries are masked PHY halfwords. Temperature is
unknown (`INT32_MIN`, limit zero), so this profile is restricted to the
supervised room-temperature cold-start experiment.

The wrapper retains its entry and mailbox at `0x85000` and `0x86000`.
The record occupies at most 24 KiB at `0x87000`, ending before the FEL
shadow at `0x8e000`. The restore helper and its compiled address/mask
allowlist occupy at most 32 KiB at `0x92000`; a private 16 KiB stack ends
at `0x9f000`. These intervals lie in CPU-routed shared SRAM
(`0x74000..0xf3fff`, [ROM analysis](../vendor-firmware/boot-rom-and-boot0.md#sram-stack-and-the-shared-sram-routing)).
The host backs up and verifies every added upload interval before launch.

Admission checks both CRCs, the complete record geometry, the actual SID,
pinned boot0 checksum, set index and selected parameter CRC before editing
parameters. It requires the complete compiled address and mask sequence;
a valid CRC cannot authorize another MMIO destination. It admits no
separate mode-register writes: boot0 consumes MR14/MR15 from words 50–51.
The wrapper clears training bits 29:16, selects initialization-only step
1 and P-state 0, and enables boot0's saved-value path through `dram_tpr1`
bit 31. The source-backed call at `0x56142` becomes a Thumb BLX to an ARM
trampoline. The trampoline preserves the original arguments, applies and
checks every PHY write with `MicroContMuxSel` clear, restores that mux to
1, then calls boot0's original `0x4bca8`.

The helper writes every entry, then reads every entry back in a separate
pass. Two PHY memories store words wider than an APB access: the ACSM
memory (`0x0a982000`–`0x0a983fff`) 64-bit words and the PHY data memory
(64 KiB from `0x0a9b0000`) 32-bit words. Each holds a halfword write until
the word's highest halfword is written, and reads return the stored word
until then. The helper therefore writes the whole word of each entry in
those ranges, in ascending halfword order, and rewrites the halfwords that
are not entries with their stored values; boot0's PIE and message-block
loaders also write every halfword in ascending order. For the 899-entry set
that adds 89 rewrites.

A readback mismatch does not end the readback pass, so one run names every
register that does not hold its value: mailbox words 11–13 keep the first
mismatch's address, expected and read values, word 27 counts the mismatches
and words 28–31 hold the second to fifth as profile index (upper half) and
value read (lower half). The PHY state is discarded by the WDT0 reset that follows. A failed pass then
exits directly to the wrapper's saved SRAM context with error 7; it must
not resume boot0 and reach its memory test. After a successful
`init_DRAM`, the wrapper checks the size, reads every entry back a second
time and runs the
[quick check](#the-quick-check-covers-address-lines-and-sixteen-windows-below-4-gib).
A mismatch in that second readback fails the restore with error 15, so the
mailbox names the pass that failed.

The second readback runs the PHY microcontroller clock. Between the hook
and the return from `init_DRAM`, boot0 runs its final PHY stage
(`0x4f46c`, here `0x5a210`), which ends with `UcclkHclkEnables` = 2
(`0x5ae9a`) and `MicroContMuxSel` = 1, and then, for an LPDDR5 set,
`0x57310`, called twice on these boards (`dram_tpr13` bit 6, `0x579ce`). That routine clears `MicroContMuxSel`, sets `UcclkHclkEnables`
to 3, reads thirteen halfwords of the message block from `0x0a9b0026` to
`0x0a9b004c`, programs controller timing from their maxima, writes
`UcclkHclkEnables` = 0 (`0x5749a`) and sets the mux to 1 (`0x574ac`).
No other boot0 code that runs after the hook writes the data memory or
`UcclkHclkEnables`: `0x4bca8`, which the hook calls on to, `0x5a210`,
`0x4b7ec`, `0x561a0`, the size probe `0x4b74c` and the simple test `0x4b4cc`
(the message-block writers `0x58ba8`, `0x590c4` and `0x59d00` run before
the hook). No code after the hook writes `MicroReset`, which stays 1, so the
training firmware stays stalled while the clock runs. With the clock off, the data memory
reads 0 over APB, as the port 3-1 run of 2026-10-31 showed. Its contents
are kept (inferred: `0x57310` reads them after the final PHY stage has
stopped the clock). The second readback therefore sets `UcclkHclkEnables` to 3 after
clearing the mux, as `0x57310` and the capture reader do, and writes back
the value it read (0 on boot0's path) before setting the mux to 1. The restored
message-block entries matter because `0x57310` reads `0x0a9b002c` and
`0x0a9b0030` among its thirteen halfwords.
The quick check finds gross errors only; the separate March C-
qualification remains required. Failures have distinct mailbox
codes for record geometry, header CRC, payload CRC, chip ID, boot0 identity,
strap, PHY readback at the hook, PHY readback after `init_DRAM`, size and
memory comparison. No failure retrains in
place. The host writes no RAM page after a reported restore failure.

The budget is one admission pass, one ordered PHY write pass and one
readback pass, with bounded record parsing and no allocation. The readback
pass follows the last write; explicit barriers surround mux transitions.
A second readback pass after
`init_DRAM` detects changes made by boot0 before the quick check. Host instruction checks
exercise the real wrapper/helper with modeled boot0 calls and MMIO. They
require exact fault codes and zero later memory accesses for wrong-board,
CRC, readback and quick-check failures. These checks establish software
ordering only; they do not qualify the PHY or authorize a launch.

### The quick check covers address lines and sixteen windows below 4 GiB

`restore_check` in `boot/a733/ddr-restore.c` runs after the second readback
pass, with the MMU and caches off. In AArch32 it reaches DRAM only from
`0x4000_0000` to `0xffff_ffff`, the first 3 GiB of the 12 GiB map. Both card
and FEL restores use it.

- **Address lines 2–31.** It writes a distinct word at `0x4000_0000` and at
  `0x4000_0000 + 2^k` for k = 2..31, reads all 31 back, then repeats with the
  complements. A stuck or shorted address line makes two of these words
  share a location, so the earlier word reads back wrong. This also tests,
  for single-bit offsets, the linear decode below 4 GiB that
  [memory-protection.md](../memory-protection.md) has not yet established.
- **Data.** Sixteen 4 KiB windows start at `0x4000_0000` and are 192 MiB apart;
  the last ends at `0xf400_0fff`. Each of four passes writes all windows
  before reading any: a rotating one (word i holds `1 << (i mod 32)`), its
  complement, the word address XOR `0xa55a5aa5`, and that value's
  complement. Every data bit is therefore driven to both levels while its
  neighbours differ.

The check touches 65,616 distinct bytes and writes and reads 262,392 bytes
each way. A mismatch stores the bytes read so
far, the address, the expected and the actual word in mailbox words 10–13.
Mailbox words 25 and 26 hold the low word of the 24 MHz system counter at the
start and end of `restore_check`, which boot0's own timer also reads. The
host reports the difference as the check time. In the instruction fixture
the nominal FEL case executes 2,247,330 instructions.

### The FEL runner falls back through one watchdog reset

`tools/run-ddr.py` with a restore build also takes `--fallback-build`, the
plain four-P-state wrapper for the same boot0. The run has at most one
WDT0 reset and one full training:

1. Before any launch it requires the reconnect access rule for the port,
   reads the SID over FEL and compares it with the record's chip ID. A
   mismatch skips the restore and runs the plain wrapper on the same
   instance, which is the ordinary cold path.
2. It requires WDT0 disabled, then arms it in the order measured on both
   boards on 2026-10-31: output 2, configuration 1 (system reset), mode
   `0x41` (enabled, four seconds) and restart word `0x14af`, all under key
   `0x16aa0000`, with readback. In that measurement the board left USB about
   4.2 s after arming and a new FEL instance enumerated at about 5.9 s.
   Evidence: `~/.cache/sunstoneos/evidence/wdt-fel-return-20261031/`.
3. One FEL request executes the wrapper and then writes mode `0x16aa0000`,
   so WDT0 is disabled as soon as the wrapper returns. The restore path runs
   boot0's power and clock setup, the 40 ms initialization and the quick
   check; it is *estimated* at under one second against the four-second
   interval. The runner records the request's duration as `exe_seconds`.
4. A clean restore result, the expected size and the host's two RAM pages
   end the run with exit status 0.
5. Any other returned result arms WDT0 again. A hang, or a request that
   fails while WDT0 counts, needs no second arming; the runner sends nothing
   more to the old instance.
6. Within 20 s of arming, the runner adopts the new FEL instance on the same
   port. The mode register keeps `0x41` through the reset, so an enabled
   mode shows that WDT0 fired. The runner disarms WDT0 and runs the plain
   wrapper once. Success gives exit status 3.

A missing new instance, an instance whose WDT0 was not enabled, a failed
full training or any FEL transport failure stops the run and quarantines the
current instance, as other runners do. Apart from the clock-point return
described below, the plain wrapper's code equals the code that trained both
boards to 12,288 MiB on the instances created by the 2026-10-31 watchdog reset
(`~/.cache/sunstoneos/evidence/resume-20261031b/`). The WDT0 register values
match the launchers in [watchdog.md](../watchdog.md); the runner writes no RTC
FEL flag or ROM redirect word, as in the measurement.

### The wrapper returns CPU-L and the DSU to the ROM clock point

boot0's board setup leaves CPU-L at 1014 MHz (PLL `0xf8802700`, mux
`0x03000000`) and the DSU at 780 MHz (`0xf8801e00`, `0x03000105`). The boot
ROM sets both to 494 MHz (PLL `0xf8802600`, muxes `0x03010000` and
`0x03010105`), but only when it is entered. The wrapper returns to the live FEL
instance with `bx lr`, so without a further step the next image would start at
boot0's points. At `finish` in `boot/a733/ddr-entry.S`, whenever the run
reached board setup (mailbox word 1 at least 2), the wrapper writes the ROM
words back. This covers the plain, restore and fallback runs. CPU-B keeps
boot0's word, as it does after a ROM entry.

CPU0 runs on CPU-L while this code runs, so each domain follows the CPU clock
driver's sequence (`drivers/clock/a733_cpu_clock.c`), timed with the 24 MHz
system counter:

1. Park the mux on the oscillator, read it back and wait 20 µs.
2. Write N = 38, then write it again with the update bit (bit 26) set.
3. Wait up to 10 ms for the update bit to clear.
4. Wait up to 70 ms for the lock bit (bit 28) on three consecutive reads at
   least 3 µs apart, then wait 20 µs.
5. Write the final mux with the source field clear (the divider alone), then
   the final mux, and wait 20 µs.

A domain that already holds the ROM words is left alone. Mailbox word 15 is 0
on success. Otherwise it is `0x100` × domain (1 CPU-L, 3 DSU) plus a code:

- 1: the PLL is not enabled, or its update bit is set. Nothing is written.
- 2: update timeout. 3: lock timeout. Both leave that mux on the oscillator.
- 4: the counter does not advance within `0x1000000` polls. The mux stays on
  the oscillator unless the stop comes during the last 20 µs wait.
- 5: the mux did not read back as parked. The PLL is not written.

A CPU-L failure leaves the DSU untouched. In every failure case FEL still
runs, at 24 MHz at worst. With the CPU on the oscillator, the 16,777,216
polls take tens of seconds (*estimated* at 1–3 µs per poll), longer than the
four-second WDT0 interval of a restore run, so a stopped counter there ends in
the watchdog reset.

`tools/run-ddr.py` refuses a build or fallback build whose manifest lacks
`clock_point_word` 15, before any device access, because such a build leaves
CPU-L and the DSU at boot0's points. After each run it reads, in the same FEL
transaction as the mailbox, the PLL, pattern (+4, +8), LFM (+0x18) and mux
words of the three domains, and records them as `cpu_clocks`, with word 15 as
`clock_point_result`. It requires the words that the OS entry guard
(`apps/os/cpu-return-entry.c`) requires: the ROM words for CPU-L and the DSU,
`0xf8802700`/`0x03000000` for CPU-B, and pattern and LFM off. A plain or
fallback run that does not meet them fails before the RAM-page checks; a
restore run falls back through the watchdog reset, whose ROM entry sets CPU-L
and the DSU again.

`checks/ddr-wrapper.py` runs the wrapper against a model of the PLL block
(`checks/ddr_clock_model.py`) in which the old lock bit stays set until the
update bit clears. It checks the order of writes, the waits and the final
words, including a counter that wraps and a lock bit that drops once. It also
covers a lock timeout, an update timeout, a disabled PLL, a mux that does not
park, a domain already at the ROM point, and a stopped counter before the
first wait, during the update wait and during the lock wait; for the last
three the check lowers the poll limit to 2,000.

### A diagnostic build records every entry around its write

`tools/prepare-ddr.py --variant restore --survey` builds the restore helper
with `RESTORE_SURVEY` defined; without the flag the helper's bytes do not
change. The diagnostic helper fills a survey at SRAM `0x96000`, inside the
helper reservation and above the helper's 9,912 bytes:

- **Header, 64 bytes.** Word 0 is `0x59565253` once the write pass starts,
  word 1 counts the entries recorded, word 2 holds `MicroContMuxSel` before
  the helper clears it, and words 3–6 hold `UcclkHclkEnables`
  (`0x0aa80100`), `MicroReset` (`0x0aaa0132`), `DctWriteProt`
  (`0x0aaa0062`) and `UctShadowRegs` (`0x0aaa0008`) at the hook.
- **Header word 7** counts the wide-memory halfwords rewritten with their
  stored values.
- **Header words 8–10.** Word 8 is 1 once the readback after `init_DRAM`
  starts; words 9 and 10 hold the `MicroContMuxSel` and `UcclkHclkEnables`
  values that pass found before it changed them (1 and 0 on boot0's path).
- **Entries, 8 bytes each from `0x96040`.** For each entry in profile
  order: the value read before the write (the helper's read-modify-write
  already reads it), the value read straight after it, the value the
  readback pass reads, and the mask.

For the 879 PHY entries of the 899-entry set the survey takes 7,096 bytes,
ending at `0x97bb8`. The passes, the mismatch reporting and the outcome are
those of the production helper, plus one read of each entry straight after
its write.

`tools/run-ddr.py` uploads zeros over the survey with the helper and reads
them back, so a helper that stops before its write pass leaves no earlier
values. After the restore returns, it reads the survey, and
`tools/ddr-survey.py` puts each entry in one class, from the masked
comparison with the record's value:

| Class | Values read | Meaning |
| --- | --- | --- |
| held | right straight after the write and in the readback pass | The register stores the value |
| already-at-value | as held, and the value before already matched | The write could not be observed |
| stored-later | wrong straight after the write, right in the readback pass | A wide-memory halfword stored with its word |
| changed-later | right straight after the write, wrong in the readback pass | The PHY or a later write changed it |
| unchanged | wrong in the readback pass and equal to the value before | The write did not reach a storage register |
| reads-zero | 0 in the readback pass, although the value before was not | A copy or status register the PHY clears |
| other-value | neither the value before nor the written value | The PHY produced the value |

The last four are mismatches.

The runner writes every entry to `survey.json`, records the class counts,
the mismatches by register family and the control words in
`restore_result.survey`, and checks the survey's mismatches against the
mailbox: they equal word 27 after error 7 and are 0 otherwise, because the
survey records only the passes at the hook. A survey without its magic word, or with
fewer entries than the profile, is recorded as an error and the run falls
back as usual. `tools/ddr-survey.py SURVEY.BIN --record RECORD.BIN` decodes a
saved survey offline.

## Two-copy host selection and standby update contract

The implemented host tools reuse `boot-slots.py` allocations and
`storage-update.py` backup-first sessions. The record bytes remain version
1. Existing single-unit allocations and commands keep their behavior.
A new allocation field, `training_records`, assigns copies A and B to two
separate 4 MiB units. It cannot coexist with `training_record`. Both units
must lie beyond the boot area, outside slots and partitions, and inside
the backed-up image. Allocation reads the actual ordinary-file backup and
records its capacity and hash; it does not establish a physical reservation.
No physical allocation or full-card backup has been identified for this stage.

The selector command is `training-record.py select --allocation ALLOCATION
--card-image IMAGE --record REFERENCE --restore-set ANALYSIS`. The reference
is a record from `from-capture`; the fixed analysis file is
`restore-set-v4.json`. `plan-standby` takes the same arguments and an
`--output DIRECTORY`. Its `plan.json` becomes the `--slot-plan` input to
`storage-update.py plan`, which produces the existing per-range sessions.

Selection reads each header before its bounded payload. It checks both
CRCs and the admitted RAM-restore profile: chip ID, boot0 identity, selected
parameter CRC, temperature sentinel, size, register addresses and masks.
The target identity comes from a real capture-derived reference record.
Among eligible copies it chooses the highest 64-bit generation, with A
first on a tie. No eligible copy means `full-training`, before any restore
parameter or PHY edit. This host selector specifies the later card hook's
behavior; it does not implement a card boot or retry a partial restore.

`training-record.py plan-standby` targets the other copy, or A when neither
copy is eligible. It refuses a valid record from another board, a repeated
active payload and generation exhaustion. The planner assigns generation
one above the eligible copies and recomputes the header CRC. It writes the
standby payload first and the header sector last. Each range remains an
existing independent backup-first session. The non-target copy's inspected
header and payload hashes bind selection to the current card image when
`storage-update.py plan` converts the writes into sessions. A changed source
copy requires a fresh plan, including when that copy was invalid or no
copy was selected. Target ranges must equal their original bytes or the
planned bytes. A torn range matching neither refuses conversion and needs
a fresh backup and plan.

The update leaves the non-target copy intact. A CRC-invalid standby copy
cannot replace an eligible older copy. Once the new header and payload
validate, the new generation is selected. This can happen after the first
128 bytes of a header sector change, when its remaining bytes already match. Resuming uses the same plan; preparing a
fresh identical update is refused so that it cannot overwrite the fallback.
Coordinator serialization and a fresh read-only backup remain required for
each physical session. Plan ordering and CRCs do not establish card power-loss
atomicity or physical recovery qualification.

The fixed-profile read budget is two header sectors and at most two 24 KiB
records. The 11,640-byte records require 24,576 card-image bytes
in total, including sector padding. Selection bounds CRC and profile work by these records, with no device
access. Checks use records from the actual captures and
allocations from the actual producer on ordinary-file fixtures. They cover
blank copies, generation ordering and ties, bad CRCs and board identity,
interruption after each written sector, unchanged active bytes, stale-image
refusal, malformed allocations and generation exhaustion. Shared storage
wire formats and native drivers require no changes. The accepted RAM-only
restore artifacts retain their earlier evidence.

Host evidence is in `~/.cache/sunstoneos/evidence/resume-dram-record-copies-1025/`.
`check-final` passes 32 named cases and 28 sector interruption points, with
the original ordinary-file backup preserved and hashed. `legacy` passes the
existing single-copy planner check, and `boot-slots` passes its 19 cases.
The paired allocation assigns sectors 57,344–65,535 and 65,536–73,727;
these are fixture reservations, not physical card reservations. No physical
card allocation or backup authority is inferred from them. Independent
correctness review approved the host implementation; physical qualification
remains outstanding.

## Next steps

Implementation and remaining physical work:

| Part | Status |
| --- | --- |
| Capture reader, stage tables, runner, DRAM and SRAM builds, wrapper variants | Implemented, host-checked, frozen (v5 sets), run in ten lifetimes |
| `tools/analyse-ddr-captures.py`, kind diff and block exclusion | Implemented, host-checked |
| Restore set | Fixed: `~/.cache/sunstoneos/evidence/dram-final-readback-1031/restore-set-v5.json`, command in `restore-set-v5.command` |
| Record format, paired selection and standby plans | Implemented and host-checked; physical card stage not started |
| Record builder from a capture | Implemented; both actual board references and identity refusals checked |
| Restore wrapper variant | Implemented; instruction and host-runner cases checked |
| FEL fallback through one WDT0 reset, address-line and window quick check | Implemented and host-checked; the fallback ran on port 3-1 and the quick check passed on both boards (2026-10-31) |
| March C- runs on the restored and P-state-0-only paths | Image implemented and host-modeled, never run on a board |
| First restore test, [runbook](../board-runbooks/dram-restore-test-1.md) | Passed on both boards on 2026-10-31 (FEL path) |
| Restore by SID as the development default, [records runbook](../board-runbooks/dram-restore-records.md) | Implemented and host-checked; not yet run on a board |

**Facts the next steps rest on**, all measured on both boards on
2026-10-25 except where marked:

- boot0's initialization-only path reaches firmware result 7 in 36 ms and
  the size line in 40 ms; P-state-0-only training takes 4,437–4,451 ms and
  four-P-state training 5,088–5,102 ms.
- The restore set is 899 registers: the union of both boards'
  P-state-0-only minus initialization-only diffs, with status pooled over
  both boards and decided per byte-lane offset, and without the `0xa0000`
  block, firmware controls, firmware inputs, controller read-result words,
  byte-lane register `0x73`, and the 42 registers that the port 3-1 survey
  showed the PHY owns.
- The values come from one capture of each board's later P-state-0-only
  lifetime: `board-session-20261025-27/port-3-3` and
  `board-session-20261025-35/port-3-1`.
- The `0xa0000` block is left out: the P-state-0-only path leaves it at its
  power-on contents, and the four-P-state path writes it, probably through
  `0x581b4` (inferred).
- MR14 and MR15 travel in parameter words 50 and 51 through boot0's
  `dram_tpr1` bit 31 path, which needs `dram_tpr10` bit 29 cleared as well
  (`0x5a08e` runs only with bit 29 clear).

**Work in order:**

1. **Record builder, implemented.** `tools/training-record.py from-capture`: read a
   P-state-0-only capture and `restore-set-v4.json`, refuse a capture
   whose SID chip ID, training kind or stage tables differ, and emit the
   record with the chip ID, the pinned boot0 checksum, the CRC of boot0's
   parameter set 5 (the strap check; both boards select it), unknown
   temperature fields, register runs and parameter words 32–51. The set
   holds only 16-bit PHY registers and parameter words, so runs need no
   width flag.
2. **Restore wrapper variant, implemented.** `tools/prepare-ddr.py --variant restore`:
   record in SRAM at `0x87000`, record checks before any edit (magic,
   version, both CRCs, chip ID, boot0 checksum, strap CRC), the parameter
   edits (`dram_tpr13` bit 2 clear, `dram_tpr10` bits 29:16 set to
   `0x0001`, `dram_tpr1` bit 31 set, words 32–51), a Thumb-to-Arm `BLX` at
   `0x56142` into a table-driven writer that clears `MicroContMuxSel`,
   writes and reads back the set, sets it to 1 and calls `0x4bca8`, then
   after `init_DRAM` the quick check below 4 GiB. Any failure stops
   and reports in the mailbox; there is no in-place retraining.
   `tools/run-ddr.py` uploads the record and, on a failure or hang, resets
   through WDT0 and runs `--fallback-build` once.
3. **Host checks, implemented.** `checks/ddr-wrapper.py` covers the
   restore path, and `checks/ddr-host-retention.py` covers its runner cases:
   a wrong-board record, a corrupt record, a read-back mismatch and a
   quick-check failure must each stop with their code before any further
   DRAM use and fall back through one WDT0 reset. Hangs, a missing or
   unexplained reset, a failed fallback and a SID mismatch are covered too.
4. **March C-.** Prepare the [memory qualification image](../memory-qualification.md)
   for two board runs: after a P-state-0-only training (the reference
   path's baseline, which has never had a full memory test) and after the
   restore. It needs the user's acceptance of its three residuals before
   `apps/memtest/check.py prepare` runs.
5. **Review and freeze** `dram-restore-test-1` for both boards under the
   design gate, then run it when power cycles are possible.
6. **Card deployment**, only after FEL qualification: the host tools now
   allocate two copies, select the newest eligible generation and plan
   standby updates payload first and header last. The host-built boot0 hook
   and helper plans are implemented and instruction-checked. Remaining
   prerequisites are runtime temperature admission, qualified recovery,
   physical reservations and board-specific backups. Every write
   session still needs a host backup.
7. **Qualification** as listed above: ten restored lifetimes per board with
   March C-, cold and warm lifetimes with stress runs (which also set the
   record's temperature limit and the retraining interval), the fallback
   cases, and boot-time measurement against full training.

**Open risks:** a trained register that equals its untrained value on both
boards (about two expected), registers outside the captured address ranges,
the excluded `0xa0000` block, MR12 and other mode registers that the
message block does not carry, temperature (every capture is a
room-temperature cold start), the P-state-0-only path's untested full
memory behaviour, and the modified boot0 that a card restore needs.
