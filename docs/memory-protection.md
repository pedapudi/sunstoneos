# EL1 memory map and protection

This document describes the stage-1 translation tables that the cached table
builder, `platform/a733-smp-cached/memory.c`, installs at EL1. The integrated
OS image, the other cached multicore images and the cached single-core images
use it. For each range the document gives what the entry maps and which
exception level can use it. Readers are assumed to know AArch64 stage-1 translation and the
FreeRTOS SMP port. Status: both layouts are **implemented** and checked entry
by entry in QEMU, with the DRAM decode check passing and failing. No part of
the map is qualified on the board, and the decode check has no definition yet
that can pass, so every image built today keeps the map below `0x4800_0000`
(local layout) or `0x7900_0000` (extended layout).

## EL1 maps 12,174 MiB of DRAM when the decode check passes

FreeRTOS tasks start at EL0 unless the image selects EL1, and EL0 tasks call
kernel functions directly. Kernel data, stacks and heap therefore stay
readable and writable at EL0. MMIO, the extended device pools and DRAM above
the boot image have no EL0 mapping, so every task that programs a device or
touches that memory runs at EL1. The local layout's pools, described after
the table, lie inside EL0 read-write data blocks.

| Physical range | Use | EL1 descriptor | EL0 access |
| --- | --- | --- | --- |
| `0x0`–`0x4000_0000` | MMIO | 1 GiB block, Device-nGnRnE, XN | none |
| `0x4000_0000`–`0x4100_0000` | boot0 and launcher | 2 MiB blocks, WB, XN | read-write |
| `0x4100_0000`–`0x4120_0000` | text and read-only data | one 2 MiB block, WB, read-only | read-only, executable |
| `0x4120_0000`–`0x4200_0000` | data, BSS, stacks, heap | WB, XN | read-write |
| `0x4200_0000`–`0x4220_0000` | secure monitor and tables | invalid | none |
| `0x4220_0000`–`0x4800_0000` | boot assets; recognizer models, input and arena | WB, XN | read-write |
| `0x4800_0000`–`0x7800_0000` | extended: pool `dma32`; local: no owner | 384 blocks, WB, XN | none |
| `0x7800_0000`–`0x7900_0000` | extended: pool `uncached`; local: no owner | extended: 8 blocks, Normal non-cacheable, XN; local: 8 blocks, WB, XN | none |
| `0x7900_0000`–`0x8000_0000` | no owner | invalid | none |
| `0x8000_0000`–`0x3_4000_0000` | no owner | eleven 1 GiB blocks, WB, XN | none |

WB means Normal write-back write-allocate, inner shareable. XN means
execute-never at both EL0 and EL1 (UXN and PXN set). Every writable entry is
execute-never, so no entry is both writable and executable. The two invalid
ranges inside the first 1 GiB of DRAM cover 114 MiB, so EL1 maps 910 MiB below
`0x8000_0000` and 12,174 MiB in all. The tables use five 4 KiB pages and one
64-byte checksum line, 20,544 bytes: one level-1 and one level-2 table for
each of EL1 and EL3, and one level-3 table that EL3 uses for the monitor
block. The 1 GiB blocks are level-1 entries 2 to 12 and add no table page.

The local layout's rows `0x4800_0000`–`0x7900_0000` and
`0x8000_0000`–`0x3_4000_0000` exist only when the decode check passes; the
extended layout's pools exist always and its 1 GiB blocks only when the check
passes. All new entries use `A733_EL1_DRAM_BLOCK` from
`include/a733_device_memory_layout.h`, the word the memory qualification image
also uses. EL3 entries above the boot image stay invalid, because EL3 does not
access that range. No allocator, pool or task owns a byte of the new range, so
mapping it grants no memory; a later owner translates its own range at
admission.

The local layout differs in two more places. Its write-back pool, at most
2 MiB (4 MiB with the large NPU graph), lies after BSS and below `0x41f0_0000`
inside the write-back data blocks. When the image links an uncached pool, the
level-2 entry for `0x41c0_0000`–`0x41e0_0000` is Normal non-cacheable and
keeps the access-permission and execute-never bits of the data block it
replaces, so EL0 can read and write both local pools.

## The DRAM decode check gates every entry above the boot image

A write-back block receives speculative reads as soon as it is mapped. A read
of an address that the DRAM controller aliases or that the boot0 firewall
protects could return another region's data or raise an SError. The builder
therefore runs `a733_dram_decode_check(ranges, count)` on core 0 at EL3, after
`a733_cpu_coherence_tables()` and before the table checksum, with the ranges
it is about to map: `0x4800_0000`–`0x7900_0000` (local layout only) and
`0x8000_0000`–`0x3_4000_0000`. The same constant table drives the entry
writes. The check returns 1 only when the controller decodes every listed byte
linearly from `0x4000_0000` with no alias, the decoded size reaches
`0x3_4000_0000`, and the firewall grants Non-secure read and write with no
protected region overlapping. When it returns 0, the builder writes none of
the entries and preparation continues with the map below the boot image.

