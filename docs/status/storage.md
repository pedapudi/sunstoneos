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
10. Open defect: reads through the file API are quadratic in file size. `fwr_read` (`drivers/storage/fat32_rw.c`) takes a path and an offset, resolves the path, scans the directory for the entry and walks the cluster chain from the first cluster to the offset on every call, so reading a file in small pieces costs time proportional to the pieces times the file size. Measured: 5,888 reads of 64 KiB over the 368 MiB test file did not finish in the application's 52 s on the board (v15 mode 2, entry `1793625959251868124-3ed648e5`); the same read in steps of up to 4 MiB is the workaround in `apps/storage-write-test/main.c`. mimus reads through this API in small pieces on every platform. Required: an open file handle keeps its current cluster and offset, so a sequential read or write continues from there, and a seek walks forward from the cached position when the target is ahead (from the start only when it is behind), with the directory entry and size cached in the handle. The write path does not have the cost: an open writer keeps its tail cluster, chained count and reserved runs (`contig_from` is a loop over the reserved runs, `fwr_write` a copy into the flush buffer), the directory entry location is cached at open, and the chain is walked once, at an append open. The fix is a separate change after the test file is complete.

Any physical card write needs its full host backup and the approved target-bound path in [goal section 4](../goal.md).

## Throughput figures measured with the controller interrupt disabled are superseded

Until the fix that sets `GCTRL` bit 4 with the interrupt mask (the board kernel's global interrupt enable, `sunxi_mmc_init_host`), the SD0 interrupt never reached the owner and every wait for a transfer ended on the 5 ms recheck timer. The earlier write throughput (368 MiB in at most 40 s, about 9.2 MiB/s, which included stream generation), the read rate of the mode 2 runs (6.1 MB/s over 232 MiB, 5.4 ms per request) and any per-request latency from the runs before the fix are therefore not the card's or the driver's rates. They are superseded by the runs of the image with the interrupt enabled; the result then carries `irq_entries` per core and `owner_recheck_wakes`, which should be about one per request and about zero. The 21.9 MB/s read figure of the standalone probe was measured by polling and is unaffected.

## A class of host-coverage gap

The platform's admission of the bounded write had no host coverage until `checks/a733-mmc-platform-write.py`. The strict CMD25 model
(`checks/sunxi-mmc-write-dma.c`) drives the core with the model's own callbacks, so it never ran the platform's descriptor-chain check
(`dma_chain` in `drivers/storage/a733_mmc_platform.c`), which refused every write-direction command. The first board writes, runs v11 and v12,
stopped there: the controller was retained with no refused register and no deadline. The class of gap is a strict model tested with its own
callbacks instead of the platform's: the model proves the core against the protocol and proves nothing about the layer that admits the core's accesses.
A model of a layer's callee must be driven through that layer. `checks/a733-mmc-platform-write.py` now runs the same model through the platform's core
callbacks and the native adapter's register filter with a verified bounds object, and `checks/a733-mmc-dma-chain.py` pins the predicate. Both fail
on the platform source of v12. The same check applies to any other service that gets a strict model for its core.

## Evidence

Board results are records in `docs/evidence/storage.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-storage-20261101.md).

<!-- evidence-table:begin -->
12 records in `docs/evidence/storage.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793601879959343280-f428558f` | 2026-11-02 | 3-1 | `b74bffc8fd32` | watchdog-reset | failed | run_total 30.086 s, reads_done 0 |
| `1793601985766378587-f5397f2b` | 2026-11-02 | 3-1 | `10fc60a38e63` | watchdog-reset | failed | run_total 30.184 s, reads_done 0 |
| `1793602100319385552-a0551753` | 2026-11-02 | 3-1 | `90c035539d6b` | returned | passed | run_total 12.159 s, sustained_rate 19.7 MB/s, per_read_rate_median 21.92 MB/s, per_read_rate_min 21.21 MB/s, per_read_rate_max 21.95 MB/s, bytes_read 2.09715e+07 B, reads 160, identify_time 0.299 s, card_capacity 6.2535e+10 B |
| `1793602559946667231-3a060254` | 2026-11-02 | 3-1 | `2c3d87a420e7` | returned | passed | run_total 11.55 s, free_clusters 1.90744e+06, clusters 1.90744e+06, cluster_size 32768 B, fat_sectors 14912, root_entries 0, run_time 0.728 s |
| `1793623788842205312-60954030` | 2026-11-02 | 3-1 | `dbcd215abfcf` | watchdog-reset | inconclusive | run_total 117.595 s, bytes_written 3.85876e+08 B, guard 40 s |
| `1793624020209550742-922a29db` | 2026-11-02 | 3-1 | `190cbba10653` | result-mismatch | passed | run_total 51.094 s, file_size 3.85876e+08 B, chain_clusters 11776, sectors_changed 189 |
| `1793625959251868124-3ed648e5` | 2026-11-02 | 3-1 | `5f83dbc1f8da` | watchdog-reset | inconclusive | run_total 70.944 s, run_limit 52 s, read_calls 5888 |
| `1793631795893042045-0c63a1cc` | 2026-11-02 | 3-1 | `64b8e3bb6957` | result-mismatch | inconclusive | run_total 60.069 s, verified_bytes 2.4327e+08 B, read_rate 6.1 MB/s, mismatches 0, irq_entries 0, owner_wakes 15094 |
| `1793632286777262813-62b0f3bb` | 2026-11-02 | 3-1 | `eed520d8aa4d` | returned | passed | run_total 64.471 s, verified_bytes 3.85876e+08 B, read_rate 21.7 MB/s, mismatches 0, total_time 19.07 s |
| `1793632378497932347-1618d4a8` | 2026-11-02 | 3-1 | `eed520d8aa4d` | returned | passed | run_total 79.176 s, appended 1.50995e+08 B, session_time 10.69 s, flush_rate 14.7 MB/s, flush_p50 256 ms, flush_p99 803 ms, generate_time 0.34 s |
| `1793632483097936083-744016a7` | 2026-11-02 | 3-1 | `190cbba10653` | result-mismatch | passed | run_total 35.738 s, file_size 5.36871e+08 B, chain_clusters 16384 |
| `1793632486531867079-fd56b38d` | 2026-11-02 | 3-1 | `eed520d8aa4d` | result-mismatch | passed | run_total 86.502 s, verified_bytes 5.36871e+08 B, read_rate 21.7 MB/s, mismatches 0, total_time 26.47 s |
<!-- evidence-table:end -->
