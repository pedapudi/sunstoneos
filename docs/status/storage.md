# Storage and autonomous card boot

**Status: implemented.**

Native SD identification, bounded reads, backup-first updates, slot loading and cold entry have software evidence. Neither current board has populated eMMC or UFS. Keep the failed UFS probe disabled.


**Board-qualified scope:** Returning microSD PIO reads at 50 MHz, four-bit,
3.3 V complete. One 4 KiB DMA read and one sentinel-prefilled 64 KiB read match
same-card PIO references and restore controller, clocks, pins and saved RAM
before normal ROM return. The sixteen-descriptor 64 KiB transfer takes
3,332 microseconds; it does not measure sustained throughput.

## Open work

1. Qualify physical installation, autonomous OS boot, cold controller and supply state, reset retirement and power-loss recovery.
2. Qualify SDR104 at 150 MHz (implemented and host-modeled): the lifetime needs whole-board power removal after success or a post-CMD11 failure, so it is blocked without physical recovery.
3. Measure sustained throughput on broader extents and cards, and qualify card replacement, removal and recovery, and voltage timing.
4. Implement SDR50 and DDR50; add GPT, multiple volumes and clients, and filesystem writes.
5. Resolve packer licensing and exact source equivalence.

Any physical card write needs its full host backup and the approved target-bound path in [goal section 4](../goal.md).

## Evidence

Board results are records in `docs/evidence/storage.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-storage-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/storage.jsonl`.
<!-- evidence-table:end -->
