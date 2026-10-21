# Ordinary-file microSD monitor packaging

`tools/prepare-microsd-boot.py` creates a 20 MiB ordinary file containing the
pinned Orange Pi boot0 and one caller-supplied flat monitor payload. This is a
host packaging capability. CPU entry, application functionality, physical boot
and storage security remain unqualified. A successful pack does not admit the
artifact for a board run or establish that its instructions are useful.

The tool never writes a card. It accepts existing ordinary inputs and creates a
set of private regular output files with a caller-selected filename prefix. It refuses symbolic links in input and output
paths, device paths, and existing output entries, including the prefix itself. Both current boards lack eMMC
and UFS; neither device belongs in a boot probe. Existing microSD contents remain
outside this tool's scope.

## Inputs

Supply the exact pinned board boot0 and `dragonsecboot` executable explicitly.
The preparer verifies their entire contents before using private copies. It does
not download dependencies or execute downloaded shell scripts.

| Input | SHA256 |
| --- | --- |
| `boot0_sdcard_orangepizero3w.fex` | `829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3` |
| `external/packages/pack-uboot/tools/dragonsecboot` | `e5f9076504a0ccde29c5978bcfc341c1c5ac02d947c47b849a5e8d26433545f6` |

Both files come from Orange Pi build revision
`a95129bf30ea11886cc35f08ebb3e69695a26656`. The packer is a static x86-64 GNU/Linux
executable with no dynamic loader or shared-library dependencies. Packaging does
not require a Linux runtime on the board.

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
  --packer /ordinary/cache/dragonsecboot \
  --output /ordinary/path/new-package
```

The output parent must already exist. For this example, filenames begin with
`new-package.`; no output directory is created. The packer runs with a fixed two-line
CRLF configuration naming only `monitor.fex`. No stock monitor, U-Boot, SCP or
OP-TEE component is inserted automatically. A temporary private workspace holds
the verified executable, copied payload and configuration. Before publication,
the wrapper checks the actual produced table of contents (TOC), sole item, reserved fields,
full payload, padding, extent and checksum. A failed pack or validation emits no
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
and JSON manifest. The manifest records complete file hashes, dependency URLs and hashes,
monitor format and extent, and explicit false qualification fields. It has no
board-admission flag. Its `card_writes` list names the two extents that carry
container data, boot0 and the TOC1 package, with offset, length and hash.
A card install writes only those extents; the
[boot-slot layout](boot-slots.md) keeps slot headers in the container's zero gaps.

| Container extent | Contents |
| --- | --- |
| Total length | 20,971,520 bytes, initially zero-filled |
| Offset 8,192 | Unmodified pinned 245,760-byte boot0 |
| Offset 16,793,600 | Validated monitor-only TOC package |
| Remaining bytes | Zero padding; no partitions or filesystems |

The pinned [A733 build recipe](https://github.com/orangepi-xunlong/orangepi-build/blob/a95129bf30ea11886cc35f08ebb3e69695a26656/external/config/sources/families/sun60iw2.conf#L182)
selects this executable and merged-file placement. Its normal configuration
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

Published [packer source](https://github.com/HandsomeMod/allwinner-bsp-tools/blob/9906775be064de771723ec2e2b716f0a531f93f4/toc_tools/toc/create_package.c)
defines matching geometry and checksum behavior. Exact correspondence between
that source and the board executable remains unproved. Those source files carry
All Rights Reserved notices, and their repository has no top-level license grant.
The Orange Pi build repository's GPLv2 root license does not resolve the
individual tool's licensing. The wrapper preserves this unresolved provenance;
it reuses the published executable and does not rewrite TOC generation.

`checks/microsd-boot-package.py` runs one synthetic, nonbootable monitor through
the actual wrapper and packer, then checks changed bounds and preservation
conditions locally. Its output proves host packaging only. Production monitor
instructions, cold entry, physical storage/security and board boot require
separate root-owned evidence.
