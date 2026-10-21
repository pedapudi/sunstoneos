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
The loader's entry path, which arms the watchdog, builds the resident contract
of the [integrated OS image](os-image.md) and branches to it, is implemented
and host-modeled. Its native entry prerequisites refuse, so the loader does not
enter the loaded image, and its native SD admission refuses before any
controller access. The [boundary section](#the-loader-stops-before-entering-the-image)
lists what each of these needs.

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
GPT entry array.

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

The loader runs after the [cold-entry gate](cold-entry.md) has admitted the
primary CPU at EL3 with translation and both caches off. It identifies the card
through the polled SD core in [sunxi_mmc.c](../drivers/storage/sunxi_mmc.c),
using the 1-bit legacy profile at a 400 kHz module clock. Its register binding
admits only read-only identification and transfer commands and refuses FIFO
writes. Each operation has an absolute deadline in system counter ticks, so
the loader first samples the counter up to 4,096 times and stops with status 5
when the value never increases. A counter that boot0 left stopped would
otherwise leave every deadline unreached while a silent controller holds the
polling loop.

1. Read both header sectors and check each header against the rules above.
2. Order valid headers by generation, newest first. Equal generations try A
   first.
3. For the first candidate, copy each extent in 4 KiB chunks to its load
   address, then hash the destination bytes. A hash mismatch or read failure
   moves to the other valid slot.
4. Check the entry prerequisites. While they refuse, publish a 512-byte
   report at `0x47008200`, magic last, and request the ROM forced-FEL return:
   RVBAR is set to `0x20` and an AArch32 warm reset is requested, as the RAM
   monitor does. The [entry path](#the-entry-path-builds-the-resident-contract)
   follows when they are granted.

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
| 7 | Entry refused: no 808-byte session extent at `0x41f00000` (1), or its board magic, version, size, generation or lease differs (2) | 1 or 2 |
| 8 | WDT0, the ROM redirect words, the RTC FEL flag or CCU word `0x24` was not in the cold state, or arming did not read back | Arming counter value, or zero |

The SD core never retries after a fault, so a read failure during one slot
also fails the other. An exception during the loader uses the cold-entry
exception record and parks, because repeating the ROM-return sequence could
repeat the fault.

The loader never writes the card. Fallback therefore covers a slot that fails
to read or verify; it cannot detect a runtime image that verifies and then
fails while running. That case needs the rollback procedure below.

## The loader stops before entering the image

`a733_stage2_entry_admission` in [admission.c](../boot/a733/stage2/admission.c)
returns refusal, because three prerequisites are missing. A fourth blocks the
SD controller access itself.

- **Remote-writer exclusion.** No stop or drain procedure exists for the E902
  core or for other DMA-capable agents
  ([cold-entry record](cold-entry.md#remote-processor-exclusion-needs-the-effective-a733-register-map)).
  Ownership of the `0x42e00000` boot0 staging area is also undecided.
- **Watchdog recovery.** A watchdog expiry after a cold boot has not been
  shown to restart the board into this monitor with both cards preserved. A
  forced-FEL return from a cold boot has not been observed either; the
  FEL-launched watchdog path writes an RTC FEL flag and a redirect flag that a
  cold boot never sets.
- **Entry state.** The CPU, GIC and timer state that boot0 leaves at EL3 has
  not been compared with the reset state that FEL-launched images see.
- **Cold SD admission.** The SD core requires evidence of settled controller
  clocks, pins, card supplies, presence and exclusive ownership.
  `a733_stage2_card_admission` in the same file returns refusal until a
  fixed-board subset of the shared providers supplies that evidence. On a
  board the current loader therefore reports status 2 without touching the
  controller. Before every poll the SD core also revalidates card presence
  through the loader's `retained` callback, which calls
  `a733_stage2_card_present`; that function also refuses. An admission that
  accepts must replace both. Presence comes from the active-low PF6
  card-detect pin, read under the pin ownership that admission retains
  ([SD presence](mmc.md)).

## The entry path builds the resident contract

With the prerequisites granted, the loader enters only the integrated OS
image. Its slot carries the 808-byte OS session as an asset loaded at
`0x41f00000`; the slot header's extent digest has already authenticated it.

1. Refuse with status 7 unless that extent exists, and the session's board
   magic, version 2, size, nonzero lease and generation equal the slot's.
2. If the image result record at `0x42000000` holds the resident OS result
   magic, copy its 128 bytes into report bytes 192..319 and clear its magic.
   Each boot of a slot repeats its generation, and the image refuses a
   contract whose generation a retained result already holds.
3. Refuse with status 8 if WDT0 mode bit 0 or configuration bit 8 is set,
   the ROM redirect words or RTC FEL flag are nonzero, or CCU word `0x24` bit 8
   is set. Otherwise write the reset output and four-second configuration, read
   them back, read the counter as `armed_at`, enable the mode and restart it.
   The arming must read back within one millisecond. This is the FEL
   launcher's sequence without the redirect and FEL flag.
4. Write `armed_at` into the session as its fresh epoch and hash the 808
   session bytes.
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
most three assets, so the OS image's two models and recorded WAV must form
one asset extent at `0x44000000`, with the session as the second asset.

The legacy profile reads about 25 KB/s, so 14 MB of models would take roughly
ten minutes. A faster SD profile needs the provider-owned module clock change
that the SD core already defines.

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

[standalone-boot-loader.py](../checks/standalone-boot-loader.py) feeds the flat
loader through the pinned boot0 parser and cleanup model, then executes the
linked AArch64 monitor from `0x47000000`. A Python SD controller and card model
follows the existing C card model's register behavior and serves card images
built by the host tool. Fourteen cases cover:

- slot A alone, and a newer slot B;
- slot B with a corrupt payload, a corrupt header, a load range reaching the
  monitor reservation, and an update interrupted before its header;
- no valid header, both payloads corrupt, and a controller read fault;
- card-detect reporting no card after the first payload read;
- the native admission refusal, and a stopped system counter, both without
  controller access;
- the unchanged entry gate refusing EL1, and an exception parking.

Every case checks the report, the ROM-return request and the copied bytes.
The native entry prerequisites refuse in every case. It
also checks that data accesses stay naturally aligned, as Device memory
requires, and that no store leaves the monitor storage, the load extents and
the reset-vector registers. Once the loader starts, a store to the cold report
also fails the case, and all 512 report bytes must match their values at
loader entry. The loader never executes from the load window or
writes the system control register. The host tool reproduces the loader's
selection and per-slot states on every card image without a modeled card
fault. System registers, the counter, the SD controller and card, and cold
admission and card-detect evidence are modeled.

[standalone-boot-slots.py](../checks/standalone-boot-slots.py) covers
allocation refusals, the install order with container extents, an interrupted
and resumed update, an interrupted install that refuses a new plan and then
resumes, refusal of a changed image before any write, rollback and its
refusal, plan refusals and the header rules. The plan refusals include a
foreign header with an extent in the standby region and the 64-bit limits.
[os-image-stage2.py](../checks/os-image-stage2.py) runs the same monitor with
the entry prerequisites, card admission and card presence modeled as granted,
and with WDT0, the redirect words, the RTC FEL flag and CCU word `0x24`
modeled. Its slot holds an image, the OS session and an asset. Eight cases
cover entry with and without a retained result, refused prerequisites, a
missing session extent, a session generation that differs from the slot's,
and an enabled watchdog, a ROM redirect or an RTC FEL flag left by another
owner. The entry cases check the four arming writes, every contract field,
the stamped session epoch and digest and the branch to the entry, then pass
the contract and session to the OS image's resident admission
([os-image-model.c](../checks/os-image-model.c)), which must accept them.
The refusal cases check that no register write or contract publication
happens.

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
