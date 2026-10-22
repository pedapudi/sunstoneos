# EL1 memory map and protection

This document describes the stage-1 translation tables that the cached
multicore image installs at EL1, which memory each entry maps and which
exception level can use it. Readers are assumed to know AArch64 stage-1
translation and the FreeRTOS SMP port. Status: the extended-layout map is
**implemented** and checked entry by entry in QEMU; no part of it is
qualified on the board.

## The extended layout maps 910 MiB of DRAM in 2 MiB and 1 GiB blocks

FreeRTOS tasks start at EL0 unless the image selects EL1, and EL0 tasks call
kernel functions directly. Kernel data, stacks and heap therefore stay
readable and writable at EL0. Device pools have no EL0 mapping, so every task
that touches pool memory runs at EL1.

| Physical range | Use | EL1 descriptor | EL0 access |
| --- | --- | --- | --- |
| `0x0`–`0x4000_0000` | MMIO | 1 GiB block, Device-nGnRnE, XN | none |
| `0x4000_0000`–`0x4100_0000` | boot0 and launcher | 2 MiB blocks, WB, XN | read-write |
| `0x4100_0000`–`0x4120_0000` | text and read-only data | one 2 MiB block, WB, read-only | read-only, executable |
| `0x4120_0000`–`0x4200_0000` | data, BSS, stacks, heap | WB, XN | read-write |
| `0x4200_0000`–`0x4220_0000` | secure monitor and tables | invalid | none |
| `0x4220_0000`–`0x4800_0000` | boot assets; recognizer models, input and arena | WB, XN | read-write |
| `0x4800_0000`–`0x7800_0000` | pool `dma32` | 384 blocks, WB, XN | none |
| `0x7800_0000`–`0x7900_0000` | pool `uncached` | 8 blocks, Normal non-cacheable, XN | none |
| `0x7900_0000`–`0x8000_0000` | no owner | invalid | none |
| `0x8000_0000`–`0x3_4000_0000` | no owner | invalid | none |

WB means Normal write-back write-allocate, inner shareable. XN means
execute-never at both EL0 and EL1 (UXN and PXN set). Every writable entry is
execute-never, so no entry is both writable and executable. The two invalid
ranges inside the first 1 GiB of DRAM cover 114 MiB. The tables use five
4 KiB pages: one level-1 and one level-2 table for each of EL1 and EL3, and
one level-3 table that EL3 uses for the monitor block.

The local layout maps only `0x4000_0000`–`0x4800_0000` of DRAM. Its
write-back pool, at most 2 MiB (4 MiB with the large NPU graph), lies after
BSS and below `0x41f0_0000` inside the write-back data blocks. When the image
links an uncached pool, the level-2 entry for `0x41c0_0000`–`0x41e0_0000` is
Normal non-cacheable and keeps the access-permission and execute-never bits
of the data block it replaces.

## Mapping the rest of the 12 GiB is staged

The board reports 12 GiB of LPDDR5, decoded at `0x4000_0000`–`0x3_4000_0000`.
The map grows in two further stages, each waiting on its own evidence:

- **Stage 2** maps `0x8000_0000`–`0x1_0000_0000` as two 1 GiB write-back
  blocks, one for a GPU pool and one for an NPU pool, with no EL0 access.
  EL1 then maps 2,958 MiB. It needs version 2 of the boot record to carry
  layout identifier 2 and launch evidence that no boot-loader payload, remote
  firmware or bus master left running by boot0 uses that range.
- **Stage 3** maps `0x1_0000_0000`–`0x3_4000_0000` as level-1 entries 4 to 12,
  1 GiB blocks each, when a consumer claims the memory. EL1 then maps
  12,174 MiB with no additional table page. It needs the same launch evidence
  for that range.

Both stages also need an EL3 check that DRAM decodes linearly to
`0x3_4000_0000` and that the DRAM controller's protected regions allow
Non-secure access to the new range.

## One translation per pool block proves the pool mapping

A level-2 block descriptor gives one output address, one memory attribute
and one permission to all 2 MiB it maps. One address translation (`AT`) of
any address in the block therefore reports what every page of the block
reports. Extended pool preparation makes one write translation per 2 MiB
block: 384 for `dma32` and 8 for `uncached`.

This holds only while every pool entry is a block descriptor. The QEMU
fixture `checks/device-memory-capacity.py` compares all 1,024 EL1 level-1 and
level-2 entries of the extended-layout table builder with the table above.
It also checks that every valid level-2 entry is a block descriptor and that
no valid entry is writable and executable. A change to any entry, including
one that splits a pool block into pages, fails that comparison.

Level-1 entry 0, the MMIO block, has a shareability field of zero in the
builder's tables. Two monitor variants (`apps/storage-read-single-core/monitor.c`
and `apps/dsi-scanout/monitor.patch`) set its outer-shareable field after the
tables are prepared. That write changes no pool entry, and the capacity check
scans the sources and fails on any later write to the cached tables of
another form. Once the table builder sets the field itself and those writes
are deleted, the golden entry changes with it.

The local layout has no QEMU golden comparison, so its preparation keeps a
check of every pool page: one write translation per write-back page and a
read and a write translation per uncached page, 1,536 for 2 MiB pools.

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
