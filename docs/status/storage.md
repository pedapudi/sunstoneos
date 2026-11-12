# Storage and autonomous card boot

**Status: implemented.**

Native SD identification, bounded reads, backup-first updates, slot loading, cold entry, descriptor-DMA reads, the FAT32 write service and UHS-I SDR104 reads have software evidence; reads, bounded writes and SDR104 reads on the 62.5 GB and 32 GB cards are board-qualified within the scope below. Neither board has populated eMMC or UFS (host-tested only); the failed UFS probe stays disabled.

**Board-qualified scope:** Returning microSD PIO reads at 50 MHz, four-bit, 3.3 V, one 4 KiB and one 64 KiB sixteen-descriptor DMA read matching same-card PIO references, restoring controller, clocks, pins and saved RAM before normal ROM return. Read-only SD Status (ACMD13), FAT32 volume mount and 160 sequential 128 KiB reads (port 3-1). Bounded CMD25 writes on port 3-1: a 512 MiB file with append, one interruption with recovery, and read-back, through the OS image's SD owner task. Read-only SDR104 at 150 MHz on port 3-3, reached by re-entering a card at 1.8 V, with the bank returned to 3.3 V at close, and bounded writes to the scratch card on port 3-3 at SDR25 (50 MHz) after the write step-down from SDR104 and SDR50. No other card, write at 100 or 150 MHz, or autonomous boot is qualified. Any physical card write needs its full host backup and the approved target-bound path in [goal section 4](../goal.md).

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| PIO reads, 50 MHz, four-bit, 3.3 V | qualified on the board | 21.4 to 21.9 MB/s per request | [SD card reference](../sd-card.md); `1793602100319385552-a0551753` |
| Descriptor-DMA read, 64 KiB, sixteen descriptors | qualified on the board | transfer 3,332 us; CRCs match PIO | [SD card reference](../sd-card.md); `sd-dma.jsonl` `sd-dma-chain-read` |
| SD Status (ACMD13), FAT32 mount | qualified on the board | allocation unit 4 MiB (8,192 sectors) in 519 us; 457 sectors backed up | `1793612289390889939-c86b87e7`, `1793602559946667231-3a060254` |
| FAT32 write service, CMD25 DMA writes | qualified on the board | 14.7 MB/s flushed; flush p50 256 ms, p99 803 ms; recovery after a cut writes 1,536 bytes | [SD card reference](../sd-card.md); `storage-write-qualification` |
| Re-entry of a card at 1.8 V, SDR25 | qualified on the board | 21.4 MB/s per request | [SD card reference](../sd-card.md); `1793649661736969334-9742cb48` |
| UHS-I SDR104 at 150 MHz, read only | qualified on the board | 47.6 MB/s per request, 27.4 MB/s sustained; window 16 to 63, delay 40 | [SD card reference](../sd-card.md); `1793650532228649180-1fb49397` |
| Session-start resolution algorithm | host-tested | 256 cut sessions all usable | [SD card reference](../sd-card.md) |
| File-create module | host-tested | 326-sector backup | [region-test-file-write](../designs/region-test-file-write.md) |
| FAT32 read cursor | host-tested | 511 links for 256 KiB, against 67,667 | `storage.jsonl` `storage-fat32-read-cursor` |
| UHS-I SDR104 and SDR50 reads in the OS owner, port 3-3 | qualified on the board | 440,401,920-byte file read back at 46.8 MB/s, 0 mismatches | [SD card reference](../sd-card.md); `1793689311355151182-d43861f7` |
| Writes at 1.8 V on scratch card 3-3, SDR104 at 150 MHz | qualified on the board | 512 MiB in 42 s, 13.2 MB/s flushed (card-bound), 0 restarts, 0 mismatches on read-back at 46.5 MB/s | [SD card reference](../sd-card.md); `1793692393910115727-f58db0e9`, `1793692075400545011-09988217` |
| Scratch-card admission (`SSBW0004`) and mode 9 reformat | qualified on the board | format of 15,286,272 bytes | [SD card reference](../sd-card.md) |
| DDR50, GPT, multiple volumes | open | | |
| Resident card bundle and update plans | host-tested | Four authenticated extents; physical qualification remains false | [Card design](../designs/autonomous-card-os.md); `card-boot-typed-bundle-host` |
| Autonomous OS boot from the card | open | No identified full backup or production slot reservation | [Card design](../designs/autonomous-card-os.md) |

## Open work

