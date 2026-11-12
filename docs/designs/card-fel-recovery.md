# A retained first-stage request selects FEL before card loading

**Status: request consumption and FEL re-entry qualified from SRAM.** The gate
has executing host checks, and no card is written. One physical run on port
3-3 executed the gate in SRAM through the ROM's FEL execute command. It
consumed the RTC request, recorded its final progress, left WDT0 re-armed and
reached a new FEL instance 0.537 seconds after the execute request. Card
loading, recovery after a watchdog reset through a card and cold boot remain
unqualified ([evidence](../evidence/card-fel-recovery.jsonl)).

## The first stage consumes the vendor request

RTC general-purpose register 2 is `0x07090108`. The vendor FEL request is
`0x5aa5a55a`. U-Boot revision `b791be842935b27268ae3d00e943a9075495f30a`
defines that value and index in `arch/arm/mach-sunxi/rtc.c`. The selected
sun60iw2 headers place the RTC at `0x07090000` and its general-purpose words
at offset `0x100`. `sunxi_board_run_fel()` sets the request with barriers and
readback before cleanup and reset.

The pinned boot0 SHA-256
`829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3`
tests the request in `0x4a990`, reached from main at `0x5e1ae`. Its matching
path clears only index 2 through `0x4aa08`, completes cache cleanup and branches
to ARM ROM `0x20` through `0x48ec6`. The helper's initial loop reads and prints
eight RTC words; it does not clear them.

The early SunstoneOS gate applies the same request before boot0's board setup,
DRAM initialization or storage access. The gate is stackless and
position-independent AArch32 code
([source](../../boot/a733/card-fel-gate.S)). A clear request continues at
boot0's original reset stub, `0x47b00`, and writes no RTC word. The matching
request requires translation and data caches off and the known clear ROM
redirect words. The gate then disables and invalidates the instruction cache,
checks a recognized watchdog state and applies its WDT0 policy (below) with
readback. It then clears the request with barriers and readback, invalidates
the branch predictor and branches to ARM ROM `0x20`. A policy readback refusal
keeps the request pending and parks with WDT0 still armed. An unconfirmed
clear enters FEL with a failure code. The gate changes no voltage, clock or
secondary power policy.

The WDT0 policy is the 32-bit word that immediately follows the gate code; the
code loads it PC-relative, so every profile runs identical code bytes. Zero
disarms WDT0: the card first stage appends zero, because FEL must not reset
under a watchdog. A nonzero value is a WDT0 mode that the gate writes with the
key and then restarts through the control register (A733 manual §8.3.6.4,
key `0xa57`). The FEL proof uses `0xb1`: enabled, 16 seconds, the largest
interval (§8.3.6.6). Before a nonzero policy, or with WDT0 already enabled,
the gate requires system-reset configuration and reset output.

On the requested path the gate records its progress in the RTC LAST word,
`0x07090104`, as stage `0x0c02` in the high half and a value in the low half
([codes](../../include/a733_card_fel_gate.h)). Each store is followed by a DSB.
The gate leaves the fault, ESR, ELR and FAR words unchanged.

| Value | Meaning |
| --- | --- |
| `1` | Request matched; gate entered |
| `2` | Watchdog mode, configuration and output checked |
| `3` | WDT0 policy applied and read back (mode zero for the card, `0xb1` for the proof) |
| `4` | Request cleared and read back as zero |
| `5` | Before the branch to ROM `0x20` |
| `0x81` | Refused: MMU or data cache enabled |
| `0x82` | Refused: a ROM redirect word is nonzero |
| `0x83` | Refused: unrecognized watchdog mode, configuration or output |
| `0x84` | Request readback nonzero after the clear; FEL entered anyway |
| `0x86` | Refused: WDT0 mode readback differs from the policy |

The captured ROM's entry at `0x20` bypasses BOOT sampling but still checks the
second software redirect at `0x070501d4`. All redirect words therefore remain
zero. The ROM consumes no RTC request itself; the gate makes the selection.

## A warm reset cannot enter the gate

The A733 manual places CPU_SUBSYS_CTRL at `0x08000000` (§5.1.5).
Its CPU0 registers have separate execution-state and reset-address roles:

