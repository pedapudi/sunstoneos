# Storage and autonomous card boot

**Status: implemented.**

Native SD identification, bounded reads, backup-first updates, slot loading and cold entry have software evidence. Neither current board has populated eMMC or UFS. Keep the failed UFS probe disabled.


**Board-qualified scope:** Returning microSD PIO reads at 50 MHz, four-bit,
3.3 V complete. One 4 KiB DMA read and one sentinel-prefilled 64 KiB read match
same-card PIO references and restore controller, clocks, pins and saved RAM
before normal ROM return. The sixteen-descriptor 64 KiB transfer takes
3,332 microseconds; it does not measure sustained throughput. A read-only probe
on port 3-1 read 160 sequential 128 KiB requests at 21.9 MB/s per request in the
same profile; it ran at boot clocks (CPU 494 MHz), so the figure is not a top-point
performance figure and the OS image re-measures it. The same probe mounted the card's
FAT32 volume read-only (32 KiB clusters, 1,907,436 clusters free, empty root directory).

## Open work

1. Qualify physical installation, autonomous OS boot, cold controller and supply state, reset retirement and power-loss recovery.
2. Qualify SDR104 at 150 MHz (implemented and host-modeled): the lifetime needs whole-board power removal after success or a post-CMD11 failure, so it is blocked without physical recovery.
3. Measure sustained throughput on broader extents and cards, and qualify card replacement, removal and recovery, and voltage timing.
4. Implement SDR50 and DDR50; add GPT, multiple volumes and clients.
5. Connect the [FAT32 write service](../designs/storage-write.md) (host-checked with a power-loss sweep) to the card: CMD25 with descriptor DMA, ACMD13 for the allocation unit, a read-over-write arbiter, bounded-write admission in the runner, and the board rate, latency and interruption measurements.
6. Map kernel and driver data EL1-only (backlog, wider than storage): an EL0 application can reach the bounds object of the write service, the private copy of its blob and the controller state; the service detects a changed blob by re-hashing it on every use ([memory protection](../memory-protection.md#backlog-kernel-and-driver-data-are-reachable-from-el0)).
7. After the first write has run, try the busy-clear interrupt (`THLD.BCIG`, `RINTSTS` bits 17 and 13) in place of the DAT0 busy timer, and compare the flush-latency distribution and the owner task's CPU time.
8. Images that link the SD core without `SUNXI_MMC_READ_ONLY` (the earlier driver probes) are write-capable to the runner and need a rebuild with that definition, or a coordinator approval record, before they run again.
9. Resolve packer licensing and exact source equivalence.

Any physical card write needs its full host backup and the approved target-bound path in [goal section 4](../goal.md).

## Evidence

Board results are records in `docs/evidence/storage.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-storage-20261101.md).

<!-- evidence-table:begin -->
4 records in `docs/evidence/storage.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793601879959343280-f428558f` | 2026-11-02 | 3-1 | `b74bffc8fd32` | watchdog-reset | failed | run_total 30.086 s, reads_done 0 |
| `1793601985766378587-f5397f2b` | 2026-11-02 | 3-1 | `10fc60a38e63` | watchdog-reset | failed | run_total 30.184 s, reads_done 0 |
| `1793602100319385552-a0551753` | 2026-11-02 | 3-1 | `90c035539d6b` | returned | passed | run_total 12.159 s, sustained_rate 19.7 MB/s, per_read_rate_median 21.92 MB/s, per_read_rate_min 21.21 MB/s, per_read_rate_max 21.95 MB/s, bytes_read 2.09715e+07 B, reads 160, identify_time 0.299 s, card_capacity 6.2535e+10 B |
| `1793602559946667231-3a060254` | 2026-11-02 | 3-1 | `2c3d87a420e7` | returned | passed | run_total 11.55 s, free_clusters 1.90744e+06, clusters 1.90744e+06, cluster_size 32768 B, fat_sectors 14912, root_entries 0, run_time 0.728 s |
<!-- evidence-table:end -->
