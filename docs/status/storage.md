# Storage and autonomous card boot

**Status: implemented.**

Native SD identification, bounded reads, backup-first updates, slot loading and cold entry have software evidence. Neither current board has populated eMMC or UFS. Keep the failed UFS probe disabled.


**Board-qualified scope:** Returning microSD PIO reads at 50 MHz, four-bit,
3.3 V complete. One 4 KiB DMA read and one sentinel-prefilled 64 KiB read match
same-card PIO references and restore controller, clocks, pins and saved RAM
before normal ROM return. The sixteen-descriptor 64 KiB transfer takes
3,332 microseconds; it does not measure sustained throughput.
The SD Status read (ACMD13) returns the card's allocation unit, 4 MiB (`AU_SIZE` 9, 8,192 sectors), in 519 microseconds, and the
read-only backup probe returned 457 listed sectors with the restoration words unchanged. A read-only probe
on port 3-1 read 160 sequential 128 KiB requests at 21.9 MB/s per request in the
same profile; it ran at boot clocks (CPU 494 MHz), so the figure is not a top-point
performance figure and the OS image re-measures it. The same probe mounted the card's
FAT32 volume read-only (32 KiB clusters, 1,907,436 clusters free, empty root directory).

## Open work

1. Qualify physical installation, autonomous OS boot, cold controller and supply state, reset retirement and power-loss recovery.
2. Finish the UHS-I fast mode. Host-checked and built, not yet run on a board in the OS: the SD owner (`apps/os/large_sd.c`) opens in the UHS-I profile and selects the best mode the card and host share: SDR104 at 150 MHz, SDR50 at 100 MHz (both tuned), SDR25 at 50 MHz, SDR12 at 25 MHz, and for a 3.3 V card high speed or default speed. A card found at 3.3 V that already signals 1.8 V (S18A clear with SDR50 or SDR104 in the CMD6 support field) is read once, closed and opened again with the bank at 1.8 V. A failed read identifies again (a new tuning) and then steps the mode down; the mode and every step are in `os_sd_mode` (`ss_storage_mode`) and in `storage-write-test`'s result. See [the adoption design](../designs/sd-uhs-adoption.md) and [the recovery design](../designs/sd-uhs-recovery.md).
   - Seen on a board, read-only, on **3-3** (Phison SD32G, 2020; the card is at 1.8 V until board power is removed): re-entry at 1.8 V; SDR104 at 150 MHz (window 16 to 63, 2.445 ms per 128 KiB request, 53.6 MB/s); SDR50 at 100 MHz (all 64 delays pass, 3.342 ms, 39.2 MB/s); SDR25 at 50 MHz (6.059 ms, 21.6 MB/s); a restart of the identification at 1.8 V under each ceiling with no CMD11 and no PF write; a 3.3 V host reads this card at 1.8 V (OCR S18A clear, support `0x801f`). **3-1** (Phison SD64G, 2026): 3.3 V high speed only (S18A set, support `0x8003`); its switch to 1.8 V is predicted in the adoption design and not yet run.
   - Not yet done: the OS read run and the bounded write at 1.8 V on 3-3 (the write at 150 MHz uses the vendor's drive phases, derived in the adoption design and pinned by a host check; no board has written at 1.8 V); the switch on 3-1; the seven image-level SD checks in the gate (the checks do not run on the current image: see the next item).
3. Measure sustained throughput on broader extents and cards, and qualify card replacement, removal and recovery, and voltage timing.
4. Implement SDR50 and DDR50; add GPT, multiple volumes and clients.
5. Connect the [FAT32 write service](../designs/storage-write.md) (host-checked with a power-loss sweep) to the card: CMD25 with descriptor DMA, ACMD13 for the allocation unit, a read-over-write arbiter, bounded-write admission in the runner, and the board rate, latency and interruption measurements.
6. Map kernel and driver data EL1-only (backlog, wider than storage): an EL0 application can reach the bounds object of the write service, the private copy of its blob and the controller state; the service detects a changed blob by re-hashing it on every use ([memory protection](../memory-protection.md#backlog-kernel-and-driver-data-are-reachable-from-el0)).
7. After the first write has run, try the busy-clear interrupt (`THLD.BCIG`, `RINTSTS` bits 17 and 13) in place of the DAT0 busy timer, and compare the flush-latency distribution and the owner task's CPU time.
8. Images that link the SD core without `SUNXI_MMC_READ_ONLY` (the earlier driver probes) are write-capable to the runner and need a rebuild with that definition, or a coordinator approval record, before they run again.
9. Fixed on the host, not yet run on the board: reads through the file API were quadratic in file size, because `fwr_read` resolved the path, scanned the directory and walked the chain from the first cluster on every call (5,888 reads of 64 KiB over the 368 MiB file did not finish in 52 s, entry `1793625959251868124-3ed648e5`). `struct fwr_cursor` and `fwr_read_cursor` (`include/fat32_rw.h`) keep the file's first cluster, size and the cluster of the last byte read, so a sequential read continues from there and a read behind it walks from the start; `ss_read` keeps one cursor per descriptor. Any directory write on the volume bumps a generation count that makes the cursor resolve again, so it never serves a stale size or chain. `checks/fat32-read-cursor.py` requires one resolution and at most one link per cluster crossed (511 and 2,047 links for 256 KiB and 1 MiB, against 67,667 and 1,075,100 without the cursor) and the data to match after an append, a replace and an unlink. The application's verify still reads in 4 MiB steps with `fwr_read` until it is switched to the cursor and run on the board.

Any physical card write needs its full host backup and the approved target-bound path in [goal section 4](../goal.md).

## Qualified scope of the write service

The service is board-qualified for bounded writes on the 62.5 GB card (Class 10, U3, V30, A1, 4 MiB allocation unit; profile 2: high speed, four-bit, 50 MHz), through the OS image's SD owner task and the runner's admission of a blob that names every write and backup range:

- **Bounded writes:** a 512 MiB file `/sunstone-test/sunstone-region-test.bin` created in a preallocated, contiguous chain (cluster 47 to 16430), verified end to end by a read-only open (mode 3, 0 mismatches) and by the read-only probe (chain whole, FAT copies equal, FAT[1] clean, FSInfo as predicted).
- **Append:** 144 MiB appended to the existing file in a later session with the same checks; the file reached its full size with the chain extended and FSInfo, directory entry and both FAT copies consistent.
- **Interruption and recovery:** the OS ended a session at its own deadline in the middle of a flush while the application wrote a new file with a sync every 8 MiB. The card was left with FAT[1] dirty, the new file's entry at 40 MiB (five syncs), a chain of 1,280 clusters and FSInfo unchanged, with the unsynced clusters unlinked. The next session mounted with the writer enabled; recovery ran once, wrote 1,536 bytes of metadata (the FAT copies and FSInfo), set FAT[1] clean, and the 40 MiB file read back with 0 mismatches. The region file read back whole afterwards (512 MiB, 0 mismatches, no card write). Recovery left the FSInfo next-free hint at 4, the lowest free cluster; that is now the first free cluster after the highest used one (or the old hint while it is still free), checked by `checks/fat32-rw.py`.
- **Throughput:** reads 21.7 MB/s (6.0 ms per 128 KiB request); writes 14.7 MB/s flushed (about 8 ms per 128 KiB piece; flush latency p50 256 ms, p99 803 ms), both with the interrupt enabled, at the top CPU point.
- **Per-session backup procedure:** the blob names the backup's SHA-256 and the enable re-reads the backup ranges and refuses unless the hash matches. A session changes the backed-up sectors, so every write session needs a fresh read-only capture (`apps/sd-backup-probe`), a new blob and plan from it, and a coordinator approval record keyed by the blob and image hashes. The first capture (`backup-395-sectors.bin`, the card before any write) is the restore point and is never replaced. Read-only modes (`ss_storage_open_read`) bind no bounds and run on a changed card. The procedure is in [the design](../designs/storage-write.md#bounded-first-write-on-the-625-gb-card).
- **Evidence:** the records below, one per run, with the entries under `~/.cache/sunstoneos/board-queue/entries/`. The runs of the interruption test are `1793635810117434075-86ece323` (the cut), `1793635995914258228-cb1f4fc3` (probe after the cut), `1793636210564944400-a065537e` (recovery), `1793636300522250962-f6977cff` (probe after recovery) and `1793636342034468404-7527d869` (region file read).

Not qualified: more than one interruption point (the host power-loss sweep covers every sector write; the board has one cut), other cards, writes outside `/sunstone-test/`, directories other than the existing one, and concurrent writers.

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
31 records in `docs/evidence/storage.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793601879959343280-f428558f` | 2026-11-02 | 3-1 | `b74bffc8fd32` | watchdog-reset | failed | run_total 30.086 s, reads_done 0 |
| `1793601985766378587-f5397f2b` | 2026-11-02 | 3-1 | `10fc60a38e63` | watchdog-reset | failed | run_total 30.184 s, reads_done 0 |
| `1793602100319385552-a0551753` | 2026-11-02 | 3-1 | `90c035539d6b` | returned | passed | run_total 12.159 s, sustained_rate 19.7 MB/s, per_read_rate_median 21.92 MB/s, per_read_rate_min 21.21 MB/s, per_read_rate_max 21.95 MB/s, bytes_read 2.09715e+07 B, reads 160, identify_time 0.299 s, card_capacity 6.2535e+10 B |
| `1793602559946667231-3a060254` | 2026-11-02 | 3-1 | `2c3d87a420e7` | returned | passed | run_total 11.55 s, free_clusters 1.90744e+06, clusters 1.90744e+06, cluster_size 32768 B, fat_sectors 14912, root_entries 0, run_time 0.728 s |
| `1793612289390889939-c86b87e7` | 2026-11-02 | 3-1 | `190cbba10653` | returned | passed | run_total 13.107 s, allocation_unit 8192, sd_status_read 519 us, identify_time 0.298816 s, backup_sectors 457, card_sectors 1.22139e+08 |
| `1793623788842205312-60954030` | 2026-11-02 | 3-1 | `dbcd215abfcf` | watchdog-reset | inconclusive | run_total 117.595 s, bytes_written 3.85876e+08 B, guard 40 s |
| `1793624020209550742-922a29db` | 2026-11-02 | 3-1 | `190cbba10653` | result-mismatch | passed | run_total 51.094 s, file_size 3.85876e+08 B, chain_clusters 11776, sectors_changed 189 |
| `1793625959251868124-3ed648e5` | 2026-11-02 | 3-1 | `5f83dbc1f8da` | watchdog-reset | inconclusive | run_total 70.944 s, run_limit 52 s, read_calls 5888 |
| `1793631795893042045-0c63a1cc` | 2026-11-02 | 3-1 | `64b8e3bb6957` | result-mismatch | inconclusive | run_total 60.069 s, verified_bytes 2.4327e+08 B, read_rate 6.1 MB/s, mismatches 0, irq_entries 0, owner_wakes 15094 |
| `1793632286777262813-62b0f3bb` | 2026-11-02 | 3-1 | `eed520d8aa4d` | returned | passed | run_total 64.471 s, verified_bytes 3.85876e+08 B, read_rate 21.7 MB/s, mismatches 0, total_time 19.07 s |
| `1793632378497932347-1618d4a8` | 2026-11-02 | 3-1 | `eed520d8aa4d` | returned | passed | run_total 79.176 s, appended 1.50995e+08 B, session_time 10.69 s, flush_rate 14.7 MB/s, flush_p50 256 ms, flush_p99 803 ms, generate_time 0.34 s |
| `1793632483097936083-744016a7` | 2026-11-02 | 3-1 | `190cbba10653` | result-mismatch | passed | run_total 35.738 s, file_size 5.36871e+08 B, chain_clusters 16384 |
| `1793632486531867079-fd56b38d` | 2026-11-02 | 3-1 | `eed520d8aa4d` | result-mismatch | passed | run_total 86.502 s, verified_bytes 5.36871e+08 B, read_rate 21.7 MB/s, mismatches 0, total_time 26.47 s |
| `1793635810117434075-86ece323` | 2026-11-02 | 3-1 | `e57491e16932` | watchdog-reset | passed | run_total 105.425 s, session_deadline 55 s |
| `1793635995914258228-cb1f4fc3` | 2026-11-02 | 3-1 | `190cbba10653` | result-mismatch | passed | run_total 45.603 s, sectors_captured 109 |
| `1793636210564944400-a065537e` | 2026-11-02 | 3-1 | `e57491e16932` | returned | passed | run_total 62.612 s, recoveries 1, recovered_clusters 0, volume_size 4.1943e+07 B, mismatches 0, wear_bytes 1536 B |
| `1793636300522250962-f6977cff` | 2026-11-02 | 3-1 | `190cbba10653` | result-mismatch | passed | run_total 12.863 s, sectors_captured 109 |
| `1793636342034468404-7527d869` | 2026-11-02 | 3-1 | `e57491e16932` | returned | passed | run_total 45.779 s, verified_bytes 5.36871e+08 B, mismatches 0, read_rate 20.3 MB/s, wear_bytes 0 B |
| `1793639891731890732-2700d966` | 2026-11-02 | 3-1 | `01b107634de6` | result-mismatch | passed | run_total 28.085 s, s18a 1, group1_support_bits 32771, au_sectors 8192, max_current 50 mA |
| `1793648082135831049-05f70d50` | 2026-11-02 | 3-3 | `9bd1f1ba6475` | watchdog-reset | failed | run_total 29.298 s, reads_done 32 |
| `1793649204965774597-d1ed3e86` | 2026-11-02 | 3-3 | `962eb56c8103` | watchdog-reset | inconclusive | run_total 110.438 s, reads_done 32, s18a 0, group1_support_bits 31 |
| `1793649661736969334-9742cb48` | 2026-11-02 | 3-3 | `94e4be87f6dd` | result-mismatch | passed | run_total 13.16 s, reads_done 32, per_read_rate_median 21.4 MB/s, identify_time 0.309 s, s18a 0, group1_support_bits 31 |
| `1793650091979550389-eb009a18` | 2026-11-02 | 3-3 | `003fa9e0e17f` | watchdog-reset | failed | run_total 111.186 s |
| `1793650532228649180-1fb49397` | 2026-11-02 | 3-3 | `aa20988e728c` | result-mismatch | inconclusive | run_total 49.23 s, card_clock 150 MHz, tuning_window 48, chosen_delay 40, per_request_rate_median 47.6 MB/s, sustained_rate 27.4 MB/s, bytes_read 6.71089e+07 B, errors 0 |
| `1793650854687556052-db9aee9e` | 2026-11-02 | 3-3 | `9f261cc92409` | watchdog-reset | failed | run_total 75.048 s |
| `1793651292191500240-5493408d` | 2026-11-02 | 3-3 | `31aba5df4dc3` | watchdog-reset | failed | run_total 29.309 s |
| `1793652351260245353-286b63df` | 2026-11-02 | 3-3 | `851a4859bdd6` | result-mismatch | passed | run_total 43.543 s, run_total 43.543 s, card_clock 150 MHz, tuning_window 47, chosen_delay 40, phase_a_mean 5.27 ms, phase_b_mean 5.42 ms, phase_c_mean 6.73 ms, phase_d_mean 4.82 ms, phase_e_mean 1.93 ms, phase_f_mean 2.44 ms, phase_g_mean 3.21 ms, phase_h_mean 6.66 ms, bytes_read 1.25829e+08 B, sustained_rate_all_phases 20.4 MB/s |
| `1793663873502474507-76c206d4` | 2026-11-02 | 3-3 | `15c9d9cdc98d` | result-mismatch | passed | run_total 13.171 s, run_total 0 s, identify_time 0.299 s, s18a 0, group1_support_bits 32799, per_read_rate_median 21.43 MB/s, reads_done 32 |
| `1793666413836584828-d698c21f` | 2026-11-02 | 3-3 | `4ba6d98e7c85` | watchdog-reset | failed | run_total 145.493 s, run_total 0 s |
| `1793666839062917650-d5d01b71` | 2026-11-02 | 3-3 | `5beae9c6b263` | watchdog-reset | failed | run_total 123.102 s, run_total 0 s |
| `1793667245855934208-55d19b3c` | 2026-11-02 | 3-3 | `ea5720010089` | result-mismatch | passed | run_total 148.541 s, run_total 0 s, sdr104_request 2.445 ms, sdr104_retuned_request 2.445 ms, sdr50_request 3.342 ms, sdr25_request 6.059 ms, sdr104_window 48, sdr50_window 64, reads_done 128 |
<!-- evidence-table:end -->
