# Large application regions

**Status: implemented.**

An application declares regions of upper memory, up to the 11 GiB the board maps, in its manifest. The OS takes them from a ledger before the scheduler starts, gives the application its own address space (ASID-tagged translation tables, switched at the task switch), and a loader task fills each region from a FAT32 file or a raw card extent and checks its SHA-256. Design: [large application regions](../designs/large-app-regions.md); creating the test file: [region test file](../designs/region-test-file-write.md).

**Software evidence:** host, emulator.

**Board-qualified scope:** A read-only probe image (boot clocks, CPU 494 MHz) took a 1 GiB region, ran EL0 routines in three address spaces and showed the isolation, the 2 MiB and 1 GiB block walk counts and one read rate that does not depend on the block size; it read the card at 21.9 MB/s per 128 KiB request and mounted its FAT32 volume. No OS image has run on a board with a large region, and no performance figure at the approved top operating points exists.

## Open work

1. Bind the OS loader to the SD0 provider of the OS image: `os_large_device()` returns none on the A733 images, so every region there fails as unloadable until the board supplies a device. The QEMU board supplies a RAM disk.
2. Run `sunstone-a733-os-large-region-test` on a board once `/sunstone-test/sunstone-region-test.bin` exists on the card (created by the storage-write path): load time, digest, read bandwidth at the top points against `apps/dram-scaling`, and the task-switch cost.
3. Measure the added cycles of a TTBR0 write at the task switch on the board; QEMU shows 4 instructions of time per switch and no cycles.
4. Count TLB refills in the OS image: EL0 cannot read the PMU, so the figure needs a counter grant or an EL1 sampler.
5. Run the partitioned scheduler's `freertos.c` with the hook on a board; it builds and boots in QEMU.
6. Reload or resize a region after start (`ss_large_request` refuses today: the ledger and tables are sized from the manifest).

## Evidence

Board results are records in `docs/evidence/memory-dma.jsonl` and `docs/evidence/storage.jsonl`; none belongs to an OS image yet.

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/large-regions.jsonl`.
<!-- evidence-table:end -->
