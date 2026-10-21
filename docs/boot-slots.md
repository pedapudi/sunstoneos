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
The loader does not enter the loaded image, and its native SD admission refuses
before any controller access. The [boundary section](#the-loader-stops-before-entering-the-image)
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
writes. Each operation has an absolute counter deadline.

1. Read both header sectors and check each header against the rules above.
2. Order valid headers by generation, newest first. Equal generations try A
   first.
3. For the first candidate, copy each extent in 4 KiB chunks to its load
   address, then hash the destination bytes. A hash mismatch or read failure
   moves to the other valid slot.
4. Publish a 512-byte report at `0x47008200`, magic last, and request the ROM
   forced-FEL return: RVBAR is set to `0x20` and an AArch32 warm reset is
   requested, as the RAM monitor does.

The report holds status, detail, selected slot and generation, card capacity,
start and end counter values and the last SD core result. Per slot it records
the header state, generation, load state and failing extent.

| Status | Meaning | Detail |
| --- | --- | --- |
| 1 | A slot was copied and verified; the image was not entered | Entry address |
| 2 | Card admission or identification failed | SD core error |
| 3 | Neither header is valid | SD core error |
| 4 | Every valid slot failed to load | SD core error |

The SD core never retries after a fault, so a read failure during one slot
also fails the other. An exception during the loader uses the cold-entry
exception record and parks, because repeating the ROM-return sequence could
repeat the fault.

The loader never writes the card. Fallback therefore covers a slot that fails
to read or verify; it cannot detect a runtime image that verifies and then
fails while running. That case needs the rollback procedure below.

## The loader stops before entering the image

Four prerequisites are missing, and each is outside this monitor:

- **Launch record.** Runtime images enter at EL3 and require a sealed launch
  contract at `0x42004000`. That contract records facts that only the host
  establishes, such as watchdog arming, FEL redirect state and an exclusive
  lease. The loader does not forge those fields. A cold-boot contract version
  with its own admission rules, together with runtime support for it, must
  exist first.
- **Remote-writer exclusion.** No stop or drain procedure exists for the E902
  core or for other DMA-capable agents
  ([cold-entry record](cold-entry.md#remote-processor-exclusion-needs-the-effective-a733-register-map)).
  Ownership of the `0x42e00000` boot0 staging area is also undecided.
- **Cold SD admission.** The SD core requires evidence of settled controller
  clocks, pins, card supplies, presence and exclusive ownership.
  `a733_stage2_card_admission` in
  [admission.c](../boot/a733/stage2/admission.c) returns refusal until a
  fixed-board subset of the shared providers supplies that evidence. On a
  board the current loader therefore reports status 2 without touching the
  controller.
- **Cold ROM return.** A forced-FEL return from a cold boot has not been
  observed. The FEL-launched watchdog path also writes an RTC FEL flag and a
  redirect flag that a cold boot never sets.

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

**Update.** Plan against a current image of the card. The tool selects the
standby slot, the one the loader would not choose, and sets its generation one
above every valid header. Applying the plan writes the payload, synchronizes,
then writes the header. If writing stops before the header, the previous slot
remains selected. `apply` compares every target extent with the plan's
recorded previous contents before writing anything. A card that changed since
planning is refused, and a partly applied plan resumes by skipping extents
that already match.

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
built by the host tool. Twelve cases cover:

- slot A alone, and a newer slot B;
- slot B with a corrupt payload, a corrupt header, a load range reaching the
  monitor reservation, and an update interrupted before its header;
- no valid header, both payloads corrupt, and a controller read fault;
- the native admission refusal, with no controller access;
- the unchanged entry gate refusing EL1, and an exception parking.

Every case checks the report, the ROM-return request and the copied bytes. It
also checks that data accesses stay naturally aligned, as Device memory
requires, and that no store leaves the monitor storage, the load extents and
the reset-vector registers. The loader never executes from the load window or
writes the system control register. The host tool reproduces the loader's
selection and per-slot states on every card image without a modeled card
fault. System registers, the counter, the SD controller and card, and cold
admission evidence are modeled.

[standalone-boot-slots.py](../checks/standalone-boot-slots.py) covers
allocation refusals, the install order with container extents, an interrupted
and resumed update, refusal of a changed image before any write, rollback and
its refusal, plan refusals and the header rules. Both checks run on the
development host:

```sh
cmake -S boot/a733/stage2 -B BUILD -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/aarch64-clang.cmake \
  -DSUNSTONE_LLD=/path/to/ld.lld
cmake --build BUILD --target a733-stage2
python checks/standalone-boot-loader.py --build BUILD --boot0 boot0_sdcard_orangepizero3w.fex --output NEW
python checks/standalone-boot-slots.py --output NEW
```

The loader uses the Python environment with Unicorn, Capstone and pyelftools.
The cold-entry recorder built by `tools/prepare-cold-entry.py` is unchanged;
the stage-2 build selects the loader with `A733_COLD_STAGE2`.
