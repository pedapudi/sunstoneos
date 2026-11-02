# Large application regions

**Status: implemented.**

An application declares regions of upper memory, up to the 11 GiB the board maps, in its manifest. The OS takes them from a ledger before the scheduler starts, gives the application its own address space (ASID-tagged translation tables, switched at the task switch), and a loader task fills each region from a FAT32 file or a raw card extent and checks its SHA-256. Design: [large application regions](../designs/large-app-regions.md); creating the test file: [region test file](../designs/region-test-file-write.md).

**Software evidence:** host, emulator.

**Board-qualified scope:** A read-only probe image (boot clocks, CPU 494 MHz) took a 1 GiB region, ran EL0 routines in three address spaces and showed the isolation, the 2 MiB and 1 GiB block walk counts and one read rate that does not depend on the block size; it read the card at 21.9 MB/s per 128 KiB request and mounted its FAT32 volume. No OS image has run on a board with a large region, and no performance figure at the approved top operating points exists.

## Open work

1. Run the SD0 binding in an OS image on a board. `apps/os/large_sd.c` (`os_large_device`, `os_large_device_close`, `os_large_sd_pins`) uses the OS's shared providers and the read-only SD core, builds into `sunstone-a733-os-large-region-test` and passes the read-only scan, but only the standalone probe has driven the card on a board. QEMU has no SD controller: its RAM disk covers the loader, and four scenarios cover a device that is present and fails (metadata, request start, mid-stream, absent).
2. Run `sunstone-a733-os-large-region-test` on a board once `/sunstone-test/sunstone-region-test.bin` exists on the card (created by the storage-write path): load time, digest, read bandwidth at the top points against `apps/dram-scaling`, and the task-switch cost.
3. Measure the added cycles of a TTBR0 write at the task switch on the board; QEMU shows 4 instructions of time per switch and no cycles.
4. Count TLB refills in the OS image: EL0 cannot read the PMU, so the figure needs a counter grant or an EL1 sampler.
5. Run the partitioned scheduler's `freertos.c` with the hook on a board; it builds and boots in QEMU.
6. Backlog: regions requested at run time. The SDK has no such call, because the ledger and the tables are sized from the manifest at build time. A run-time request would need a ledger whose storage grows while the system runs, an upper-memory allocator that serves the request (the ledger is the allocator only for the regions the manifest declares), page-table pages taken at run time (`APP_SPACE_PAGES` per space is the bound), and a break-before-make update of the application's root with `TLBI ASIDE1IS` on every core.
7. Backlog: tearing down a space and reusing its ASID. The pool never takes an ASID back; reuse needs the table clear and the `TLBI ASIDE1IS` described in the design.

## Evidence

Board results are records in `docs/evidence/memory-dma.jsonl` and `docs/evidence/storage.jsonl`; none belongs to an OS image yet.

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/large-regions.jsonl`.
<!-- evidence-table:end -->
