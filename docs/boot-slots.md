# Two boot slots keep a verified previous image on the card

A SunstoneOS microSD card holds two boot slots, A and B. Each slot contains a
runtime image, up to three assets such as mimus models, and a one-sector header
that records their card extents, load addresses and SHA-256 hashes. A stage-2
loader in the boot0 monitor payload reads both headers, copies the newest slot
whose hashes match into memory and reports the outcome. An update writes the
standby slot and commits its header last, so an interrupted update leaves the
previous slot selectable.

Status: the card layout, the host tool [boot-slots.py](../tools/boot-slots.py)
and the loader in [boot/a733/stage2](../boot/a733/stage2) are implemented and
pass host checks. No card has been written and no board has run the loader.
The loader admits the SD controller state that boot0 leaves after reading the
card. It checks that state against the pinned boot0 and the A733 manual; host
models cover the check.
The loader's entry path, which arms the watchdog, builds the resident contract
of the [integrated OS image](os-image.md) and branches to it, is implemented
and host-modeled. Remote-writer, CPU/cache, local-entry and reset-policy checks
are implemented. The supported cold route requires the caller-owned predecessor
contract; an arbitrary warm or FEL launch is unsupported. Physical cold boot
and reset coverage remain unqualified. The
[boundary section](#the-loader-stops-before-entering-the-image) lists what each
prerequisite needs.

## Card layout

Sector numbers are 512-byte card sectors.

| Sectors | Contents | Owner |
| --- | --- | --- |
| 16..495 | Pinned boot0 | Container manifest `card_writes` |
| 8192 | Slot A header | Fixed by this layout |
| 16384 | Slot B header | Fixed by this layout |
| 32800.. | Monitor TOC1 package, at most 8,160 sectors | Container manifest `card_writes` |
| 40960.. | Slot payload regions and existing partitions | Allocation record |

The first 20 MiB (sectors 0..40959) form the boot area that the
[microSD packager](microsd-boot-package.md) describes. The header sectors lie at
4 MiB and 8 MiB. Each sits in a separate 4 MiB allocation unit, apart from boot0,
the unpopulated backup TOC1 at 12 MiB and the TOC1 at 16 MiB, so programming
one header does not share a card erase unit with them. A card install writes
only the boot0 and TOC1 extents listed in the container manifest; the zero gaps
of the 20 MiB container file would otherwise erase both headers.

Slot payload regions start at or beyond sector 40960. An allocation record,
described with the [install procedure](#install-update-and-rollback), assigns
each region explicitly. The tool refuses regions that overlap the boot area,
each other or any primary MBR partition, and regions that are not whole 4 MiB
units. Cards with a GPT are refused, because boot0 at sector 16 overlaps the
GPT entry array. `allocate --training-record FIRST:BLOCKS` also assigns one
4 MiB unit to the DRAM training record
([restore design](designs/dram-training-restore.md)), under the same rules.
The paired form uses `--training-record-a FIRST:8192` and
`--training-record-b FIRST:8192` together. It records `training_records`
with A and B extents and rejects overlap between them. The paired and
legacy forms are mutually exclusive; existing allocations keep their format.
A produced allocation describes the supplied backup file. Physical use still
requires that card's preserved full backup and reviewed reservation.

## Slot header

The header is little-endian and one sector long. Its last 32 bytes are the
SHA-256 of bytes 0..479.

| Offset | Field |
| --- | --- |
| 0 | Magic `0x41533733534c4f54` (`AS73SLOT` read as big-endian bytes) |
| 8, 12 | Version 1; slot index, 0 for A and 1 for B |
| 16, 24 | Generation, nonzero; its bitwise complement |
| 32, 36 | Extent count, 1..4; flags, zero |
| 40 | Entry address, 4-byte aligned, inside the runtime image |
| 48..63 | Reserved, zero |
| 64 + 64*i | Extent i: first sector, byte length, load address, kind, reserved word, SHA-256 |
| 320..479 | Reserved, zero |
| 480..511 | SHA-256 of bytes 0..479 |

Extent 0 has kind 1 and holds the runtime image; extents 1..3 have kind 2 and
hold assets. Unused extents are zero. Every used extent must start at or
beyond sector 40960 and end within the card. Its load range must lie in
`0x41000000..0x47000000`, below the monitor reservation that holds the loader.
Within one header, extents may not overlap on the card or in memory.

## The loader selects the newest verified slot

The loader first requires separate remote-writer and CPU/cache ownership
admission. Both currently refuse without the missing cold-chain evidence.
The [cold-entry gate](cold-entry.md) establishes local EL3 state with
translation and caches off; it does not establish exclusive memory ownership.

After those prerequisites, the existing [SD core](../drivers/storage/sunxi_mmc.c)
identifies the card at a 400 kHz module clock. Its high-speed profile reads
SCR, selects four-bit operation with ACMD6, checks and selects high speed
with CMD6, then uses a 100 MHz module clock and a 50 MHz card clock at 3.3 V.
The loader supplies no voltage-switch callback and compiles out UHS-I.
The adapter permits read commands and refuses the write direction and FIFO
writes. It shares module encoding and exact DMA descriptor checks with the
runtime platform through [a733_mmc_read.h](../include/a733_mmc_read.h).

Identification has a two-second deadline, each read has 500 ms, and the
entire load has 30 seconds. The loader also refuses a backward counter or
4,096 consecutive samples with no raw counter progress. Admission settling,
clock changes and final controller stop use the same lifetime bound.

1. Read both header sectors and check each header against the rules above.
2. Order valid headers by generation, newest first. Equal generations try A
   first.
3. Read each extent in requests of at most 256 sectors (128 KiB), then hash
   the destination bytes. Aligned full sectors go directly to the destination.
   Headers, unaligned destinations and the last partial sector use a private
   bounce buffer. Padding never overwrites bytes beyond an extent.
4. A digest mismatch after clean read completion tries the other valid slot.
   A transfer fault retains the descriptors and borrowed destination and parks.
5. Before the [entry path](#the-entry-path-builds-the-resident-contract) or
   ROM return, require the core to be READY without a borrowed buffer or DMA
   ownership. Verify controller and IDMAC idle, stop the card clock through
   the existing UPDATE command, then gate only the SDC0 module. A failed stop
   parks without reset, fallback or image entry.

The report holds status, detail, selected slot and generation, card capacity,
start and end counter values and the last SD core result. Per slot it records
the header state, generation, load state and failing extent.

| Status | Meaning | Detail |
| --- | --- | --- |
| 1 | A slot was copied and verified; the image was not entered | Entry address |
| 2 | Card admission or identification failed | SD core error |
| 3 | Neither header is valid | SD core error |
| 4 | Every valid slot failed to load | SD core error |
| 5 | The system counter does not advance; the card was not accessed | Zero |
| 6 | The resident contract was published and the image entered | Entry address |
| 7 | Session extent size refused (1); base identity/header refused (2); recorded profile/epoch refused (3); SID differs (4); CPU grant refused (5) | Refusal number |
| 8 | WDT0, the ROM redirect words or the RTC FEL flag was not in the cold state, or arming did not read back | Zero when the loader did not enable WDT0 mode; otherwise the arming counter value |
| 9 | Remote-writer ownership refused before SD access | Unproven-master mask |
| 10 | CPU/cache ownership refused before SD access | Zero |
| 11 | SD ownership remains retained; the loader parks | Original loader status; SD diagnostic fields remain available |
| 12 | CPU static-OFF ownership remains retained; the loader parks | Original loader status; no ROM return |

Report words at offsets 320 through 360 hold the selected CMD6 timing,
configured card rate, completed read count, completed bytes, clean-stop flag
and DMA ownership-barrier count. Configured frequency is not a measurement.

The loader never writes the card. Fallback covers invalid headers and digest
mismatches after completed reads. It cannot detect an image that verifies and
then fails while running; that case needs the rollback procedure below.
Exceptions retain the cold-entry report and park.

## Cold SD admission starts from boot0's controller state

boot0 reads the monitor package from the card through SDC0 immediately before
the cold entry. No SDC0 teardown was found on boot0's handoff path: its call
at `0x5e35c` reaches a stub at `0x49bbc` that returns 0. The controller, its
clock and its pins are therefore expected to remain in boot0's transfer
state. Admission does not depend on that observation, because it reads the
bus and clock registers and refuses a closed bus or a gated clock.
`a733_stage2_card_admission` in
[admission.c](../boot/a733/stage2/admission.c) runs once, before the SD core's
first controller access, and compares that state with the pinned boot0 and the
A733 manual. It refuses unless every check below holds.

| Checked state | Expected value | Source |
| --- | --- | --- |
| SMHC0 bus register `0x02002d0c` | Gate passing (bit 0) and reset deasserted (bit 16) | boot0 resource setup at `0x5d60c`; manual 4.1.6.129 |
| SMHC0 clock register `0x02002d00` | Running (bit 31), a documented source, reserved bits zero | boot0 clock setup; manual 4.1.6.128 |
| PF0..PF5 | Function 2 (SDC0) with pull-up | boot0 header storage pin list |
| PF6 | IO-disabled reset value, or an input; no pull or pull-up | Manual 18.14.6.68; boot0 does not configure PF6 |
| PF bank voltage | 3.3 V in `GPIO_POW_MOD_SEL`, `GPIO_POW_VAL` and `GPIO_POW_VAL_SET_CTL` | Manual 18.14.6.1 to 18.14.6.3; same rule as the native SD platform |
| SDC0 controller | No command start bit, no busy state, internal DMA disabled | Manual 11.3.8 |
| Clock parents | Normalized 24 MHz reference, stable unmodulated PERI0_400M branch and effective gates | Existing runtime SD provider and [clock reference](../include/a733_clock_reference.h) |
| SDC0 interrupt | Disabled, inactive and no pending interrupt; distributor not updating | Existing runtime SD provider |

The SD core assumes a 400 kHz module clock during identification, and
boot0 leaves the module at its faster transfer rate. PF6 is IO-disabled, so it
cannot report card detection. Admission therefore makes two kinds of write,
both to registers that serve only SDC0:

1. PF6 receives its pull-up, then becomes an input.
2. The module clock is gated, set to the 24 MHz oscillator divided by N = 2
   and M = 30, then enabled again. The native SD platform changes this clock
   in the same order. Each write is read back, and a mismatch refuses.

Admission then waits 1 ms for the pull-up to settle and requires PF6 to read
low, the active-low card-detect level ([SD presence](mmc.md)). The card sees
a pause and a slower clock while its bus is idle; the SD core's first command
is CMD0.

Before every SD core poll, `a733_stage2_card_present` repeats the bus, clock,
PF0..PF6 configuration, bank voltage, clock parent and interrupt checks against
the selected values, then samples PF6. Any change, or a card that leaves, freezes the SD core with
an admission fault.

Two conditions have no register to check, and neither is verified.

- **Card supply.** Reading the PMIC needs bus transactions, so the supply has
  no read-only observation. The only evidence is that its rails served
  boot0's read of this card. boot0 is configured for the AXP2202 PMIC and may
  program its rails. The pinned boot0 header holds `power_mode` 1. The pinned
  board configuration (`sys_config.fex`, `[target]` comment) reads the low
  three bits of that byte as `axp_id`, where 1 selects the AXP2202, while
  U-Boot's older generic `private_boot0.h` reads 1 as no PMIC. The boot0
  binary contains PMU driver code for the DCDC, ALDO and CLDO rails. The card
  rail state after boot0 needs a board observation.
- **Exclusive use of SDC0 and its memory.** The package holds no other image
  and its U-Boot and OP-TEE pointers are zero. This does not prove that other
  Arm cores are off or that dirty shared-cache lines are absent. Separate
  CPU/cache and remote-writer admission must establish those conditions.

A board run is also needed to confirm the boot0 module clock value, which the
models represent with an assumed transfer rate.

The high-speed clock change follows the existing runtime sequence: the core
stops the card clock and waits for UPDATE completion before the adapter gates
the SDC0 module, changes its source/divider and enables it. Each write is read
back. Shared parent clocks, bus gates, reset and supply rails remain owned.

The linked loader reserves 33 descriptors (528 bytes) and a 128 KiB bounce
buffer, both aligned to 64 bytes inside its existing 4 MiB monitor reservation.
The fixed 32 KiB code boundary and report addresses remain unchanged. With
cold admission linked, the SD change adds 3,480 text bytes and 127,600 BSS
bytes. Each request validates at most 33 descriptors, reading their four
fields; it adds no per-block translation or cache-maintenance loop. The shared
helper extraction adds 32 bytes to the compiled runtime provider and one
function call per submitted DMA command, with the same descriptor reads.

A 15 MiB contiguous payload needs 120 maximum-size reads instead of 3,840
4 KiB reads. At the configured 50 MHz four-bit rate, its data-only bus time
has a 0.63-second lower bound. Command, card, polling and uncached SHA-256
costs are additional and unmeasured. The 30-second limit bounds failure; it
does not predict throughput. CPU caches remain disabled. A practical cold
boot still requires resolved CPU/cache ownership, physical DMA qualification
and measured end-to-end loading time.

## The loader stops before entering the image

Before card access or slot copies, [admission.c](../boot/a733/stage2/admission.c)
checks remote-writer exclusion and CPU/cache ownership. An early remote refusal
returns to forced FEL before SD acquisition. Once CPU ownership has changed,
failure retains that ownership and parks.

- **Remote writers.** The native read-only checker reports a mask of agents
  whose exclusion is unproven. The A733 device inventory excludes OR100; its
  historical bit 1 remains reserved. The generic provider requires both CE resets
  asserted. This cold monitor also accepts the reviewed no-submission predecessor
  contract after SID, eGON, media and BOOT mismatch checks, preserving every
  other writer bit. This depends on a trusted fixed package and caller-owned
  initial quiescence, not a session Boolean or late idle sample. See the
  [ordinary SD entry contract](remote-writers.md#ordinary-nonsecure-sd-boot-has-a-caller-owned-entry-contract).
- **CPU/cache ownership.** The loader identifies each secondary PPU, admits
  the fixed OFF policy and clear start requests, then waits for acknowledged
  static OFF. It retains that policy through loading and cleans the documented
  CPU/DSU hierarchy before DMA. The resident OS publishes each target's entry
  before restoring dynamic policy. A policy-write failure retains ownership
  and parks; an OFF sample alone never admits memory. See the
  [cold CPU contract](designs/cold-cpu-static-off.md). This protects subsequent
  stage2 loads, not monitor bytes already loaded by boot0; the pinned
  predecessor and prior remote-write contract remain separate prerequisites.

Before OS entry, the loader checks the local EL3/core-0/A55 state, masked
interrupts, disabled data cache and translation, and the counter-frequency
contract. The reset policy requires disabled WDT0, zero ROM redirect words
and a zero RTC FEL flag. Normal card boot permits the BOOT input to select
storage. These readbacks establish the selected reset policy; physical
watchdog expiry and successful card reboot remain unqualified.

## The entry path builds the resident contract

With the prerequisites granted, the loader enters only the integrated OS
image. Its slot carries the OS session as an asset loaded at `0x41f00000`;
the slot header's extent digest has already authenticated it. The session is
808 bytes, 904 bytes for recorded workload 2 with a CPU grant, or 992 bytes
in the microphone build. The recorded profile's input epochs are zero; its
board and optional grant generation both equal the selected slot generation.

1. Refuse with status 7 unless that extent exists with one of these three
   sizes, the session's board magic, version 2, size, nonzero lease and
   generation equal the slot's, and its OS magic, version and size field
   match, the size field being equal to the extent. Recorded workload 2 also
   requires a zero board epoch, an SID authority and the SHA-256 of the four
   live SID identity words. Its 904-byte form requires a valid CPU grant,
   zero grant epoch, matching generation, both clusters and a bounded change
   count. A 904-byte session with another workload refuses.
2. If the image result record at `0x42000000` holds the resident OS result
   magic, copy its 128 bytes into report bytes 192..319 and clear its magic.
   Each boot of a slot repeats its generation, and the image refuses a
   contract whose generation a retained result already holds.
3. Refuse with status 8 if WDT0 mode bit 0 or configuration bit 8 is set,
   or the ROM redirect words or RTC FEL flag are nonzero. Otherwise write the reset output and four-second configuration, read
   them back, read the counter as `armed_at`, enable the mode and restart it.
   The arming must read back within one millisecond. This is the FEL
   launcher's sequence without the redirect and FEL flag. If the mode
   readback or the timing check fails after the mode write, the report holds
   status 8 with the arming counter value as its detail. WDT0 then stays
   armed with no ROM redirect, so the board leaves the ROM's FEL mode with a
   cold reset within four seconds.
4. Write `armed_at` into the board session and optional recorded CPU grant
   as their fresh epoch, then hash every session byte.
5. Clear the contract magic at `0x42004000`. Write version 2, the epoch, the
   slot generation and its inverse, the build ID named by the session and the
   session lease. Write `armed_at`, a startup deadline one second later, no
   diagnostic deadline (`UINT64_MAX`) and expiry four seconds after arming.
   Write the armed watchdog fields, zero FEL fields, and the session address,
   size and digest. Write the resident OS contract magic last.
6. Publish the report with status 6, `armed_at` in report bytes 112..119 and
   the contract address in bytes 120..127, then mask interrupts, invalidate
   the instruction cache and branch to the slot entry at EL3 with
   translation and caches off.

The OS image's armed boot reads WDT0, the redirect words and the FEL flag
back against this contract before it admits it. A slot has one image and at
most three assets. [Cold OS preparation](designs/autonomous-card-os.md) combines
both models in one span at `0x44000000`, preserving their fixed addresses.
The other two assets are the session and the WAV. `boot-slots.py plan
--os-session` accepts an unassigned zero-epoch recorded template and binds
both board and grant generation before computing the slot's extent hashes.

## Install, update and rollback

None of these procedures may run on a physical card until recovery is
established and both existing cards are preserved, as the project goal
requires. The tool only reads and writes ordinary regular files; no command
opens a block device. The physical write step therefore remains a separate,
reviewed operation that writes only the extents of a checked plan.

Every procedure starts from a full-card host backup, verified by hash and kept
read-only. The working copy of that backup is the image the tool plans
against and applies to.

**Install.**

1. Create the slot regions in unallocated space beyond sector 40960, for
   example by shrinking a partition on the working copy only after the
   backup exists.
2. Record ownership: `boot-slots.py allocate --backup BACKUP --slot-a
   FIRST:BLOCKS --slot-b FIRST:BLOCKS --output allocation.json`. The record
   binds the card size, the backup hash and the partition sector hash.
3. Package the loader with `prepare-microsd-boot.py`, then plan:
   `boot-slots.py plan --allocation allocation.json --card-image WORKING
   --image os.bin@0x41000000 --entry 0x41000000 --asset base.mimus@0x44000000
   --container PREFIX.manifest.json --output install`. The plan orders the slot
   payload, then its header, then boot0 and the TOC1 package, so the new
   monitor is written only after a valid slot exists.
4. Apply the plan to the working copy and run `boot-slots.py verify` on it.
   Only the verified plan's extents may be written to the card.
   `storage-update.py plan` turns them into one bounded storage session per
   changed extent ([planning a boot-card update](storage-update.md#a-boot-slot-plan-becomes-one-session-per-changed-range)).

**Update.** Plan against a current image of the card. The tool selects the
standby slot, the one the loader would not choose, and sets its generation one
above every valid header. Applying the plan writes the payload, synchronizes,
then writes the header. If writing stops before the header, the previous slot
remains selected. `apply` compares every target extent with the plan's
recorded previous contents before writing anything. A card that changed since
planning is refused, and a partly applied plan resumes by skipping extents
that already match.

An interrupted plan must be resumed by running `apply` again with the same
plan directory. Once its header is written, the new slot is selected, so a
fresh plan would choose the slot that holds the previous image as its standby
and overwrite it. `plan` refuses when the selected slot already holds the
requested image, assets and entry address. This covers an install that
stopped after its header but before boot0 and the TOC1 package. The same
check refuses a plan that copies the active image into the standby slot, so
the two slots never hold the same image. `plan` also refuses in two other
cases:

- a valid header of the other slot has an extent inside the standby region,
  as a header written by another tool may have;
- a valid header holds the largest 64-bit generation, so no newer generation
  exists.

Entry and load addresses that do not fit in 64 bits are refused when the
arguments are read.

**Rollback.** `boot-slots.py rollback` plans one write: a zero sector over the
active header. It refuses unless the other slot's header and payload verify.
After rollback the older generation is selected again, and its payload bytes
are unchanged. When neither slot verifies, the only recovery is restoring the
backup through FEL recovery and a host card writer.

Installing a new boot0 and TOC1 replaces the card's previous boot chain. The
full backup is the only way to restore it.

## Checks

[standalone-boot-loader.py](../checks/standalone-boot-loader.py) runs the linked
AArch64 loader after the pinned boot0 parser and cleanup model. Sparse card
images come from the actual host slot producer. Remote-writer and CPU/cache
ownership are explicit model grants; the native prerequisites still refuse.

The focused high-speed/DMA cases exercise maximum-size direct transfers,
partial-sector canaries, unaligned destinations, digest fallback, exact
chain refusal, DMA errors, reset/idle retention, clock-parent and interrupt
refusal, changed provider state, card removal and counter/deadline failures.
The model observes the actual DMA ownership callbacks and validates every
submitted descriptor against the current extent before producing card bytes.
Completion must precede the AFTER callback, and all reset/idle checks must
precede clean stop or return. Retained cases require no ROM reset or image entry.

Evidence is under `~/.cache/sunstoneos/evidence/resume-stage2-sd-1026/`.
The affected runtime provider's sanitizer and native build checks also pass.
Existing arithmetic, SD protocol and physical high-speed PIO evidence retain
their prior scope. These checks model system registers, clock/IRQ state,
DMA and cache visibility; they do not qualify cold boot or physical DMA.

[standalone-boot-slots.py](../checks/standalone-boot-slots.py) covers
allocation refusals, the install order with container extents, an interrupted
and resumed update, an interrupted install that refuses a new plan and then
resumes, refusal of a changed image before any write, rollback and its
refusal, plan refusals and the header rules. The plan refusals include a
foreign header with an extent in the standby region and the 64-bit limits.
[os-image-stage2.py](../checks/os-image-stage2.py) runs the same monitor and
cold SD admission with the three entry prerequisites modeled as granted
unless a case refuses one, and with WDT0, the redirect words, the RTC FEL flag
and CCU word `0x24` modeled. Its slot holds an image, the OS session and an
asset. Fifteen cases cover:

- entry with and without a retained result, and entry with the 992-byte
  microphone session;
- all three prerequisites refused, and each one refused alone;
- a missing session extent, an extent of another size, an 808-byte session
  whose size field claims 992 bytes, and a session generation that differs
  from the slot's;
- an enabled watchdog, a ROM redirect or an RTC FEL flag left by another
  owner, and a WDT0 mode write that does not read back.

The entry cases check the four arming writes, every contract field, the
stamped session epoch and digest and the branch to the entry, then pass the
contract and session to the OS image's resident admission
([os-image-model.c](../checks/os-image-model.c)), which must accept them. The
check compiles that model a second time for the resident microphone build and
uses it for the 992-byte session.
The refusal cases check that no contract is published. The mode readback
case checks the four arming writes and a nonzero detail; every other refusal
case checks that no register write happens and, for status 8, a zero detail.

The checks run on the development host:

```sh
cmake -S boot/a733/stage2 -B BUILD -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/aarch64-clang.cmake \
  -DSUNSTONE_LLD=/path/to/ld.lld
cmake --build BUILD --target a733-stage2
python checks/standalone-boot-loader.py --build BUILD --boot0 boot0_sdcard_orangepizero3w.fex --output NEW
python checks/standalone-boot-slots.py --output NEW
python checks/os-image-stage2.py --build BUILD --boot0 boot0_sdcard_orangepizero3w.fex \
  --model RESIDENT_MODEL --output NEW
```

`RESIDENT_MODEL` is `checks/os-image-model.c` built for the resident lifetime,
as its header comment shows.

The loader uses the Python environment with Unicorn, Capstone and pyelftools.
The cold-entry recorder built by `tools/prepare-cold-entry.py` is unchanged;
the stage-2 build selects the loader with `A733_COLD_STAGE2`.
