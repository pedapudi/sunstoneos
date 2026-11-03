# Large application regions

**Status: implemented.**

An application declares regions of upper memory, up to the 11 GiB the board maps, in its manifest. The OS takes them from a ledger before the scheduler starts, gives the application its own address space (ASID-tagged translation tables, switched at the task switch), and a loader task fills each region from a FAT32 file or a raw card extent and checks its SHA-256. Design: [large application regions](../designs/large-app-regions.md); creating the test file: [region test file](../designs/region-test-file-write.md).

**Software evidence:** host, emulator.

**Board-qualified scope:** A read-only probe image (boot clocks, CPU 494 MHz) took a 1 GiB region, ran EL0 routines in three address spaces and showed the isolation, the 2 MiB and 1 GiB block walk counts and one read rate that does not depend on the block size; it read the card at 21.9 MB/s per 128 KiB request and mounted its FAT32 volume. No OS image has run on a board with a large region, and no performance figure at the approved top operating points exists.

## Heap from the ledger

A manifest heap above 4 MiB is the application's last large region (`SS_LARGE_HEAP`): the C library's heap, zero filled by `_sbrk`, with no card file. Host evidence: `checks/app-region.c` (records, admission, refusals) and the host layout check. Emulator evidence: `checks/qemu-os-boot.py` cases `heap-from-ledger`, `heap-other-space-faults`, `heap-end-faults`, `heap-worker-shares` and `heap-too-big`. The board image is `sunstone-a733-os-heap-test` (`apps/heap-test`); it has not run on a board. Design: [application memory layout](../designs/application-memory-layout.md).

## Defect found on a board and fixed

The first run of `sunstone-a733-os-large-region-test` in the CPU-return profile (found by the storage agent on a board) took a data abort in `os_large_prepare`: the admission read the boot level-1 table through `TTBR0_EL1`, and the A733 EL1 table leaves the 2 MiB block that holds the tables unmapped. Admission now reads a copy the platform makes (`os_kernel_l1_head`). The QEMU OS platform maps its tables the same way, and the cases that admit a region fault with ESR 0x96000006 if the read returns. No board run of the fix yet.

## Measured on a board, OS image at the CPU-return top points

Entry `1793632829779717086-f7d0d1cf` (3-1, A76, `PERF_BOOT=high`, development DRAM; a repeat of a first run whose expectation was frozen as raw bytes, so that run reads result-mismatch). The image includes the table-copy fix, the controller interrupt enable from the storage branch and its 50 s manifest.

- **Load, first image:** 30.659 s, 17.5 MB/s, digest matched. The design predicted 24 to 27 s. Both first runs reported 30,659,000 us because the loader timed from the 1 ms FreeRTOS tick.
- **Load, after the loader changes** (entry `1793633314510847134-f446c660`): 24.386 s, 22.0 MB/s, digest matched, as the storage agent's mode 3 read the same file (21.7 MB/s). Of it, 23.253 s is reading, 0.154 s copying and 0.979 s hashing with the SHA-256 instructions (548 MB/s), timed with the 24 MHz counter.
- **Task switch:** 20,000 round trips of two notifications between two tasks, 4.40 us per round trip in one space and 4.36 us with a TTBR0 write at each switch. The write adds nothing measurable; the difference is under one counter tick (42 ns) per switch and has the wrong sign.
- **Read bandwidth, one A76 core:** 12.79 GB/s over a 40 MiB window (eight timed passes), 12.66 GB/s over the whole 1 GiB (three passes), 1.0 percent lower. The `apps/dram-scaling` record `read_one_a76` is 12.69 GB/s.
- **Not measured:** TLB refill counts (EL0 cannot read the PMU in the OS image).

## Load time: why 30.7 s against 24.7 s, and the changes

Read from the code (`apps/os/large_load.c`, `apps/os/large_sd.c`, `drivers/storage/fat32_ro.c`, `drivers/storage/sunxi_mmc.c`), before any new run:

