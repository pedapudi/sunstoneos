# Memory, DRAM restoration and DMA

**Status: implemented.**

Native mappings, privileged reservation pools, cache
maintenance, DMA0 services and shared IOMMU tables have host or emulator evidence.
Full-capacity memory and device visibility remain unqualified.

**Board-qualified scope:** Full training and explicit-record restores on both
boards report 12,288 MiB and pass two 4 KiB page comparisons. SID-selected
restoration on 3-1 passes twice with 879 PHY writes and quick checks of 34.116
and 34.117 ms. CPU-L/DSU boot clocks restore. EL3 entry restores MBUS to 600 MHz
on both boards while preserving the checked pattern.

Evidence paths are relative to `~/.cache/sunstoneos/evidence/`:

- `dram-default-board-1031/3-1-203259/session.json` and
  `dram-default-board-1031/3-1-204839/session.json` identify the default restores.
- `second-board-20261018-3-3/platform-attempt1/` records the second-board survey
  with restored SRAM.
- `sol-resume-20261028/` and `sol-resume-20261031/` bind the MBUS and checkpoint
  memory comparisons. These checks do not cover all reported RAM.

The [memory qualification image](../memory-qualification.md) and complete
block-map corruption checks are implemented. Its legacy frozen image is under
`~/.cache/sunstoneos/build/memory-dma-1031/images/memtest/`. The
[native probe contract](../board-probe-contract.md) supplies a converted example
and host checks; physical static secondary-OFF ownership remains a prerequisite.
No full-map memory run is qualified. Restore temperature coverage, lower DRAM
points, card deployment and decode of the boot0 memory configuration remain open.

The EL1 map describes 12,174 MiB outside protected ranges and the unowned
`0x79000000..0x80000000` hole. Full-map publication needs a passing per-boot DRAM
decode check; current images retain the 910 MiB extended or 126 MiB local map.
The larger pool reserves 768 MiB cacheable and 16 MiB uncached memory.
Physical range ownership and external-pool mapping remain unqualified.

The USB-class OS profile reserves an 8 MiB Normal non-cacheable arena at
`0x42200000..0x42a00000`, between the secure monitor and radio assets.
The existing uncached allocator owns this linker reservation. USB takes the
first 5 MiB lease; retained JPEG input, IOMMU tables and guarded output planes
use separate leases in the remainder. Application regions, card staging and
the private NPU pool remain outside the arena. The application receives only
a read-only alias of USB payload pages at `0x7a040000..0x7a440000`.

[The focused host check](../../checks/usb-memory.py) covers the production
linker reservation, retained allocation capacity, native table construction,
adjacent write-back mappings and every core's table checksum. It models
registers and cache effects. Runner admission supplies physical ownership
evidence; an optional session extent checks geometry only. Physical DMA and
USB return remain unqualified.

## DMA

**Board-qualified scope:** None.

DMA0, DMA1 and IOMMU operations have no physical qualification.
Host checks pass for descriptor bounds, DMA0 admission, retained-allocation
exclusion and IOMMU table construction. FreeRTOS emulator checks pass for
DMA service ownership and cyclic shutdown ordering.

DMA0 supports bounded descriptor chains, 35-bit addressing, polled and interrupt
completion, copied and cyclic receive services, retained failures and serialized
FreeRTOS callers. INTID 143 wakes the I2S and microphone-profile owner.
Cyclic output excludes metadata and all retained allocations before callbacks;
[the ownership evidence](../designs/dma-rx-copy-ownership.md) preserves callback
counts for valid copies. Hardware ordering is still modeled.

DMA0 needs physical channel/controller/data-access authority and transfer tests.
DMA1 is blocked by unresolved CPUS request and DRAM encoding; DMA0's encoding
does not establish DMA1's. Both IOMMU banks have table construction and fault
observation, but native master exclusion, drain, installation and translation
need physical evidence. No transfer above 4 GiB is qualified.

The [memory map](../memory-protection.md), [DMA](../dma.md),
[address widths](../dma-address-width.md), [IOMMU](../iommu.md),
[restoration design](../designs/dram-training-restore.md) and
[memory admission](../designs/memtest-map-admission.md) records hold details.
Physical SD DMA read evidence belongs to [storage](storage.md), rather than
qualifying the general DMA controllers.
