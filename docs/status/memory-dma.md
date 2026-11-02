# Memory, DRAM restoration and DMA

**Status: implemented.**

Native mappings, privileged reservation pools, cache maintenance, DMA0 services and shared IOMMU tables have host and emulator evidence. Full-capacity memory and device visibility are unqualified.

**Software evidence:** host, emulator.

**Board-qualified scope:** Full training and explicit-record restores on both
boards report 12,288 MiB and pass two 4 KiB page comparisons. SID-selected
restoration on 3-1 passes twice with 879 PHY writes and quick checks of 34.116
and 34.117 ms. CPU-L/DSU boot clocks restore. EL3 entry restores MBUS to 600 MHz
on both boards while preserving the checked pattern.

## Open work

1. Qualify full-capacity memory and device visibility, and physical range ownership with external-pool mapping.
2. Cover restore temperature, lower DRAM points, card deployment and decoding of the boot0 memory configuration.
3. Establish physical static secondary-OFF ownership for the [native probe contract](../board-probe-contract.md).
4. Qualify physical DMA and USB return.

## DMA

**Software evidence:** host, emulator.

**Board-qualified scope:** None.

DMA0, DMA1 and IOMMU operations have no physical qualification. Host checks pass for descriptor bounds, DMA0 admission, retained-allocation exclusion and IOMMU table construction. FreeRTOS emulator checks pass for DMA service ownership and cyclic shutdown ordering. DMA0 supports bounded descriptor chains, 35-bit addressing, polled and interrupt completion, copied and cyclic receive services.

DMA0 needs physical channel, controller and data-access authority and transfer tests. DMA1 is blocked by an unresolved CPUS request and DRAM encoding. Native master exclusion, drain, installation and translation need physical evidence; no transfer above 4 GiB is qualified. See [memory map](../memory-protection.md), [DMA](../dma.md), [IOMMU](../iommu.md) and [storage](storage.md) for the physical SD DMA reads.

## Evidence

Board results are records in `docs/evidence/memory-dma.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-memory-dma-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/memory-dma.jsonl`.
<!-- evidence-table:end -->