The DRAM-controller package owns the check. Until its definition lands,
`memory.c` links a weak definition that returns 0. The capacity check reports
from each built image's symbol table which definition the image links; every
image built today links the weak one.

The new entries cost one store each: 11 in the extended layout and 403 in the
local layout. They enter the checksum that every core compares with its own
readback, and the tables do not change after preparation, so the builder
issues no new `AT`, cache maintenance or TLB maintenance.

## The GPU output region is read-only at EL0 and EL1 in the GPU linear OS image

The OS image built with GPU linear layers (`SUNSTONE_A733_OS_GPU_LINEAR`)
compiles the builder with `A733_GPU_OUTPUT_EL0_READ_ONLY`. The builder then
rewrites local-layout level-2 entries 54 and 55,
`0x46C0_0000`–`0x4700_0000`, with AP[2:1] = 0b11, the value of the text
block, and keeps UXN, PXN, write-back and inner shareable. AArch64 stage-1
permissions cannot give EL1 write access together with EL0 read-only access,
so EL1 loses write access too. No CPU store can therefore leave a dirty line
over the GPU's output. The GPU kernel's completion must use `DC CIVAC`, which
needs read permission only, because `DC IVAC` is checked as a write and faults
on the region. EL3 keeps its read-write entries. The builder stops with
`#error` when the definition meets the extended layout.

`aarch64_device_translate_el0()` in `arch/aarch64/device_memory.c` translates
an address with the EL0 permission check (`AT S1E0R`, `AT S1E0W`). It runs at
EL1 with the MMU on and has the signature, sequence and cost of
`aarch64_device_translate()`: one `ISB` and no `DSB`.

## One translation per 2 MiB block proves the pool mapping

A level-2 block descriptor gives one output address, one memory attribute
and one permission to all 2 MiB it maps. One address translation (`AT`) of
any address in the block therefore reports what every page of the block
reports. Extended pool preparation makes one write translation per 2 MiB
block: 384 for `dma32` and 8 for `uncached`. Local pool preparation in images
whose tables come from the cached builder makes one write translation at the
pool start and one at each following 2 MiB boundary below the end: two or
three for 2 MiB pools, and three with the large NPU graph.

This holds only while every pool entry is a block descriptor. The QEMU
fixtures in `checks/device-memory-capacity.py` compare all 1,024 EL1 level-1
and level-2 entries of the builder with the table above. They run the
extended layout and three local-layout images: with the uncached pool,
without it, and with the read-only GPU output view. Each runs with the decode
check passing and failing. They also check that every valid level-2 entry is a block
descriptor and that no valid entry is writable and executable. A change to
any entry, including one that splits a pool block into pages, fails that
comparison.

The compile definition `A733_LOCAL_POOLS_BLOCK_MAPPED` selects the block
stride, and `memory.c` stops with `#error` without it.
`cmake/a733-smp-cached.cmake` sets it on every target in its directory whose
sources include `memory.c`, and the cached single-core cmake files set it on
their targets. The definition leaves the absolute symbol
`a733_local_pools_block_mapped` in the image. The capacity check confirms
from every built image's symbol table that each image with that symbol links
`a733_cached_memory_prepare()`, and that each image linking both the builder
and a local pool has the symbol. Images whose tables come from `platform/a733/platform.c` have no golden
comparison and keep one write translation per pool page.

Level-1 entry 0, the MMIO block, has a shareability field of zero in the
builder's tables. Two monitor variants (`apps/storage-read-single-core/monitor.c`
and `apps/dsi-scanout/monitor.patch`) set its outer-shareable field after the
tables are prepared. That write changes no pool entry, and the capacity check
scans the sources and fails on any later write to the cached tables of
another form. Once the table builder sets the field itself and those writes
are deleted, the golden entry changes with it.

## What the map does not protect

- Kernel data, kernel stacks and FreeRTOS lock words are writable at EL0. An
  EL0 task can corrupt a pool ledger or hang every core through them.
  Separating them needs an EL1-only data region.
- Read-only data shares the executable text block. Making it execute-never,
  adding guard pages, or isolating kernel structures needs level-3 tables or
  a privileged data block.
- Admitted NPU models stay writable. A read-only model mapping would change
  the tables after preparation, which needs a mapping generation counter.
- `SCTLR_EL1.WXN` is not set. The golden comparison already refuses any
  writable executable entry.
