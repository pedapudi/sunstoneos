# Design: restoring DRAM training from a record on the microSD card

Status: **RAM-only restore implemented and host-checked; no physical restore
has run.** The capture tools, fixed restore set, record builder, SRAM
admission and restore helper are implemented. The card allocation and record
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
  1,008 of them on both. After the review of 2026-10-25 the final set is
  derived by one recorded command over all twenty captures, per board and
  united, with status pooled over both boards and with firmware controls,
  firmware inputs and controller read-result words left out: 1,036
  registers, 527 byte-lane registers, 445 address/command sequencer words,
  21 `0x30000` registers, 20 message-block result fields, 20 parameter words
  and three others. 410 vary between trainings; 626 are fixed by the
  P-state-0-only path. The [first test's runbook](../board-runbooks/dram-restore-test-1.md)
  lists the exclusions and their reasons.
- **The chip IDs differ in their last word only** (`0x40892413` on 3-3,
  `0x40792413` on 3-1). Trained byte-lane values differ between the boards
  by a median of 2 codes, so a record belongs to one board, as the chip-ID
  check requires.

The last reconnect of 2026-10-25 added a four-P-state lifetime per board,
which confirms the status class (`0x0a51022c`, already status on 3-1) and shows
that the `0xa0000` block is mostly stable after four-P-state training but
not after P-state-0-only training. No further lifetime is available, so
the set, 1,036 registers, is fixed from these lifetimes and the first board test waits for
power cycles to return; [its runbook](../board-runbooks/dram-restore-test-1.md)
gives the set's derivation, the build it needs, the predictions, the
qualification ladder and the residual risk. Until cold and warm lifetimes
exist, a record is valid only for room-temperature cold starts.

**Projected DRAM time with a restore:** 40 ms for boot0's
initialization-only path, plus about 1 ms for 1,016 16-bit PHY writes and
their read-back, the 20 parameter words being set before the call (*estimated* at 1 µs per APB write), plus the
20 ms quick check: about 60 ms against 5,093 ms of training today, a saving
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

## Pre-admission rejection trains fully; post-edit failure stops

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
and sampled-memory failures stop the attempt. The FEL wrapper reports the
error through its SRAM mailbox and returns through the normal FEL swap; the
card hook stops in SRAM. Neither path retrains in place. A warm reset has no
qualified recovery destination.

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

The accepted offline evidence is in
`~/.cache/sunstoneos/evidence/dram-card-restore-20261026/`. The experimental
boot0 SHA-256 is
`e4a5ba15cb9f8ae494b5374463e210925496fa363b566ce7f4b86a3db45837ae`;
the default image is
`a31525f3843c2b2e0f051833113e03959cceda51221ddafe910ddb1d3a117117`.
Both load the same 10,472-byte helper, SHA-256
`3e3bc281037d580018429e86293a62757fedcd9e14db2635d7c45885da34c1ae`.
Its single initialized section occupies `0x92000..0x948e8`. Bootstrap sections
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

For the actual record, the first valid helper uses 21 sectors and the two
records use 56 sectors: 39,424 bytes read. A bad first helper followed by a
valid second uses 50,176 bytes. The format caps both-helper/both-record reads
at 226 sectors, or 115,712 bytes. One non-refreshable ten-second counter
budget covers the attempt. Restore performs 1,016 masked PHY writes with
immediate readback, followed by a separate 1,016-read final pass. The memory
sample writes and reads 278,528 bytes each. The instruction fixture caps each
case at five million instructions; the largest observed case uses 4,860,609.
This count is not a hardware performance measure.
The accepted RAM helper rebuilt byte-for-byte identically, so its earlier
physical prerequisites and offline evidence remain applicable.

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
   mismatch and a forced part A failure. Pre-admission rejection must leave parameters
   unchanged and run full training once. Post-edit readback and memory failures
   must stop before caller continuation, with no in-place retraining.
4. **Boot time**: ten restored and ten fully trained boots per board,
   comparing boot0's `DRAM BOOT DRIVE INFO` to `Actual DRAM SIZE` interval.
5. **Card**: host backup, record write through the new region, then the
   card-boot variant repeats steps 2–4.

## Budgets

| Quantity | Budget | Basis |
| --- | --- | --- |
| DRAM interval with restore | 500 ms; prediction about 60 ms | 40 ms measured for the initialization-only path on both boards, plus *estimated* writes and the quick check |
| Helper and record reads | At most 115,712 bytes; one ten-second attempt deadline | Single-sector PIO; physical latency unmeasured |
| CRC over the record | Bounded by the 24 KiB record limit | Instruction-checked; physical cost unmeasured |
| Temperature admission | Default full-training fallback | Runtime measurement and allowed range unqualified |
| Current quick check | 8 KiB sample; 278,528 bytes read and written each | Wider Part A/B checks remain future proposals |
| Hook code | 668 bytes in 1,024 bytes of slots; separate 10,472-byte helper | Eight zero presence words; reserved helper extents |
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

The first implementation admits only the fixed 1,036-entry restore set and
its recorded P-state-0 captures. The builder binds the analysis file by
SHA-256, checks the capture chip ID, training kind and stage tables, and
uses the pinned boot0 parameter set 5 for the strap CRC. Capture words
32–51 are 32-bit values: the record encodes their 40 little-endian
halfwords as a run at `0x83278`, without truncating their upper halves.
The remaining 1,016 entries are masked PHY halfwords. Temperature is
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

A failed write check exits directly to the wrapper's saved SRAM context;
it must not resume boot0 and reach its memory test. After a successful
`init_DRAM`, the wrapper checks the size and performs a bounded uncached
sample test below 4 GiB. This sample is a gross-error check; the separate
March C- qualification remains required. Failures have distinct mailbox
codes for record geometry, header CRC, payload CRC, chip ID, boot0 identity,
strap, PHY readback, size and memory comparison. No failure retrains in
place. The host refuses its RAM page checks on any restore failure.

The budget is one admission pass and one ordered PHY write/readback pass,
with bounded record parsing and no allocation. Ordered same-register
readback follows each write; explicit barriers surround mux transitions.
A second readback pass after
`init_DRAM` detects changes made by boot0 before the sample test. Host instruction checks
exercise the real wrapper/helper with modeled boot0 calls and MMIO. They
require exact fault codes and zero later memory accesses for wrong-board,
CRC, readback and sampled-memory failures. These checks establish software
ordering only; they do not qualify the PHY or authorize a launch.

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
is a record from `from-capture`; the fixed analysis file remains
`restore-set-v3.json`. `plan-standby` takes the same arguments and an
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
records. The observed 13,836-byte records require 28,672 card-image bytes
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
| Restore set | Fixed: `~/.cache/sunstoneos/evidence/dram-training-analysis-20261025/restore-set-v3.json`, command in `restore-set-v3.command` |
| Record format, paired selection and standby plans | Implemented and host-checked; physical card stage not started |
| Record builder from a capture | Implemented; both actual board references and identity refusals checked |
| Restore wrapper variant | Implemented; instruction and host-runner cases checked |
| March C- runs on the restored and P-state-0-only paths | Image implemented and host-modeled, never run on a board |
| First restore test, [runbook](../board-runbooks/dram-restore-test-1.md) | SRAM artifacts built; physical prerequisites remain |

**Facts the next steps rest on**, all measured on both boards on
2026-10-25 except where marked:

- boot0's initialization-only path reaches firmware result 7 in 36 ms and
  the size line in 40 ms; P-state-0-only training takes 4,437–4,451 ms and
  four-P-state training 5,088–5,102 ms.
- The restore set is 1,036 registers: the union of both boards'
  P-state-0-only minus initialization-only diffs, with status pooled over
  both boards and without the `0xa0000` block, firmware controls, firmware
  inputs and controller read-result words; 410 vary between trainings and
  626 are fixed by the P-state-0-only path.
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
   P-state-0-only capture and `restore-set-v3.json`, refuse a capture
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
   after `init_DRAM` a sampled pattern check below 4 GiB. Any failure stops
   and reports in the mailbox; there is no in-place retraining.
   `tools/run-ddr.py` gains a `--record` upload for that variant.
3. **Host checks, implemented.** `checks/ddr-wrapper.py` covers the
   restore path, and `checks/ddr-host-retention.py` covers its runner cases:
   a wrong-board record, a corrupt record, a read-back mismatch and a
   memory-check failure must each stop with their code before any further
   DRAM use.
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