1. Preserve an identified full backup, reserve the slot allocation and provide the queue's physical installation path before seeking storage-write approval ([design](../designs/autonomous-card-os.md)). The resident FEL completion diagnostic passes; cold boot, BOOT-input renewal, reset retirement and installation interruption recovery remain unqualified.
2. Extend the image-level SD checks to the profile 2 and profile 3 images. `checks/storage-read-service.py` (two-client, cancellation, removal and USB cases), `checks/storage-read-sd-modes.py` and `checks/storage-read-dma.py` run in the gate against the profile 1 read image, and `checks/sd-return-native.py` against the returning image. The service cases stop at the first read of the profile 2 image, inside the native aperture filter (`allowed` in `drivers/storage/a733_mmc_platform_native.c`), because only the sd-modes machine models that profile's sequence (CMD11, the PF selector write, clock phases and CMD19 tuning). Move that sequencing into the shared machine of `checks/storage-read-native.py`. The SD path is described in [the SD card reference](../sd-card.md). The board documents disagree on whether DC1SW2 powers the card, so no supply cycle is attempted.
3. Measure sustained throughput on broader extents and cards, and qualify card replacement, removal and recovery, and voltage timing.
4. Implement SDR50 and DDR50; add GPT, multiple volumes and clients.
5. Write qualification beyond one cut: more interruption points, other cards, directories other than `/sunstone-test/`, concurrent writers.
6. Map kernel and driver data EL1-only (backlog, wider than storage): an EL0 application can reach the bounds object of the write service, the private copy of its blob and the controller state; the service detects a changed blob by re-hashing it on every use ([memory protection](../memory-protection.md#backlog-kernel-and-driver-data-are-reachable-from-el0)).
7. Try the busy-clear interrupt (`THLD.BCIG`, `RINTSTS` bits 17 and 13) in place of the DAT0 busy timer, and compare the flush-latency distribution and the owner task's CPU time.
8. Images that link the SD core without `SUNXI_MMC_READ_ONLY` (the earlier driver probes) are write-capable to the runner and need a rebuild with that definition, or a coordinator approval record, before they run again.
9. Switch the application's verify to `fwr_read_cursor` and run it on the board.

## Evidence

Board results are records in [`docs/evidence/storage.jsonl`](../evidence/storage.jsonl); add one with `tools/evidence.py add`. Design records are in [sd-uhs](../evidence/sd-uhs.jsonl), [sd-uhs-recovery](../evidence/sd-uhs-recovery.jsonl), [sd-dma](../evidence/sd-dma.jsonl), [storage-write](../evidence/storage-write.jsonl), [mmc-multiblock-write](../evidence/mmc-multiblock-write.jsonl), [region-test-file-write](../evidence/region-test-file-write.jsonl) and [spi-native-dma](../evidence/spi-native-dma.jsonl). Earlier prose evidence is in [the historical log](../history/status-storage-20261101.md).

<!-- evidence-table:begin -->
67 records in `docs/evidence/storage.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `sd-uhs-adoption-write-phases` |  |  | `` | not-run | passed |  |
| `storage-fat32-read-cursor` |  |  | `` | not-run | passed | links_256kib_with_cursor 511, links_1mib_with_cursor 2047, links_256kib_without_cursor 67667, links_1mib_without_cursor 1.0751e+06 |
| `storage-host-coverage-gap` |  |  | `` | not-run | inconclusive |  |
| `storage-interrupt-disabled-throughput` |  |  | `` | not-run | inconclusive |  |
| `storage-write-qualification` |  |  | `` | returned | passed | read_rate 21.7 MB/s, write_rate_flushed 14.7 MB/s, flush_latency_p50 256 ms, flush_latency_p99 803 ms, recovery_metadata_bytes 1536 B |
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
| `1793639891731890732-2700d966` | 2026-11-02 | 3-1 | `01b107634de6` | result-mismatch | passed | run_total 28.085 s, s18a 1, group1_support_bits 32771, au_sectors 8192, max_current 50 mA, group1_support_predicted 31, group1_support_measured 32771 |
| `1793648082135831049-05f70d50` | 2026-11-02 | 3-3 | `9bd1f1ba6475` | watchdog-reset | failed | run_total 29.298 s, reads_done 32, platform_stage 18 |
| `1793649204965774597-d1ed3e86` | 2026-11-02 | 3-3 | `962eb56c8103` | watchdog-reset | inconclusive | run_total 110.438 s, reads_done 32, s18a 0, group1_support_bits 31, crc_fold 93 |
| `1793649661736969334-9742cb48` | 2026-11-02 | 3-3 | `94e4be87f6dd` | result-mismatch | passed | run_total 13.16 s, reads_done 32, per_read_rate_median 21.4 MB/s, identify_time 0.309 s, s18a 0, group1_support_bits 31 |
| `1793650091979550389-eb009a18` | 2026-11-02 | 3-3 | `003fa9e0e17f` | watchdog-reset | failed | run_total 111.186 s |
| `1793650532228649180-1fb49397` | 2026-11-02 | 3-3 | `aa20988e728c` | result-mismatch | inconclusive | run_total 49.23 s, card_clock 150 MHz, tuning_window 48, chosen_delay 40, per_request_rate_median 47.6 MB/s, sustained_rate 27.4 MB/s, bytes_read 6.71089e+07 B, errors 0, tuning_window_passing_delays 58, first_32_median_request 2.51 ms, first_32_rate 52.2 MB/s, all_requests_median 2.76 ms, all_requests_rate 47.6 MB/s, sustained_time 2.446 s, identify_time_with_sweep 0.893 s, requests_over_5ms 127, probe_gap_per_request 0.67 ms |
| `1793650854687556052-db9aee9e` | 2026-11-02 | 3-3 | `9f261cc92409` | watchdog-reset | failed | run_total 75.048 s |
| `1793651292191500240-5493408d` | 2026-11-02 | 3-3 | `31aba5df4dc3` | watchdog-reset | failed | run_total 29.309 s |
| `1793652351260245353-286b63df` | 2026-11-02 | 3-3 | `851a4859bdd6` | result-mismatch | passed | run_total 43.543 s, run_total 43.543 s, card_clock 150 MHz, tuning_window 47, chosen_delay 40, phase_a_mean 5.27 ms, phase_b_mean 5.42 ms, phase_c_mean 6.73 ms, phase_d_mean 4.82 ms, phase_e_mean 1.93 ms, phase_f_mean 2.44 ms, phase_g_mean 3.21 ms, phase_h_mean 6.66 ms, bytes_read 1.25829e+08 B, sustained_rate_all_phases 20.4 MB/s |
| `1793663873502474507-76c206d4` | 2026-11-02 | 3-3 | `15c9d9cdc98d` | result-mismatch | passed | run_total 13.171 s, run_total 0 s, identify_time 0.299 s, s18a 0, group1_support_bits 32799, per_read_rate_median 21.43 MB/s, reads_done 32 |
| `1793666413836584828-d698c21f` | 2026-11-02 | 3-3 | `4ba6d98e7c85` | watchdog-reset | failed | run_total 145.493 s, run_total 0 s |
| `1793666839062917650-d5d01b71` | 2026-11-02 | 3-3 | `5beae9c6b263` | watchdog-reset | failed | run_total 123.102 s, run_total 0 s |
| `1793667245855934208-55d19b3c` | 2026-11-02 | 3-3 | `ea5720010089` | result-mismatch | passed | run_total 148.541 s, run_total 0 s, sdr104_request 2.445 ms, sdr104_retuned_request 2.445 ms, sdr50_request 3.342 ms, sdr25_request 6.059 ms, sdr104_window 48, sdr50_window 64, reads_done 128 |
| `sd-uhs-adoption-3-1-switch` | 2026-11-02 |  | `` | not-run | inconclusive |  |
| `sd-uhs-adoption-3-3-layout-capture` | 2026-11-02 | 3-3 | `99c966ae6350` | result-mismatch | inconclusive |  |
| `sd-uhs-adoption-scratch-card-plan` | 2026-11-02 |  | `` | not-run | inconclusive |  |
| `sd-uhs-adoption-write-second-attempt` | 2026-11-02 |  | `` | not-run | inconclusive |  |
| `sd-uhs-adoption-write-table` | 2026-11-02 |  | `` | returned | inconclusive |  |
| `1793683567666621650-3e3c6631` | 2026-11-03 | 3-3 | `5c866e6806ef` | watchdog-reset | inconclusive |  |
| `1793683923163699289-3c296ae6` | 2026-11-03 | 3-3 | `99c966ae6350` | result-mismatch | inconclusive |  |
| `1793683932704369296-a2880fff` | 2026-11-03 | 3-3 | `99c966ae6350` | result-mismatch | inconclusive |  |
| `1793685607294001067-6c6ebb1d` | 2026-11-03 | 3-3 | `c4ee679e857c` | watchdog-reset | inconclusive |  |
| `1793686672190595430-c7085caf` | 2026-11-03 | 3-3 | `8f5a453e9d8f` | refused-before-launch | inconclusive |  |
| `1793687494152436674-9dcd2cb2` | 2026-11-03 | 3-3 | `8f5a453e9d8f` | watchdog-reset | inconclusive |  |
| `1793687913540627897-0dcbebfc` | 2026-11-03 | 3-3 | `38edac6cf0f7` | watchdog-reset | inconclusive |  |
| `1793688426368337098-abb94372` | 2026-11-03 | 3-3 | `e4457f8e655b` | watchdog-reset | inconclusive |  |
| `1793688694680089992-4e190fcd` | 2026-11-03 | 3-3 | `3899a8578c19` | watchdog-reset | inconclusive |  |
| `1793689119969310811-abcfb6bc` | 2026-11-03 | 3-3 | `3e308c9c6222` | returned | inconclusive |  |
| `1793689196470444588-1be26413` | 2026-11-03 | 3-3 | `3e308c9c6222` | result-mismatch | inconclusive |  |
| `1793689311355151182-d43861f7` | 2026-11-03 | 3-3 | `3e308c9c6222` | returned | inconclusive |  |
| `1793689497237363008-de0d289f` | 2026-11-03 | 3-3 | `83bfe0ba5a1b` | returned | inconclusive |  |
| `1793690349419475462-69bd6c78` | 2026-11-03 | 3-3 | `7d8a20d78016` | returned | failed | run_total 24.42 s, written_bytes 1.52863e+07 B, restarts 4 |
| `1793690518271950065-2379bd2f` | 2026-11-03 | 3-3 | `2cd3d2ea0ecc` | returned | passed | run_total 21.683 s, written_bytes 1.52863e+07 B, restarts 0, owner_time 1.01 s |
| `1793690551898353510-7e2b465d` | 2026-11-03 | 3-3 | `2cd3d2ea0ecc` | watchdog-reset | failed | run_total 21.854 s, run_total 21.9 s |
| `1793690860427726046-3bcb9231` | 2026-11-03 | 3-3 | `fd9b50bd779f` | returned | passed | run_total 21.466 s, written_bytes 1.52863e+07 B, restarts 0 |
| `1793690890150131786-bfd9daeb` | 2026-11-03 | 3-3 | `fd9b50bd779f` | watchdog-reset | failed | run_total 22.79 s, run_total 21.9 s |
| `1793691370707308356-ea4277f1` | 2026-11-03 | 3-3 | `21fd17502717` | watchdog-reset | failed | run_total 21.144 s, run_total 21.1 s |
| `1793691438173510313-2620933c` | 2026-11-03 | 3-3 | `21fd17502717` | watchdog-reset | failed | run_total 21.625 s, run_total 21.1 s |
| `1793691730130275875-f98a0644` | 2026-11-03 | 3-3 | `e3541922da29` | returned | passed | run_total 21.586 s, written_bytes 1.52863e+07 B, restarts 0 |
| `1793691763257856580-edf2f59c` | 2026-11-03 | 3-3 | `e3541922da29` | watchdog-reset | failed | run_total 22.741 s, run_total 21.9 s |
| `1793692002289431339-7dfb06bb` | 2026-11-03 | 3-3 | `215ff6f2fcf2` | returned | passed | run_total 61.47 s, written_bytes 5.36871e+08 B, flush_rate 13.5 MB/s, flush_latency_p50 336 ms, flush_latency_p99 703 ms, restarts 0 |
| `1793692075400545011-09988217` | 2026-11-03 | 3-3 | `215ff6f2fcf2` | returned | passed | run_total 33.819 s, verified_bytes 5.36871e+08 B, mismatches 0, read_rate 46.5 MB/s |
| `1793692362723456455-361d643b` | 2026-11-03 | 3-3 | `cb9bb22b5303` | returned | passed | run_total 22.211 s, written_bytes 1.52863e+07 B, busy_timeouts 1, restarts 0 |
| `1793692393910115727-f58db0e9` | 2026-11-03 | 3-3 | `cb9bb22b5303` | returned | passed | run_total 62.497 s, written_bytes 5.36871e+08 B, flush_rate 13.2 MB/s, busy_timeouts 15, restarts 0 |
<!-- evidence-table:end -->
