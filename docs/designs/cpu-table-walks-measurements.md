# Measurements: cacheable EL1 table walks and one table builder

Status: **measured in host models**; not run on the board. These figures
compare the budgets in [cpu-table-walks.md](cpu-table-walks.md) with two
builds. The base build is commit `14c3cd8`, the integration commit that this
change starts from. The head build is commit `130fe53`, the last commit of
this change. No figure here describes physical cache, walk or coherence
behaviour.

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
