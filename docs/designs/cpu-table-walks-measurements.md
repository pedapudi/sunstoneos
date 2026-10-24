# Measurements: cacheable EL1 table walks and one table builder

Status: **measured in host models**, and the core-0 forced-walk loop
**measured on the board** (section "Board measurement of the core-0 walk
loop"). The host-model figures compare the budgets in
[cpu-table-walks.md](cpu-table-walks.md) with two builds. The base build is
commit `14c3cd8`, the integration commit that this change starts from. The
head build is commit `130fe53`, the last commit of this change. Only the
board section describes physical walk behaviour.

## Conditions

- Sources: each commit exported with `git archive` into its own directory.
  The head copy also carries the new `continue.S` SHA-256 in the three
  hash-pin lines of `cmake/a733-storage-maintenance.cmake`,
  `cmake/a733-smp-observe.cmake` and `cmake/a733-smp-supervised.cmake`. The
  design lists that edit for the primary agent, and the head tree does not
  configure without it.
- Images: the cached eight-core diagnostic image
  (`a733-smp-cached-diagnostic`) and the cached crypto single-core image
  (`apps/crypto-single-core`), built with Clang 21.1.8 and `ld.lld`.
- Tools: `checks/cpu-table-walks.py` from the head commit, run with
  `--baseline` on the base image and with `--single-core-image` on the head
  images. It runs the linked code in Unicorn and models system registers,
  cache maintenance and the counter. Short scripts using the same model
  counted instructions and barriers in `a733_cached_memory_prepare()`, ran
  the single-core table check, compared the prepared table pages word by
  word, and ran the writer scan of `checks/device-memory-capacity.py` and the
  literal scan on each tree. Section sizes come from `llvm-size` and
  function sizes from `llvm-nm`.

## Budget metrics

| Metric | Budget | Base | Head | Within budget |
| --- | --- | --- | --- | --- |
| Table writes after the builder returns (writer-scan hits) | 0 | 17 | 0 | yes |
| Files outside the header and frozen images that spell the EL1 TCR value | 0 | 11 | 0 | yes |
| Flush before the first table write | flush call precedes the builder | no flush | precedes the builder and every table-section write | yes |
| `enable_el1` instructions, success path | at most 2,500 | 75 | 2,233 | yes |
| Table-line maintenance per boot, eight-core image | 1,344; 1,664 until the publication is dropped | 320 `DC CVAC` | 1,345; 1,666 | no, by one line per flush |
| Table-line maintenance per boot, single-core image | 448 | 0 | 449 | no, by one line |
| Table readback per core and boot | 512 `LDP` | 0 | 512 `LDP` | yes |
| Table memory | 20,544 B | 20,480 B | 20,544 B | yes |
| EL1 mapping | unchanged until stage 2 | 126 MiB of Normal DRAM in this image | 126 MiB | yes |
| `TCR_EL1` installed by the eight-core entry | `0x200803019` | `0x200803019` | `0x200803019` | yes |

Notes on the rows:

- The 17 writer-scan hits at base are 16 statements and one patch context
  line in `apps/dsi-scanout/monitor.patch` and
  `apps/storage-read-single-core/monitor.c`.
- The base literal scan finds 22 files. Eleven remain at head: the header,
  which now holds both values, and ten files for the noncacheable
  continuation profile and the frozen production image. The eleven removed
  files are the two assembly sources, eight single-core and coherence host
  checks, and `checks/storage-read-single-core.py`. The design counts ten,
  omitting that last check.
- The maintenance counts exceed the budget because the EL3 flush covers the
  whole table section, including the 64-byte checksum line: 321 lines
  instead of 320. The eight-core count is 321 at EL3 plus 128 per core on
  eight cores, and 321 more while the `continue.c` publication remains. The
  single-core count is 321 plus 128. Section "Implementation results" of the
  design states this reason.
- The prepared table pages differ from base in two words only: level-1
  entry 0 of the EL3 and the EL1 regime gains `SH = 0b10`. The capacity
  check's 910 MiB figure applies to the extended device-memory profile,
  which this diagnostic image does not enable. The stage-2 blocks are
  compiled out in both builds.

## Other costs

| Metric | Base | Head | Cause stated in the design |
| --- | --- | --- | --- |
| `a733_cached_memory_prepare()` instructions, eight-core image | 11,669 | 15,596 | the 321-line flush and the checksum over two pages |
| `a733_cached_memory_prepare()` instructions, single-core image | 11,702 | 15,629 | same |
| Barriers in `a733_cached_memory_prepare()` | 0 | 1 `DSB SY` | the flush ends with `DSB SY` |
| `DSB` and `ISB` in `enable_el1`, success path | 11 | 15 | the flush and the Device readback before the enable |
| Single-core table check (`check_el1`) | absent | 2,189 instructions, 512 `LDP`, 128 `DC CIVAC`, no system-register write | step 3 of the design |
| Table-section writes during preparation | 2,694 | 2,695 | the checksum word |
| `.text`, eight-core image | 51,200 B | 53,248 B | new flush and check primitives |
| Changed functions, eight-core image | — | +1,216 B: `flush` 208 B and `check_el1` 500 B new, `enable_el1` +376 B, `a733_cached_memory_prepare()` +132 B | same |
| `.text`, single-core image | 492,224 B | 492,224 B | `platform_init()` grows by 56 B inside existing alignment padding |

Each added instruction and barrier runs once per boot or once per core
entry. No metric changes on a path that runs after the scheduler starts. The
board runs in section "Board evidence required" of the design still decide
whether cacheable walks reduce walk cost.

## Board measurement of the core-0 walk loop

Conditions: port 3-3, 2026-10-24, bundle 4 of
[board-bundles-next.md](board-bundles-next.md); raw log
`~/.cache/sunstoneos/evidence/board-session-20261023/port-3-3/bb4-run/kernel.log`.
Core 0, Cortex-A55 (`MIDR_EL1` `0x412fd050`), at 494,001,083 Hz: 494,104
cycles over 24,005 ticks of the 24 MHz counter. `PMCR_EL0` reads
`0x41453041` (six event counters), and all six events of the record are
implemented. A is `TCR_EL1` `0x200803019` (uncached walks), B `0x200803519`
(cached walks). Each block is 1,024 iterations of `TLBI VMALLE1`, `DSB ISH`,
`ISB` and one load, with IRQs masked.

The counters were checked first in the same run:

- an empty region retires 20 instructions in each of 64 regions, and takes
  64 cycles at the minimum and median and 131 at most;
- a region of 2,000 ALU instructions counts 2,020 instructions, exactly
  2,000 above the empty median, in 7,238 cycles;
- `L2D_CACHE_REFILL` is implemented (`PMCEID0_EL0` `0x7fffffff`) and counts
  1,031 refills for one load per line of a cleaned and invalidated 64 KiB
  buffer of 1,024 lines.

Counts per block; the six blocks of each value are listed in run order.
`INST_RETIRED` is 15,388 in every block: 15 loop instructions per iteration
plus 28 for the calls around the loop.

| Block | Value | Cycles | Cycles per iteration | `L1D_CACHE_REFILL` | `L2D_CACHE_REFILL` | `BUS_ACCESS` | `DTLB_WALK` | `ITLB_WALK` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0, warm-up, discarded | A | 715,999 | 699.2 | 63 | 63 | 7,468 | 1,024 | 1,024 |
| 1 | A | 709,968 | 693.3 | 6 | 0 | 7,168 | 1,024 | 1,024 |
| 2 | B | 447,234 | 436.8 | 4 | 6 | 4,124 | 1,024 | 1,024 |
| 3 | B | 446,624 | 436.2 | 0 | 0 | 4,096 | 1,024 | 1,024 |
| 4 | A | 709,614 | 693.0 | 1 | 0 | 7,168 | 1,024 | 1,024 |
| 5 | A | 709,747 | 693.1 | 0 | 0 | 7,168 | 1,024 | 1,024 |
| 6 | B | 446,597 | 436.1 | 0 | 0 | 4,096 | 1,024 | 1,024 |
| 7 | B | 446,621 | 436.2 | 0 | 0 | 4,096 | 1,024 | 1,024 |
| 8 | A | 709,023 | 692.4 | 0 | 0 | 7,168 | 1,024 | 1,024 |
| 9 | A | 710,207 | 693.6 | 0 | 0 | 7,168 | 1,024 | 1,024 |
| 10 | B | 446,609 | 436.1 | 0 | 0 | 4,096 | 1,024 | 1,024 |
| 11 | B | 446,612 | 436.1 | 0 | 0 | 4,096 | 1,024 | 1,024 |
| 12 | A | 709,792 | 693.2 | 0 | 0 | 7,168 | 1,024 | 1,024 |

Summary of blocks 1 to 12:

| Metric | A, uncached walks | B, cached walks |
| --- | --- | --- |
| Cycles per iteration, median | 693.1 | 436.1 |
| Spread, (max − min) / median | 0.17% | 0.14% |
| Time per iteration at 494.0 MHz | 1.40 µs | 0.88 µs |
| Bus accesses per iteration | 7 | 4 |
| Data and instruction walks per iteration | 1 and 1 | 1 and 1 |

A calibration region before each block (`TLBI VMALLE1`, `DSB ISH`, `ISB`,
then the end of the region) counted no data walk and one instruction walk in
every block. The prepared image required each block to count 1,024 plus
those counts, so it reported all 13 blocks as refused; the corrected check in
`apps/board-bundles-cpu/walk.h` requires 1,024 of each and accepts all 13.
The design applies its revert rule to this result in section "Board result
of the core-0 walk loop".