| Register | Address | Documented role |
| --- | --- | --- |
| CLU0_CPU0_CTRL_REG | `0x08001000` | Bit 0 selects AA64NAA32 at processor reset (§5.1.6.6) |
| CLU0_RVBARADDR0_L | `0x08001004` | Low reset-address bits for AArch64 execution (§5.1.6.7) |
| CLU0_RVBARADDR0_H | `0x08001008` | High reset-address bits for AArch64 execution (§5.1.6.8) |

The Cortex-A55 TRM §B2.93 defines `RVBAR_EL3` as read-only, derived from the
RVBARADDR inputs and describing AArch64 reset entry. The AArch32 reset vector
instead depends on `SCTLR.V` and VINITHI (§B1.81). The pinned vendor BL31, the
TF-A A733 port and SunstoneOS's `cpu_start.c` all select AArch64 before using
these registers. An AArch32 warm reset therefore cannot be directed to a gate
in DRAM, and the RAM proof uses no reset.

## The RAM proof executes the gate from SRAM

On a card, the ROM loads the first stage into SRAM and runs it in AArch32 with
the MMU off. The ROM's FEL execute command does the same for an uploaded image.
A measured SRAM probe executed through that command at `0x52000` ran in
AArch32 supervisor mode with SCTLR `0x00c51838`: MMU and data cache off,
instruction cache on ([SRAM probe result](../boot.md#sram-probe-result)).
The proof therefore uploads the gate to `0x52000` and executes it there.

The proof ELF ([sources](../../apps/card-fel-recovery/)) contains two read-only
loads. The first holds the exact gate code at `0x52000` followed by the policy
word `0xb1`. The second is a 96-byte profile at `0x52400`. The profile names
its version, the gate address and code length, the RTC stage `0x0c02`, the
final progress value 5, the WDT0 policy and the code's SHA-256. The runner's
admission ([tools/card_recovery.py](../../tools/card_recovery.py)) requires an
AArch32 executable entered at `0x52000`, loads only inside
`0x52000..0x52d80`, code matching the profile hash, and a policy word equal to
the profile's `0xb1`. The packaged card first stage contains the same code
followed by policy zero. The profile has no storage, DRAM, clock or rail
authority, and the runner initializes no DRAM.

The runner sequence is as follows:

1. Reset the board to a fresh FEL instance and upload both loads with the
   tool's `write` command.
2. Clear the crumb words and arm WDT0 in mode `0x41` (a four-second interval).
3. Write the session tag to `0x07090100` and the request to `0x07090108`, and
   read both back.
4. Execute `0x52000`, ending that transaction after at most 2 seconds.
5. Wait for a new FEL instance, read the request word, then read WDT0, disarm
   it and read the crumb words in one command.

The `write` command leaves the instruction cache enabled. The reviewed FEL
tool, built with the A733 `icache_fix` patch, runs a helper before every
register read or write command: it clears `SCTLR.I`, invalidates the
instruction cache and returns. The request-arming command in step 3 is the
last command before the execute command, so instruction fetch from the
uploaded gate starts with the cache off. The gate also clears `SCTLR.I` and
invalidates the instruction cache itself. The runner refuses any FEL binary
other than the reviewed one and runs a frozen copy.

The gate's branch to `0x20` re-enters the ROM from inside the execute command.
On the board the ROM sent that command's status before running the code: the
transaction completed normally in 0.013 seconds. If the status is lost to the
disconnect instead, the transaction fails or the runner ends it after
2 seconds. In the captured ROM,
the paths from `0x20` and from the reset vector at `0x44` meet at the same FEL
routine (`0x2514`, called with `r0 = 0x7f`). Both then run the same clock and
USB initialization and the FEL loop at `0x1061c`. The last run of the earlier
DRAM-based recovery image (queue entry `1794485680312692366-e77f3e34`)
measures that path: after an AArch32 warm reset with USB still configured, the
next command reached a new instance 1.79 seconds after the launch, and WDT0
still read `0x41`, so no watchdog reset had occurred. The runner therefore
accepts a normal completion and also treats a failed execute transaction, or
one it ends at 2 seconds, as the expected disconnect. It accepts that only for this execute command, after the
command has started, with the watchdog and request armed. It sends no further
request to the old instance and waits for a new one.

The proof policy keeps the board recoverable when the ROM does not
re-enumerate. WDT0 then expires 16 seconds after gate entry and resets the
board to FEL, the path every returning image takes. The RTC words survive that
reset. The runner tells the two outcomes apart by the WDT0 mode it reads first
and by the time from the execute request to the new instance:

| Outcome | WDT0 mode read first | Instance after execute | Runner result |
| --- | --- | --- | --- |
| ROM re-enumerated | `0xb1`, still counting | under 16 s | `returned`; WDT0 disarmed over FEL |
| No re-enumeration | `1`, the reset remnant | 16 s or more | `watchdog-reset`; proof failed |

Acceptance requires the first row together with the request zero, LAST
`0x0c020005`, the matching session tag, no fault word and WDT0 mode zero after
the runner's disarm. Any other receipt fails the proof; a normal return with an
invalid receipt quarantines the instance. The ROM's FEL entry clears SRAM
`0x44000..0xf4000`, so the runner reads no SRAM after the return.

The host checks execute the actual gate code at `0x52000` and at the card
location `0x83000` with both policies, the instruction cache on and off, and
the packaged card sector (`checks/card-fel-recovery.py`). They cover each
refusal, both readback failures, omitted stores, the cold path and the tool's
cache helper. `checks/board-runner.py` models the execute disconnect, the
bounded transaction, the ordering of uploads, arming and execution, both
outcomes in the table and each invalid receipt.

The session budget is 60 seconds, with `finish_by` 55 seconds and a 3-second
native progress window. The worst-case allocations below take their bases from
the run named above.

| Stage | Re-enumerated (s) | Not re-enumerated (s) | Basis |
| --- | --- | --- | --- |
| Preparation and reset to fresh FEL | 15 | 15 | Measured 6.0 |
| Upload | 1 | 1 | Measured 0.01 per load |
| WDT0 and request arming | 0.5 | 0.5 | Measured 0.05 |
| Gate execution | under 0.001 | under 0.001 | About 120 instructions |
| Execute transaction end | 2 | inside the WDT0 interval | Runner limit |
| WDT0 interval from gate entry | none | 16 | Policy `0xb1` |
| ROM to a usable new instance | 5 | 5 | Measured 1.79 after re-entry, 1.9 after a WDT0 reset |
| Capture and WDT0 disarm | 1 | 1 | Measured 0.03 |
| Total | 24.5 | 38.5 | |

The re-enumerated path leaves 30.5 seconds before `finish_by`. The other leaves
16.5 seconds before `finish_by` and 21.5 seconds in the session. In the
re-enumerated path the runner must disarm WDT0 before the 16-second interval
ends: execute end, enumeration and capture take at most 8 seconds, a margin of
8 seconds. The initial `0x41` interval covers arming and execution, about
0.05 seconds of 4 seconds. The gate records its last progress within
microseconds, well inside the 3-second window. These are test budgets, not
measured cold-boot latency.

## Integration after the proof

Autonomous card boot is permitted only after a confirmed cold power-on. Every
warm reset, including watchdog expiry, must enter FEL unless the OS explicitly
requests a card reboot. That request must authorize one boot and be consumed
before the OS starts. An absent FEL request does not authorize card boot.
Without an explicit reboot request, an unknown reset cause must select FEL,
so a hung OS cannot create a card-boot loop.

The A733 has no retained hardware reset cause. The manual's PRCM register
inventory (§4.2.4) and RTC register inventory (§9.1.6) list none.
RTC `0x07090160[13]` reports the current reset level (§9.1.7.16), and
`0x070901f4[3]` enables the POR reset output (§9.1.7.21); neither records the
preceding reset. WDT `0x02050004[0]` is an interval-interrupt pending bit, and
RTC `0x07090304` is ROM software progress;
[the reset audit](../watchdog-reset-evidence.md) explains those exclusions.
The pinned U-Boot's `board_sun60iw2.c` issues the watchdog software-reset
command without reading a cause latch. Its AXP8191 driver decodes PMIC register
`0x50[2:0]` as supply-rise, button and IRQ power sources, but the inspected
code does not establish that field's retention or clearing, and the gate
cannot reach the PMIC before boot0 initializes the TWI bus.

The vendor kernel detects a cold boot with a software marker instead. Its RTC
driver (`bsp/drivers/rtc/rtc-sunxi.c` at board-kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, lines 1018..1041) reads an RTC
general-purpose word: the reset value zero means the RTC domain lost power,
and the driver then stores a nonzero boot reason. The manual states the basis:
RTC registers below offset `0x300` lie in the VDD_RTC power domain
(§9.1.6 note), and their contents persist while VDD_RTC stays above 0.7 V
(§9.1.4.4). A system reset, including a WDT0 reset, does not reset them; the
breadcrumb words at `0x07090100..0x0709011c` survive every recorded watchdog
return.

SunstoneOS uses the same principle with a register that no other code touches.
IC_CHARA_REG at `0x070901f0` (§9.1.7.20) is in the VDD_RTC domain and resets
to `0x00000000`. A write takes effect only when bits 31:16 hold the key
`0x16aa`; bits 15:0 then read `0x16aa`, and otherwise read zero. The
predicted full readback after a keyed write is `0x16aa16aa`; the predicate
tests bits 15:0 only. The captured
ROM accesses only RTC offsets `0x120`, `0x15c`, `0x160`, `0x16c`, `0x304`,
`0x310` and `0x328`, and the pinned boot0 holds no literal for `0x070901f0`,
so neither changes the marker.

The cold-boot predicate is therefore `0x070901f0[15:0] == 0`:

| Reset kind | Predicted `0x070901f0` at gate entry | Gate decision |
| --- | --- | --- |
| Power-on after VDD_RTC fell below 0.7 V | `0x00000000` | Cold: set the marker, continue to boot0 |
| WDT0 reset after the marker was set | `0x16aa16aa` | Warm: FEL |
| ROM `0x20` re-entry or FEL launch | Unchanged; the gate does not run | None |
| Power interruption too short to discharge VDD_RTC | `0x16aa16aa` | Warm: FEL |
| Any other value | Unknown | FEL |

The marker must be set in every power epoch before the first reset that can
reach the card. The cold-path gate sets it. The runner must also set it at its
first FEL contact, because a board that powers up in FEL never runs the gate;
without that write the first watchdog return would read zero and boot the card.
A misclassification in the safe direction, a cold power-on read as warm, enters
FEL. A warm reset read as cold requires the marker to be cleared without a
VDD_RTC power loss.

Two parts of the predicate remain unconfirmed on the board: the keyed readback
value and its retention across a WDT0 reset. The board also has no measurement
of whether removing USB power discharges VDD_RTC; the 2026-10-31 replug left
the PMIC rail selectors unchanged ([watchdog.md](../watchdog.md)).

With the cold-boot predicate, a warm reset enters FEL without a request. The
vendor request remains the path for software that runs after a cold decision.
A card-aware OS must set it before an expected watchdog stop
and verify the write before parking. Its admission and renewal predicates must
name that recovery policy and the exact request value. Existing zero-request
contracts remain distinct. The RAM proof does not implement or qualify those
OS contract changes.

The early gate is packaged with the pinned boot0 by changing only its initial
branch, header length/checksum and appended gate sector. The normal path keeps
the vendor setup and loader. Its stock package location remains unchanged;
the FAT32 loader and below-sector-2048 installation are subsequent work.
The gate occupies 472 bytes of code and a 4-byte policy word; the packaged first stage is 246,272 bytes
(sectors 16..496). The current installer's 480-sector boot0 range ends at
495 and does not authorize that extra sector. The installation plan below
replaces it with a boot-area write class.

The captured ROM admits the new aligned length `0x3c200` under its
`0xa7800` limit. Its conditional checksum path does not establish universal
authentication of appended bytes; the host producer independently checks the
pinned original and the complete derived checksum. The source identities and
offsets are in [the recovery inventory](../vendor-firmware/data/card-fel-recovery.json).

RTC retention requires the manual's RTC supply condition, `VDD_RTC > 0.7 V`.
The RAM proof establishes request consumption and direct FEL entry. It does
not establish watchdog-reset-to-card execution, power-loss retention, recovery
from an inaccessible fabric, or autonomous cold boot. Each remains a separate
physical qualification requirement.

## Installing the first stage on the port 3-3 card

**Status: plan only.** No step below has run. Each board step needs the
coordinator's approval, and the write needs a storage-write approval record.
The card on port 3-3 is the scratch card with CID
`2750485344333247120000029d014c47`. Its MBR holds one FAT32 partition from
sector 2048 for 61,065,216 sectors, so sectors 1..2047 lie outside every
partition.

### Code that must land before the first board step

- **Gate revision with the cold-boot predicate.** A request still selects FEL.
  Without a request, `0x070901f0[15:0] == 0x16aa` selects the warm path, zero
  selects the cold path, and any other value selects FEL. The warm path
  disarms WDT0 and branches to ROM `0x20`. The current gate refuses and parks
  when WDT0 mode is nonzero and the configuration or output words differ from
  `1` and `2`. WDT0 mode reads `1` after a watchdog reset, and the
  configuration and output words after a reset are unmeasured, so the current
  gate could park a board after a watchdog reset with no USB device. The
  revision checks those two words only before arming WDT0; a disarm needs
  neither. The cold path writes `0x16aa16aa` to `0x070901f0` and reads it back,
  arms WDT0 at 16 seconds, and continues at boot0's reset stub `0x47b00`.
  If boot0 then hangs, the watchdog reset reaches the warm path and FEL.
  A refusal after a reset enters FEL instead of parking.
- **Gate record.** Both paths leave the LAST and fault words unchanged, so a
  hung image's breadcrumbs survive the reset. The gate records in RTC
  general-purpose register 2 (`0x07090108`): `0x0c02` in bits 31:16, bit 15
  set once a cold path has run in this power epoch, bit 14 set when the
  latest entry took the warm path, and the WDT0 mode read at entry in bits
  7:0. The record never equals the request value `0x5aa5a55a`.
- **Runner reads and marker.** At first FEL contact, the runner reads
  `0x070901f0`, `0x07090160`, `0x07090108` and WDT0 configuration, mode and
  output (`0x02050010`, `0x02050014`, `0x02050018`). When
  `0x070901f0[15:0]` is zero it writes `0x16aa16aa` and reads it back. It reads
  the same words after every return, records them, then clears the gate
  record. A watchdog return takes its WDT0 mode from the gate record, because
  the gate disarms WDT0 before the runner reads it.
- **Boot-area storage admission.** `tools/storage_bounds.py` admits only
  ranges inside the FAT32 partition. A boot-area class admits one write range
  equal to the first-stage extent named by `first-stage.json`, ending below
  both sector 2048 and the partition start, with backup ranges `[0,1]` and
  `[16,496]` (sectors 16..511) and an approval record that names the candidate's SHA-256.
- **Raw-extent write application.** A new mode of `apps/storage-write-test`
  takes the bounds blob, the mode word and the candidate sectors as inputs.
  It calls `ss_storage_enable`, which re-reads the backup ranges and refuses
  unless their SHA-256 equals the blob's. It writes the extent through the
  OS SD owner in ascending CMD25 pieces of at most 256 sectors, reads the
  extent back, compares SHA-256 with the candidate, and mounts the FAT32
  partition without writing before it closes the card, as
  `include/sunstone_storage.h` requires after raw device writes.

The first-stage extent depends on the gate size. A gate and policy word of at
most 512 bytes give 481 sectors, 16..496; at most 1,024 bytes give 482 sectors,
16..497. The current 476 bytes give 16..496. The backup covers 16..511, so it
serves either size.

### Board steps

| Step | Kind | Card access | Prediction |
| --- | --- | --- | --- |
| A | Predicate reads on one returning image that ends in a WDT0 reset | None | `0x070901f0` reads `0` at first contact (no writer exists) and, with the marker write approved, `0x16aa16aa` after the write and after the watchdog return; `0x07090160[13]` reads `1` both times |
| B | SRAM proof of the revised gate's warm path through FEL `exe`, marker set, no request | None | New FEL instance within 1 s of the request; gate record bit 14 with mode `0xb1`; WDT0 re-armed to `0xb1` under the proof policy; LAST unchanged |
| C | Read-only capture with `apps/sd-backup-probe`, ranges `0:1 16:496 2048:1 24576:1 32800:1` (500 sectors) | Read | Sector 0 and sector 2048 equal the registered restore point; sectors 24576 and 32800 lack the TOC1 magic `0x89119800` at byte 16 |
| D | Raw-extent write with `--storage-write` | Write 16..496 | Readback SHA-256 equals the candidate's; sector 0 unchanged |
| E | One returning image that ends in a WDT0 reset | None | The cold-predicate gate's warm-path record below |

Step C writes the capture to
`~/.cache/sunstoneos/build/card-boot-1111/card-backup/3-3-boot-area.bin`
(500 records of a little-endian 64-bit sector and its 512 bytes), with a
manifest listing the SHA-256 of the file, of the 16..511 records and of each
single-sector record. The approval record's backup file holds the 497 records
of sectors 0 and 16..511, the backup ranges that the OS re-reads. Sector 0 must match the restore point before the
approval record is written. If sector 24576 or 32800 holds a valid TOC1 header,
installation stops, because the cold path would hand off to that package.

Step D writes exactly one extent:

| First sector | Sectors | Bytes | Contents | Restored from |
| --- | --- | --- | --- | --- |
| 16 | 481 | 246,272 | Pinned boot0 with the patched branch, length and checksum, then the gate sector | Step C records 16..496 |

A gate over 508 bytes changes the row to 482 sectors and 246,784 bytes,
restored from records 16..497.

The OS verifies the backup digest before its first write, so a card that
changed after step C is refused. The session needs about 1 s for the card
opening, under 50 ms for the two CMD25 pieces and under 10 ms for the
readback, within the 60-second session and its 55-second finish.

### Predicted boot records

The ROM's card loader finds a valid boot0 header at sector 16 and copies
`0x3c200` bytes to `0x47000`. The patched branch enters the gate at
`0x83000`.

- **Watchdog return** (marker set by the runner): the gate takes the warm
  path in microseconds and enters FEL. The runner then reads gate record
  `0x0c024001` (bit 14, WDT0 mode `1`), WDT0 mode `0`, `0x070901f0` =
  `0x16aa16aa`, and the image's own LAST and fault words. The ROM's card read
  of 246 KB adds an unmeasured delay to the 1.79-second reset-to-instance time
  recorded for a ROM re-entry; at 4-bit 25 MHz it is about 20 ms, and step E
  budgets 6 s.
- **Cold power-on** (VDD_RTC discharged): the gate reads zero, sets the
  marker, arms WDT0 for 16 s and continues into boot0. boot0 initializes
  DRAM and looks for a TOC1 package at sector 32800 and then 24576. Neither
  holds one, so boot0 prints `Loading boot-pkg fail`, waits 10 ms and branches
  to ROM `0x20` (`829ea8e3@0x5e1b8..0x5e1d2`), which enters FEL. If the runner
  does not disarm WDT0 within 16 s, the reset reaches the warm path and FEL.
  The record then holds bit 15, and bit 14 if that reset occurred.
- **Power-on with VDD_RTC still above 0.7 V**: the warm path, as for a
  watchdog return.

### Restoring the card over FEL

Every predicted path ends in FEL, so restoration needs no card removal. From
any FEL instance, a read-only capture of `0:1 16:496` confirms the installed
bytes, and a raw-extent write with the step C records 16..496 as the candidate
returns the extent to its original bytes; its readback must equal the hash of
those records. If sector 16 held no boot0 header before the install, the ROM
then finds no boot0 and enters FEL, as it does today.

One failure remains outside this procedure: a defect that stops both the
warm path and the cold path before FEL. The board then presents no USB device
until the card is removed. Step B executes the revised gate's code bytes on
the board from SRAM, and the host checks execute the packaged sector at
`0x83000`, before step D runs.
