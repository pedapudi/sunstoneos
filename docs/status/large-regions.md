# Large application regions

**Status: implemented.**

An application declares regions of upper memory, up to the 11 GiB the board maps, in its manifest. The OS takes them from a ledger before the scheduler starts, gives the application its own address space (ASID-tagged translation tables, switched at the task switch), and a loader task fills each region from a FAT32 file or a raw card extent and checks its SHA-256. A manifest heap above 4 MiB is the application's last large region (`SS_LARGE_HEAP`), zero filled by `_sbrk`. Designs: [large application regions](../designs/large-app-regions.md), [application memory layout](../designs/application-memory-layout.md); creating the test file: [region test file](../designs/region-test-file-write.md).

**Software evidence:** host, emulator.

**Board-qualified scope:** A read-only probe image (boot clocks, CPU 494 MHz) took a 1 GiB region, ran EL0 routines in three address spaces and showed the isolation and the 2 MiB and 1 GiB block walk counts. An OS image at the CPU-return top points loaded a 512 MiB file from the card into a region with a matching digest, switched address spaces at the task switch with no measurable cost, and read the region at the single-core rate. A 6 GiB region without a source was written, read, copied and verified word by word, and its three boundary faults were observed. Not covered: any run of the partitioned scheduler's hook, the TLB refill counts in the OS image, and the SD0 binding failures of the OS image (QEMU only).

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Region ledger, per-application tables, non-global entries | qualified on the board | isolation faults as designed in a probe and on a 6 GiB region | `1793603354809199340-a4ed0f70`, `1793638861895715015-a7176fec`, `1793638933466934204-4dc4152d`, `1793638990638144080-9a9aad2d` |
| Address-test decode result over the 12,224 MiB map | qualified on the board | no alias, no beacon | `1793600664425885881-a871e54d` |
| Loader from a FAT32 file, SHA-256 check | qualified on the board | 512 MiB in 24.386 s, 22.0 MB/s, within 12 percent of the card's bus ceiling | `1793633314510847134-f446c660` |
| TTBR0 write at the task switch | qualified on the board | 4.36 us against 4.40 us per round trip | `1793632829779717086-f7d0d1cf` |
| Read, write and copy of a 6 GiB region | qualified on the board | read 12.66, write 12.16, copy 5.84 GB/s | `1793637177478128324-7b885910` |
| Heap from the ledger | qualified on the board | see [application memory layout](../designs/application-memory-layout.md) | `application-memory-layout-heap-modes` |
| Partitioned scheduler's task-switch hook | host-tested | builds and boots in QEMU | `checks/qemu-os-boot.py` |
| Device grant | open | interface only | [design](../designs/large-app-regions.md) |
| Regions requested at run time | open | needs a growing ledger, table pages at run time, a broadcast invalidation | [application memory layout](../designs/application-memory-layout.md) |

## Open work

1. Run the SD0 binding in an OS image through the failure scenarios on a board. `apps/os/large_sd.c` (`os_large_device`, `os_large_device_close`, `os_large_sd_pins`) uses the OS's shared providers and the read-only SD core; QEMU has no SD controller, so its RAM disk covers the loader and four scenarios cover a device that is present and fails (metadata, request start, mid-stream, absent).
2. Count TLB refills in the OS image: EL0 cannot read the PMU, so the figure needs a counter grant or an EL1 sampler.
3. Run the partitioned scheduler's `freertos.c` with the hook on a board.
4. Regions requested at run time: a ledger whose storage grows while the system runs, an upper-memory allocator that serves the request, page-table pages taken at run time, and a break-before-make update of the application's root with `TLBI ASIDE1IS` on every core.
5. Tearing down a space and reusing its ASID: the pool never takes an ASID back; reuse needs the table clear and the `TLBI ASIDE1IS` described in the design.
6. Set `TCR_EL1.AS` (bit 36 of both `A733_CPU_COHERENCE_TCR_*` constants) together with the checks and `platform/a733/cpu_continue.c` that pin them, and run the CPU-return images on both clusters, when application teardown or more than 255 applications per session is needed. The pool already widens to 65535 when AS is set and the hardware reports 16 bits.
7. A faster card bus mode (SDR104) is the storage task that would shorten the load.

## Evidence

Board results are records in [`docs/evidence/memory-dma.jsonl`](../evidence/memory-dma.jsonl) and [`docs/evidence/storage.jsonl`](../evidence/storage.jsonl); the host and defect records are in [`docs/evidence/large-regions.jsonl`](../evidence/large-regions.jsonl).

<!-- evidence-table:begin -->
2 records in `docs/evidence/large-regions.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `large-regions-host-qemu` |  |  | `` | not-run | passed | task_switch_qemu_same_space 289.16 ticks, task_switch_qemu_ttbr0_write 289.34 ticks, hook_instructions_no_space 6 instructions, hook_instructions_space 20 instructions |
| `large-regions-table-read-defect` |  |  | `` | returned | failed |  |
<!-- evidence-table:end -->
