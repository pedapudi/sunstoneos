# Ordinary-file microSD monitor packaging

`tools/prepare-microsd-boot.py` creates a 20 MiB ordinary file containing the
pinned Orange Pi boot0 and one caller-supplied flat monitor payload. This is a
host packaging capability. CPU entry, application functionality, physical boot
and storage security remain unqualified. A successful pack does not admit the
artifact for a board run or establish that its instructions are useful.

The resident stage2 monitor supports the fixed nonsecure SD0 entry contract in
[remote-writers.md](remote-writers.md#ordinary-nonsecure-sd-boot-has-a-caller-owned-entry-contract).
The caller must establish a fresh, CE-quiesced power-on lifetime before ROM or
boot0 reuses DRAM. A whole-system-reset route requires physical qualification.
boot0's local AArch64 handoff inherits that lifetime; an arbitrary warm jump or
FEL upload does not. The pinned stock image or validated restore derivative and
sole-monitor package constrain the predecessor; runtime SID, header, media and
BOOT checks detect mismatches without attesting reset history. Packaging does
not qualify power/reset coverage or cold memory operation.

The tool never writes a card. It accepts existing ordinary inputs and creates a
set of private regular output files with a caller-selected filename prefix. It refuses symbolic links in input and output
paths, device paths, and existing output entries, including the prefix itself. Both current boards lack eMMC
and UFS; neither device belongs in a boot probe. Existing microSD contents remain
outside this tool's scope.

## Inputs

Supply the exact pinned board boot0 explicitly. The preparer verifies its entire
contents before use. It does not download dependencies or execute downloaded
shell scripts. The vendor `dragonsecboot` executable is an optional cross-check
(see [the TOC1 packer](#toc1-packer)); when supplied, the preparer verifies it
the same way.

| Input | SHA256 |
| --- | --- |
| `boot0_sdcard_orangepizero3w.fex` | `829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3` |
| `external/packages/pack-uboot/tools/dragonsecboot` (optional) | `e5f9076504a0ccde29c5978bcfc341c1c5ac02d947c47b849a5e8d26433545f6` |

Both files come from Orange Pi build revision
`a95129bf30ea11886cc35f08ebb3e69695a26656`. The vendor packer is a static x86-64
GNU/Linux executable with no dynamic loader or shared-library dependencies.
Packaging does not require a Linux runtime on the board.

The input JSON has exactly these fields. Replace the example length and hash
with the whole payload's actual identity:

```json
{
  "schema": 1,
  "kind": "a733-microsd-monitor-container-input",
  "monitor": {
    "format": "sunstone-a733-flat-monitor-v1",
    "path": "monitor.bin",
    "bytes": 268,
    "sha256": "<whole-file SHA256>"
  }
}
```

Relative payload paths resolve from the JSON file's directory. ELF inputs,
including existing FEL ELFs, are not accepted. The flat payload must have
`monitor\0` at offset 4, the little-endian loadword `0x47000000` at offset
`0x2c`, and an AArch64 unconditional immediate `B` at offset zero. That branch
must skip the entire `0x108`-byte mutation header and land on a complete
instruction inside the supplied payload. Checking this shape does not execute
or qualify the instruction stream. The pinned boot0 parser reads the loadword
before replacing runtime metadata in the header; TOC `run_addr` is not used as
an alternate load address.

The reserved monitor load interval is `0x47000000..0x47400000`. Payloads must
also fit the package within the published image extent. With the packer's
alignment, the largest admitted payload is 4,176,896 bytes; a nominal 4 MiB
payload does not fit. Package staging begins at `0x42e00000`; the selected
application ELF ranges below `0x42200000` and extended pool at `0x48000000` do
not overlap this monitor reservation. These addresses specify packaging bounds,
not a CPU entry or memory-ownership qualification.

```sh
python3 tools/prepare-microsd-boot.py \
  --input /ordinary/path/input.json \
  --boot0 /ordinary/cache/boot0_sdcard_orangepizero3w.fex \
  --output /ordinary/path/new-package
```

Add `--packer /ordinary/cache/dragonsecboot` to also run the vendor executable;
the preparer then requires its package to equal the repository packer's byte for
byte and fails otherwise.

The output parent must already exist. For this example, filenames begin with
`new-package.`; no output directory is created. The packer runs with a fixed two-line
CRLF configuration naming only `monitor.fex`. No stock monitor, U-Boot, SCP or
OP-TEE component is inserted automatically. A temporary private workspace holds
the copied payload and configuration (and the verified vendor executable, when
cross-checking). Before publication, the wrapper checks the produced table of
contents (TOC), sole item, reserved fields, full payload, padding, extent and
checksum with a decoder separate from the packer. A failed pack or validation emits no
output files. A failure during final publication can leave an incomplete set of
new files. Publication holds the existing parent directory open and creates each
file exclusively; a concurrent existing entry is preserved. The manifest is
written last and remains insufficient for qualification. Retrying with existing
output entries fails instead of replacing them.

## Outputs and format authority

Output suffixes are `monitor.bin`, `boot_package.cfg`, `boot_package.fex`,
`packer.log`, `microsd-monitor-container.bin` and `manifest.json`. Each suffix is
appended to the caller-selected prefix, separated by a period. These files hold
the copied monitor, exact configuration, actual package, log, merged container
and JSON manifest. The manifest records complete file hashes, the boot0 dependency URL and hash,
the packer that produced the package (the hash of `tools/toc_package.py` and
whether the vendor cross-check ran and matched), monitor format and extent, and
explicit false qualification fields. It has no
board-admission flag. Its `card_writes` list names the two extents that carry
container data, boot0 and the TOC1 package, with offset, length and hash.
A card install writes only those extents; the
[boot-slot layout](boot-slots.md) keeps slot headers in the container's zero gaps.
`storage-update.py plan` turns those extents, after a boot-slot plan, into
bounded storage sessions that never write the backup package unit at sector
24576 ([card update planning](storage-update.md#a-boot-slot-plan-becomes-one-session-per-changed-range)).

| Container extent | Contents |
| --- | --- |
| Total length | 20,971,520 bytes, initially zero-filled |
| Offset 8,192 | Unmodified pinned 245,760-byte boot0 |
| Offset 16,793,600 | Validated monitor-only TOC package |
| Remaining bytes | Zero padding; no partitions or filesystems |

The pinned [A733 build recipe](https://github.com/orangepi-xunlong/orangepi-build/blob/a95129bf30ea11886cc35f08ebb3e69695a26656/external/config/sources/families/sun60iw2.conf#L182)
selects the vendor executable and the merged-file placement. Its normal configuration
contains other firmware components; the monitor-only configuration is an explicit
separate host input. The U-Boot header also defines backup sector 24576, but the
published merged producer does not populate that backup, and this wrapper does
not add one. Zero-filled file gaps are not evidence that matching card extents
are free or may be overwritten.

The produced TOC has a 64-byte main header and one 368-byte item record. Its
magic is `0x89119800`, its item starts at offset 1024, and the total extent is
rounded to 16 KiB. The monitor item has type 3, no encryption, zero `run_addr`
and index, and exact input length. Padding is zero. The checksum is a modulo
32-bit sum of little-endian words over the declared padded extent, replacing
the checksum field with `0x5f0a6c39` for calculation.

## TOC1 packer

`tools/toc_package.py` builds the TOC1 package without the vendor executable. It
follows the published
[packer source](https://github.com/HandsomeMod/allwinner-bsp-tools/blob/9906775be064de771723ec2e2b716f0a531f93f4/toc_tools/toc/create_package.c)
and its configuration parser, and runs as a command
(`toc_package.py boot_package.cfg --directory DIR --output boot_package.fex`) or as
a module (`pack`, `unpack`, `pack_config`).

The configuration is the vendor `[package]` section: one `item=name, file` line
per item, at most 16 items, `;` and `#` comments, spaces, tabs or commas between
the two names. Items are laid out in order. The item table starts at offset 64 and
the first payload at the next 1 KiB boundary after the table; each later payload
starts at the next 1 KiB boundary; the total is rounded to 16 KiB. Every item has
type 3, no encryption and zero `run_addr` and index. The packer refuses inputs
the vendor tool mishandles without checking: more than 16 items, names of 64
bytes or more, a configuration line of 255 bytes or more, a package section over
1023 bytes, or a package over 10 MiB.

Not implemented: encryption, signing, certificates, key ladders and the TOC0
container (the vendor tool's other modes). The monitor-only and normal
(U-Boot, monitor, SCP) packages use only plain binary items, so the boot path
needs none of them.

`checks/toc-package.py` packs 15 monitor sizes (on and around the 1 KiB and 16 KiB
boundaries, and an empty file), the vendor's normal three- and four-item
configurations, a 16-item package with random lengths, empty middle items, a
63-byte item name and configuration syntax variants. It checks each package
against the layout above with a separate decoder, and, when the pinned vendor
executable is present (`--packer`, or `sources/board-boot/dragonsecboot` in the
cache), requires identical package bytes and an identical log. When the
executable is absent it skips the comparison and keeps the layout checks. It also
corrupts each format field and requires the decoder to refuse every corruption.
A packer with a different checksum seed or different rounding fails it, with or
without the executable.

The packer is written from the published source above, whose files carry
Allwinner copyright notices; the project owner confirmed that this use is
permitted.

`checks/microsd-boot-package.py` runs one synthetic, nonbootable monitor through
the actual wrapper, then checks changed bounds and preservation conditions
locally; with `--packer` it also requires the vendor executable's package to
match. Its output proves host packaging only. Production monitor instructions,
cold entry, physical storage/security and board boot require separate root-owned
evidence.

## Optional card DRAM restore image

`--restore-variant` accepts the manifest from `tools/prepare-card-ddr.py` while
`--boot0` still supplies the pinned original. The packager verifies the derived
image hash, eGON checksum, allowed branch destinations and unchanged bytes.
It also binds the manifest's temperature policy, helper identity and unit
locations to the configuration consumed by the bootstrap. The output manifest
records the derivative separately from its original dependency. Without this
option the package contains the original boot0.

The [restore design](designs/dram-training-restore.md#card-boots-need-a-host-built-boot0-variant)
defines the reserved helper extents and backup-first preparation. Packaging does
not install those helpers, reserve physical card space or qualify the modified
boot path. Current default variants perform full training because capture
temperature limits remain unqualified.
