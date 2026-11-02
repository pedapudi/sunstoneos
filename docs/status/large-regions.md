# Large application regions

**Status: implemented.**

An application declares regions of upper memory, up to the 11 GiB the board maps, in its manifest. The OS takes them from a ledger before the scheduler starts, gives the application its own address space (ASID-tagged translation tables, switched at the task switch), and a loader task fills each region from a FAT32 file or a raw card extent and checks its SHA-256. Design: [large application regions](../designs/large-app-regions.md); creating the test file: [region test file](../designs/region-test-file-write.md).

**Software evidence:** host, emulator.

**Board-qualified scope:** A read-only probe image (boot clocks, CPU 494 MHz) took a 1 GiB region, ran EL0 routines in three address spaces and showed the isolation, the 2 MiB and 1 GiB block walk counts and one read rate that does not depend on the block size; it read the card at 21.9 MB/s per 128 KiB request and mounted its FAT32 volume. No OS image has run on a board with a large region, and no performance figure at the approved top operating points exists.

## Defect found on a board and fixed

The first run of `sunstone-a733-os-large-region-test` in the CPU-return profile (found by the storage agent on a board) took a data abort in `os_large_prepare`: the admission read the boot level-1 table through `TTBR0_EL1`, and the A733 EL1 table leaves the 2 MiB block that holds the tables unmapped. Admission now reads a copy the platform makes (`os_kernel_l1_head`). The QEMU OS platform maps its tables the same way, and the cases that admit a region fault with ESR 0x96000006 if the read returns. No board run of the fix yet.

## Measured on a board, OS image at the CPU-return top points

Entry `1793632829779717086-f7d0d1cf` (3-1, A76, `PERF_BOOT=high`, development DRAM; repeat of `1793632747850074996-92d6900c`, whose expectation was frozen as raw bytes and so reads result-mismatch). The image includes the table-copy fix, the controller interrupt enable from the storage branch and its 50 s manifest.

- **Load:** the 536,870,912-byte file streamed from SD0 into the first half of the 1 GiB region in 30.659 s, 17.5 MB/s (16.7 MiB/s); the digest matched (state READY, error 0). The design predicted 24 to 27 s: missed by 14 percent. Both runs report the same 30,659,000 us.
- **Task switch:** 20,000 round trips of two notifications between two tasks, 4.40 us per round trip in one space and 4.36 us with a TTBR0 write at each switch. The write adds nothing measurable; the difference is under one counter tick (42 ns) per switch and has the wrong sign.
- **Read bandwidth, one A76 core:** 12.79 GB/s over a 40 MiB window (eight timed passes), 12.66 GB/s over the whole 1 GiB (three passes), 1.0 percent lower. The `apps/dram-scaling` record `read_one_a76` is 12.69 GB/s.
- **Not measured:** TLB refill counts (EL0 cannot read the PMU in the OS image).

## Load time: why 30.7 s against 24.7 s, and the changes

Read from the code (`apps/os/large_load.c`, `apps/os/large_sd.c`, `drivers/storage/fat32_ro.c`, `drivers/storage/sunxi_mmc.c`), before any new run:

- **The clock.** `elapsed_us` was a difference of FreeRTOS tick counts times 1000, so it had a 1 ms step; two loads agreeing to the microsecond meant the same number of ticks, not the same microsecond. It now reads the 24 MHz counter.
- **The pipeline.** `fat32_stream` calls `submit` for the next 128 KiB and then hashes the current piece, so it looks overlapped. It was not: `a733_mmc_platform_read` only accepts the request, and the controller steps that write CMD18 run in the polls that follow. The loader hashes before it polls, so each piece cost the read plus the copy and the digest in sequence. `sd_submit` now steps the controller until the command is issued (`command_step` 2), so the transfer runs while the loader works.
- **The digest.** `storage_digest_add` was plain C, on the loader's pinned core (core 0, an A55). The 6.0 s difference over 4,096 pieces is 1.46 ms per piece, which is 90 MB/s for the copy and the C SHA-256 together, an estimate of about 20 cycles per byte at that core's clock, which is what plain C SHA-256 costs on an in-order A55. The loader runs at EL1, so it reads its own `ID_AA64ISAR0_EL1` and does not depend on the missing CPU observations (`docs/status/cpu-topology.md`, backlog item 7). It now uses the SHA-256 instructions (`drivers/storage/storage_digest_ce.S`) when SHA2 is present, and the C path otherwise. QEMU's A76 has the instructions: the loaded and digest-mismatch cases run the new path, and a wrong round constant fails them.
- **The copy.** There is no bounce buffer: the DMA fills one of the two 128 KiB buffers and the loader copies it once into the region (8 bytes at a time through the cached kernel mapping), then hashes the same buffer. The copy should be a small part of a piece (estimate, not measured: 128 KiB through a cached mapping).
- **Per-phase times.** The loader now records the time spent in the copy and in the digest (`ss_large_status` `copy_us`, `digest_us`; the test application returns them). The reading time is the load time minus those two. The first run before this change has no phase records; the 1.46 ms per piece above is a difference with the storage agent's mode 3, not a measurement.

## Open work

1. Run the SD0 binding in an OS image on a board. `apps/os/large_sd.c` (`os_large_device`, `os_large_device_close`, `os_large_sd_pins`) uses the OS's shared providers and the read-only SD core, builds into `sunstone-a733-os-large-region-test` and passes the read-only scan, but only the standalone probe has driven the card on a board. QEMU has no SD controller: its RAM disk covers the loader, and four scenarios cover a device that is present and fails (metadata, request start, mid-stream, absent).
2. Done 2026-11-02: see the measured section below.
3. Done: the TTBR0 write at the task switch adds no measurable time (see the measured section).
4. Count TLB refills in the OS image: EL0 cannot read the PMU, so the figure needs a counter grant or an EL1 sampler.
5. Run the partitioned scheduler's `freertos.c` with the hook on a board; it builds and boots in QEMU.
6. Backlog: regions requested at run time. The SDK has no such call, because the ledger and the tables are sized from the manifest at build time. A run-time request would need a ledger whose storage grows while the system runs, an upper-memory allocator that serves the request (the ledger is the allocator only for the regions the manifest declares), page-table pages taken at run time (`APP_SPACE_PAGES` per space is the bound), and a break-before-make update of the application's root with `TLBI ASIDE1IS` on every core.
7. Backlog: tearing down a space and reusing its ASID. The pool never takes an ASID back; reuse needs the table clear and the `TLBI ASIDE1IS` described in the design.
8. Backlog: set `TCR_EL1.AS` (bit 36 of both `A733_CPU_COHERENCE_TCR_*` constants) together with the checks and `platform/a733/cpu_continue.c` that pin them, and run the CPU-return images on a board on both clusters. Do it when application teardown or more than 255 applications per session is needed. With AS clear, 255 ASIDs is the width the hardware is configured for, and the pool already widens to 65535 when AS is set and the hardware reports 16 bits.

## Evidence

Board results are records in `docs/evidence/memory-dma.jsonl` and `docs/evidence/storage.jsonl`; none belongs to an OS image yet.

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/large-regions.jsonl`.
<!-- evidence-table:end -->