- **The clock.** `elapsed_us` was a difference of FreeRTOS tick counts times 1000, so it had a 1 ms step; two loads agreeing to the microsecond meant the same number of ticks, not the same microsecond. It now reads the 24 MHz counter.
- **The pipeline.** `fat32_stream` calls `submit` for the next 128 KiB and then hashes the current piece, so it looks overlapped. It was not: `a733_mmc_platform_read` only accepts the request, and the controller steps that write CMD18 run in the polls that follow. The loader hashes before it polls, so each piece cost the read plus the copy and the digest in sequence. `sd_submit` now steps the controller until the command is issued (`command_step` 2), so the transfer runs while the loader works.
- **The digest.** `storage_digest_add` was plain C, on the loader's pinned core (core 0, an A55). The 6.0 s difference over 4,096 pieces is 1.46 ms per piece, which is 90 MB/s for the copy and the C SHA-256 together, an estimate of about 20 cycles per byte at that core's clock, which is what plain C SHA-256 costs on an in-order A55. The loader runs at EL1, so it reads its own `ID_AA64ISAR0_EL1` and does not depend on the missing CPU observations (`docs/status/cpu-topology.md`, backlog item 7). It now uses the SHA-256 instructions (`drivers/storage/storage_digest_ce.S`) when SHA2 is present, and the C path otherwise. QEMU's A76 has the instructions: the loaded and digest-mismatch cases run the new path, and a wrong round constant fails them.
- **The copy.** There is no bounce buffer: the DMA fills one of the two 128 KiB buffers and the loader copies it once into the region (8 bytes at a time through the cached kernel mapping), then hashes the same buffer. The copy should be a small part of a piece (estimate, not measured: 128 KiB through a cached mapping).
- **Per-phase times.** The loader now records the time spent in the copy and in the digest (`ss_large_status` `copy_us`, `digest_us`; the test application returns them). The reading time is the load time minus those two. The first image had no phase records; the 1.46 ms per piece was a difference with the storage agent's mode 3. The run after the changes measured the phases (below).

## A 6 GiB region on a board

Entry `1793637177478128324-7b885910` (3-1, A76, `PERF_BOOT=high`, development DRAM). The application `apps/large-6g` declares one 6 GiB read-write region with no source (`SS_LARGE_UNFILLED`: ready at once, contents as the memory held them). The OS placed it at `0x80000000`; it ends at `0x200000000`, above 4 GiB, and is six 1 GiB blocks. One A76 core, 64-byte vector accesses, 24 MHz counter; nothing is checked or hashed inside a timed pass.

| Pass | 40 MiB window (eight passes) | Whole 6 GiB (two passes) |
|---|---|---|
| Read | 12.78 GB/s | 12.66 GB/s |
| Write | 12.51 GB/s | 12.16 GB/s |
| Copy (3 GiB onto 3 GiB, source bytes) | 5.84 GB/s | 5.84 GB/s |

The `apps/dram-scaling` figure `read_one_a76` is 12.69 GB/s. A copy moves twice its source bytes through the memory. Integrity, untimed: the 64-bit hash of each word's address written over all 6 GiB and read back, 805,306,368 words, 0 mismatches (2.41 s to write, 2.68 s to read back).

Isolation, one fault per image because an EL0 fault ends the application with a failure record (the application first reads the region's first and last words, so the fault is the probe's own):

- The byte below the region (`0x7fffffff`), owner's space: level 2 translation fault (entry `1793638861895715015-a7176fec`).
- The byte above it (`0x200000000`), owner's space: level 1 permission fault, the next gigabyte being the kernel's EL1-only block (`1793638933466934204-4dc4152d`).
- The first byte of the region from a task in no application space, which sees what a second application sees: level 1 permission fault (`1793638990638144080-9a9aad2d`).

The first three attempts at the bandwidth run took a level 1 translation fault at the first access: the application's tables were written with cached stores and the eight-core images walk tables without the data caches. The clean of `feb6ac47` fixed it.

## What bounds the load

The 512 MiB file loads in 24.386 s, 22.0 MB/s. The loader's phases are timed with the 24 MHz counter: copy 0.154 s (3.5 GB/s) and SHA-256 0.979 s (548 MB/s with the extension instructions). They overlap the card's transfer of the next piece, so what remains is the card: 23.25 s is the load minus those two, and the whole load is within 12 percent of the 25 MB/s raw ceiling of the 4-bit 50 MHz high-speed bus mode (`SUNXI_MMC_SD_HIGH_SPEED4`, `apps/os/large_sd.c`). Copying by DMA straight into the region would save at most the 0.15 s; hashing in parallel would save at most about 1 s. Neither is worth doing before the bus mode changes. The gain is a faster mode (SDR104), which is a storage task. A region declared without a `sha256` is not hashed.

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

Broadcast TLB invalidation (memory-limits work, step 7). Twenty soaks of 25 s on port 3-1, each 250 000 control periods at 10 kHz under the
mimus recognition, measured the cost of a broadcast invalidation issued from a Cortex-A55 core 1 000 times per second. `TLBI VAE1IS` and `TLBI
ASIDE1IS` raised the control path's maximum from 3.33 us (no task) to 3.54 us at most; `TLBI VMALLE1IS` gave one soak of six at 5.62 us, with a jitter of
5.38 us against the bound of 5 us, which the data neither tie to the invalidation nor exclude. The page and ASID forms are allowed as the fallback of the
release protocol up to that rate; the all-entries form is not. Unmeasured: 100 per second, entries in use by the receivers, receivers on the Cortex-A76
cores. Evidence: `docs/designs/application-memory-layout.md`, "Predicted board soaks of the broadcast invalidation on the control path" and its result;
the diagnostic with the wake metrics (`apps/latency`, `SUNSTONE_LATENCY_TLBI`) could not resolve the stall.

