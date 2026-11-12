# Large application regions

**Status: implemented.**

An application declares regions of upper memory, up to the 11 GiB the board maps, in its manifest. The OS takes them from a ledger before the scheduler starts, gives the application its own address space (ASID-tagged translation tables, switched at the task switch), and a loader task fills each region from a FAT32 file or a raw card extent and checks its SHA-256. A manifest heap above 4 MiB is the application's last large region (`SS_LARGE_HEAP`), zero filled by `_sbrk`. The same ledger now serves the NPU, the kernel's metadata and the device range list, as one allocator over all DRAM above the OS image: [unified memory](../designs/unified-memory.md). Designs: [large application regions](../designs/large-app-regions.md), [application memory layout](../designs/application-memory-layout.md); creating the test file: [region test file](../designs/region-test-file-write.md).

**Software evidence:** host, emulator.

**Board-qualified scope:** A read-only probe image (boot clocks, CPU 494 MHz) took a 1 GiB region, ran EL0 routines in three address spaces and showed the isolation and the 2 MiB and 1 GiB block walk counts. An OS image at the CPU-return top points loaded a 512 MiB file from the card into a region with a matching digest, switched address spaces at the task switch with no measurable cost, and read the region at the single-core rate. A 6 GiB region without a source was written, read, copied and verified word by word, and its three boundary faults were observed. The 80-graph encoder image then ran with its NPU reservation taken from the ledger at admission, and with placement requests at 6, 10 and 12 GiB: 768 exact jobs and a normal return at each, with the published ranges read back, a guard at the low 32-bit alias unchanged at 6 and 10 GiB, and the return cleaning the caches by set/way. Not covered: any run of the partitioned scheduler's hook, the TLB refill counts in the OS image, and the SD0 binding failures of the OS image (QEMU only).

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Region ledger, per-application tables, non-global entries | qualified on the board | isolation faults as designed in a probe and on a 6 GiB region | `1793603354809199340-a4ed0f70`, `1793638861895715015-a7176fec`, `1793638933466934204-4dc4152d`, `1793638990638144080-9a9aad2d` |
| Address-test decode result over the 12,224 MiB map | qualified on the board | no alias, no beacon | `1793600664425885881-a871e54d` |
| Loader from a FAT32 file, SHA-256 check | qualified on the board | 512 MiB in 24.386 s, 22.0 MB/s, within 12 percent of the card's bus ceiling | `1793633314510847134-f446c660` |
| TTBR0 write at the task switch | qualified on the board | 4.36 us against 4.40 us per round trip | `1793632829779717086-f7d0d1cf` |
| Read, write and copy of a 6 GiB region | qualified on the board | read 12.66, write 12.16, copy 5.84 GB/s | `1793637177478128324-7b885910` |
| NPU reservation from the ledger, with metadata from kernel chunks | qualified on the board | 80-graph encoder, 768 exact jobs, queue open 285 ms, 32 MiB kept after the trim | `1793752782443510918-71e48d74` |
| NPU placement request (physical base chosen by the session) | qualified on the board | 6, 10 and 12 GiB each published as asked; the guard at the alias of 6 and 10 GiB unchanged; 12 GiB has no alias in DRAM | `1793752782443510918-71e48d74`, `1793752905870906415-8ee3b298`, `1793758036160485351-c9fae9df` |
| Device range list read back after the return | qualified on the board | pages below 4 GiB, because FEL addresses are 32 bits; the runner refuses a head at or above 4 GiB | `1793753010853111400-91014eb3`, `1793758036160485351-c9fae9df` |
| Cache clean at the CPU return | qualified on the board | by set/way: 0.13 ms on core 0, 0.02 ms on each A55 secondary, 2.49 ms on each A76, against 227 ms per 32 MiB range by line | `1793758036160485351-c9fae9df` |
| Heap from the ledger | qualified on the board | see [application memory layout](../designs/application-memory-layout.md) | `application-memory-layout-heap-modes` |
| Partitioned scheduler's task-switch hook | host-tested | builds and boots in QEMU | `checks/qemu-os-boot.py` |
| Device grant | open | interface only | [design](../designs/large-app-regions.md) |
| Broadcast TLB invalidation cost (1,000 per second from an A55 core) | qualified on the board | page and ASID forms: control-path maximum 3.54 us against 3.33 us without; all-entries form not cleared (5.38 us jitter in one soak of six) | `application-memory-layout-tlbi-soaks` |
| Regions requested at run time | open | needs a growing ledger and table pages at run time; the release protocol is in the design | [application memory layout](../designs/application-memory-layout.md) |

## Open work

