# Memory, DRAM restoration and DMA

**Status: implemented.**

Native mappings, privileged reservation pools, cache maintenance, DMA0 services and shared IOMMU tables have host and emulator evidence. Full-capacity memory and device visibility are unqualified.

**Software evidence:** host, emulator.

**Board-qualified scope:** Full training and explicit-record restores on both
boards report 12,288 MiB and pass two 4 KiB page comparisons. SID-selected
restoration on 3-1 passes twice with 879 PHY writes and quick checks of 34.116
and 34.117 ms. CPU-L/DSU boot clocks restore. EL3 entry restores MBUS to 600 MHz
on both boards while preserving the checked pattern. Sustained DRAM reads on 3-1 in
development mode reach 14.8 GB/s for one A76 core and fall to 8.25 GB/s for all
eight cores, as the number of independent streams grows (see
[DRAM bandwidth](../designs/dram-bandwidth.md#board-measurements-of-the-scaling-study)).

## Open work

1. Read and then write boot0's nine NSI interconnect words in an image and repeat the DRAM scaling study ([plan](../designs/dram-bandwidth.md#plan-awaiting-approval)); the write needs coordinator approval.
2. Qualify full-capacity memory and device visibility, and physical range ownership with external-pool mapping.
3. Cover restore temperature, lower DRAM points, card deployment and decoding of the boot0 memory configuration.
4. Establish physical static secondary-OFF ownership for the [native probe contract](../board-probe-contract.md).
5. Qualify physical DMA and USB return.

## DMA

**Software evidence:** host, emulator.

**Board-qualified scope:** None.

DMA0, DMA1 and IOMMU operations have no physical qualification. Host checks pass for descriptor bounds, DMA0 admission, retained-allocation exclusion and IOMMU table construction. FreeRTOS emulator checks pass for DMA service ownership and cyclic shutdown ordering. DMA0 supports bounded descriptor chains, 35-bit addressing, polled and interrupt completion, copied and cyclic receive services.

DMA0 needs physical channel, controller and data-access authority and transfer tests. DMA1 is blocked by an unresolved CPUS request and DRAM encoding. Native master exclusion, drain, installation and translation need physical evidence; no transfer above 4 GiB is qualified. See [memory map](../memory-protection.md), [DMA](../dma.md), [IOMMU](../iommu.md) and [storage](storage.md) for the physical SD DMA reads.

## Evidence

Board results are records in `docs/evidence/memory-dma.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-memory-dma-20261101.md).

<!-- evidence-table:begin -->
4 records in `docs/evidence/memory-dma.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793597424798692951-6741863a` | 2026-11-02 | 3-1 | `4e4f51b1ed5f` | returned | failed | run_total 19.252 s, read_one_a55 5.36 GB/s, read_one_a76 12.69 GB/s, read_two_a76 11.37 GB/s, read_six_a55 9.03 GB/s, read_eight 8.44 GB/s |
| `1793600409082394041-436818e2` | 2026-11-02 | 3-1 | `d7d1b5e761f6` | result-mismatch | failed | run_total 19.902 s, sum_one_a55 5.5 GB/s, sum_two_a55 10.05 GB/s, sum_four_a55 10.77 GB/s, sum_six_a55 9.06 GB/s, sum_one_a76 14.83 GB/s, sum_two_a76 14.02 GB/s, sum_four_a55_two_a76 9.36 GB/s, sum_eight 8.24 GB/s, sum_six_a55_stagger_4096 9.06 GB/s, sum_two_a76_stagger_4096 13.82 GB/s, sum_eight_stagger_4096 8.24 GB/s, sum_six_a55_stagger_65536 9.01 GB/s, sum_two_a76_stagger_65536 13.36 GB/s, sum_eight_stagger_65536 8.23 GB/s, sum_six_a55_stagger_262144 9.06 GB/s, sum_two_a76_stagger_262144 12.87 GB/s, sum_eight_stagger_262144 8.25 GB/s, sum_one_a55_mode1 5.28 GB/s, sum_one_a76_mode1 14.7 GB/s, sum_six_a55_mode1 9.05 GB/s, sum_two_a76_mode1 13.28 GB/s, sum_eight_mode1 8.22 GB/s, sum_one_a55_mode2 4.2 GB/s, sum_one_a76_mode2 12.54 GB/s, sum_six_a55_mode2 8.64 GB/s, sum_two_a76_mode2 13.44 GB/s, sum_eight_mode2 7.91 GB/s, sum_one_a55_mode3 2.19 GB/s, sum_one_a76_mode3 6.73 GB/s, sum_six_a55_mode3 5.76 GB/s, sum_two_a76_mode3 8.53 GB/s, sum_eight_mode3 5.74 GB/s |
| `1793600534434449836-8f1652a3` | 2026-11-02 | 3-1 | `1a8e6ff37cc0` | returned | failed | run_total 20.572 s, sum_one_a55 5.46 GB/s, sum_two_a55 10.02 GB/s, sum_four_a55 10.76 GB/s, sum_six_a55 9.07 GB/s, sum_one_a76 14.81 GB/s, sum_two_a76 14.15 GB/s, sum_four_a55_two_a76 9.34 GB/s, sum_eight 8.27 GB/s, sum_one_a55_swprefetch 4.19 GB/s, sum_two_a55_swprefetch 6.62 GB/s, sum_four_a55_swprefetch 9.53 GB/s, sum_six_a55_swprefetch 8.36 GB/s, sum_one_a76_swprefetch 13.8 GB/s, sum_two_a76_swprefetch 13.75 GB/s, sum_four_a55_two_a76_swprefetch 8.93 GB/s, sum_eight_swprefetch 7.94 GB/s |
| `1793600624723356256-fb710c2d` | 2026-11-02 | 3-1 | `5c6bbac1b12b` | returned | failed | run_total 21.112 s, sum_one_a55 5.49 GB/s, sum_two_a55 10.01 GB/s, sum_four_a55 10.82 GB/s, sum_six_a55 9.07 GB/s, sum_one_a76 14.86 GB/s, sum_two_a76 14.08 GB/s, sum_four_a55_two_a76 9.36 GB/s, sum_eight 8.25 GB/s, sum_one_a55_mode5 5.17 GB/s, sum_two_a55_mode5 4.8 GB/s, sum_four_a55_mode5 7.32 GB/s, sum_six_a55_mode5 7.22 GB/s, sum_one_a76_mode5 12.45 GB/s, sum_two_a76_mode5 11.75 GB/s, sum_four_a55_two_a76_mode5 8.38 GB/s, sum_eight_mode5 7.45 GB/s, sum_four_a55_mode6 6.88 GB/s, sum_six_a55_mode6 5.93 GB/s, sum_two_a76_mode6 11.3 GB/s, sum_eight_mode6 5.84 GB/s |
<!-- evidence-table:end -->