1. Run the SD0 binding in an OS image through the failure scenarios on a board. `apps/os/large_sd.c` (`os_large_device`, `os_large_device_close`, `os_large_sd_pins`) uses the OS's shared providers and the read-only SD core; QEMU has no SD controller, so its RAM disk covers the loader and four scenarios cover a device that is present and fails (metadata, request start, mid-stream, absent).
2. Count TLB refills in the OS image: EL0 cannot read the PMU, so the figure needs a counter grant or an EL1 sampler.
3. Run the partitioned scheduler's `kernel/scheduler/freertos.c` with the hook on a board.
4. Regions requested at run time: a ledger whose storage grows while the system runs, an upper-memory allocator that serves the request, page-table pages taken at run time, and a break-before-make update of the application's root with `TLBI ASIDE1IS` on every core.
5. Tearing down a space and reusing its ASID: the pool never takes an ASID back; reuse needs the table clear and the `TLBI ASIDE1IS` described in the design.
6. Set `TCR_EL1.AS` (bit 36 of both `A733_CPU_COHERENCE_TCR_*` constants) together with the checks and `platform/a733/cpu_continue.c` that pin them, and run the CPU-return images on both clusters, when application teardown or more than 255 applications per session is needed. The pool already widens to 65535 when AS is set and the hardware reports 16 bits.
7. A faster card bus mode (SDR104) is the storage task that would shorten the load.

## Unified memory: what the board runs showed

Placements at 6 and 10 GiB passed with identical figures. The third run (12 GiB) was exact and returned normally, but its list page had been placed above 4 GiB and the runner read the 32-bit truncation of its address; the OS now takes list pages below 4 GiB. The return's cache clean had cost 434 ns per `DC CIVAC` on every core, 227 ms per 32 MiB range on cores 0 to 5 and 185 ms on cores 6 and 7, and each range was cleaned eight times. Each core now cleans its own L1 and L2 by set/way as it goes down, reading its cache geometry at run time (Cortex-A55 and Cortex-A76 differ), and core 0 cleans the shared L3 last. The rerun at 12 GiB read the list back with the 12 GiB range, and every core's walk-counter words, which are dirty lines no clean by address reaches, arrived in DRAM. The sweeps took between 0.02 and 2.49 ms. The Arm technical reference manuals for both cores also flush L1 and L2 in hardware once `CPUPWRCTLR.CORE_PWRDN_EN` is set and `WFI` runs; the software sweep on the secondaries is kept so that the result does not depend on that.

The terminal's block-by-block range check (1.3 to 1.5 ms across the eight cores) remains. The rerun's total job time was 694 ms against 657 ms in the 6 and 10 GiB runs; the figure is taken before the return begins, and the difference has not been explained.

## Evidence

Board results are records in [`docs/evidence/memory-dma.jsonl`](../evidence/memory-dma.jsonl) and [`docs/evidence/storage.jsonl`](../evidence/storage.jsonl); the host and defect records are in [`docs/evidence/large-regions.jsonl`](../evidence/large-regions.jsonl).

<!-- evidence-table:begin -->
6 records in `docs/evidence/large-regions.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `large-regions-host-qemu` |  |  | `` | not-run | passed | task_switch_qemu_same_space 289.16 ticks, task_switch_qemu_ttbr0_write 289.34 ticks, hook_instructions_no_space 6 instructions, hook_instructions_space 20 instructions |
| `large-regions-table-read-defect` |  |  | `` | returned | failed |  |
| `1793752782443510918-71e48d74` | 2026-11-04 | 3-1 | `2b7e0a643f3e` | returned | passed | run_total 96.072 s, jobs 768, open_time 285.2 ms, published_pool_bytes 3.35544e+07 B, guard_differing_words 0, return_walk_core0 466 ms, return_walk_core6 372 ms |
| `1793752905870906415-8ee3b298` | 2026-11-04 | 3-1 | `2b7e0a643f3e` | returned | passed | run_total 96.108 s, jobs 768, open_time 284.2 ms, published_pool_bytes 3.35544e+07 B, guard_differing_words 0, return_walk_core0 462 ms, return_walk_core6 371 ms |
| `1793753010853111400-91014eb3` | 2026-11-04 | 3-1 | `2b7e0a643f3e` | returned | inconclusive | run_total 95.812 s, jobs 768, return_walk_core0 227 ms, return_walk_core6 185 ms |
| `1793758036160485351-c9fae9df` | 2026-11-04 | 3-1 | `5d157656719f` | returned | passed | run_total 103.864 s, jobs 768, open_time 285.3 ms, published_pool_bytes 3.35544e+07 B, sweep_core0 0.127 ms, sweep_a55_secondary 0.018 ms, sweep_a76 2.494 ms, range_walk_checks 1.5 ms |
<!-- evidence-table:end -->
