# Design: cacheable EL1 translation-table walks and one table builder

Status: **implemented** in the package files and checked in instruction
models; not run on the board. Section "Implementation results" records the
measured counts and where the implementation differs from the design. No
figure below has been measured on the board. Readers are assumed to know AArch64 stage-1 translation, the
Secure and Non-secure physical address spaces, and data-cache maintenance by
virtual address.

The cached images build five 4 KiB translation-table pages in the monitor
interval at `0x421f0000`: level 1, level 2 and level 3 for EL3, and level 1
and level 2 for EL1. Before this change `A733_CPU_COHERENCE_TCR`
(`0x200803019`) makes the EL1 hardware table walker read descriptors as Normal
non-cacheable memory (IRGN0 = ORGN0 = 0b00), so every descriptor read that
misses the walk caches goes to DRAM. This change sets IRGN0 = ORGN0 = 0b01
(write-back, read-allocate, write-allocate), giving `0x200803519`, which is
the value `platform_init()` in `platform/a733/platform.c` already uses for its
own tables. A cacheable walker can hit a stale cache line over a table page, so
the change adds two cache-maintenance steps and a per-boot check of the EL1
tables. It also finishes every EL1 descriptor inside the builder.

The new value applies first to the cached single-core images, which run only
on Cortex-A55 core 0, where the walk cost has a pinned source (section
"Sources and unproven sequences"). The cached eight-core images also schedule
tasks on the Cortex-A76, for which no walk-cost source is pinned, so they keep
`0x200803019` until the core-6 measurement in section "Board evidence
required" passes. The header holds both values,
`A733_CPU_COHERENCE_TCR_CACHED_WALK` and
`A733_CPU_COHERENCE_TCR_UNCACHED_WALK`, and defines `A733_CPU_COHERENCE_TCR`
as the cached value when the target defines `SUNSTONE_CACHED_SINGLE_CORE`, and
as the uncached value otherwise. Every user of `A733_CPU_COHERENCE_TCR`,
including the profile check in `platform/a733/device_memory_layout.c`, which
both image classes link, therefore compares with its own image's value. The
eight-core images still run the flush and the table check below. With
non-cacheable walks the flush removes a stale line that a later eviction could
write over the tables, and the check refuses tables that such a write-back
corrupted.

## Ownership and data flow

1. **Cold entry, core 0, EL3, MMU and caches off.** At the start of
   `a733_cached_memory_prepare()`, core 0 runs `DC CIVAC` (clean and
   invalidate by address to the point of coherency) over the five table pages
   before the first descriptor write. With the MMU off, EL3 addresses the
   Secure physical address space, so this removes any Secure dirty line that
   could later be evicted over a descriptor. Today the publication in
   `platform/a733-smp-cached/continue.c` cleans the tables with `DC CVAC`
   after they are written, which would write such a line back over the new
   descriptors.
2. **Build.** `a733_cpu_coherence_tables()` and the rest of
   `a733_cached_memory_prepare()` write every entry of the five pages.
   Level-1 entry 0, the MMIO block, gains the outer-shareable field
   (`SH = 0b10`) in both regimes. The two monitor variants that set that field
   afterwards (`apps/storage-read-single-core/monitor.c`, seven image branches,
   and `apps/dsi-scanout/monitor.patch`, one branch) lose those 16 writes.
   No code writes a table after `a733_cached_memory_prepare()` returns. As its
   last step, core 0 stores a 64-bit additive checksum of the two EL1 pages in
   `a733_cached_el1_table_sum`, a 64-byte-aligned word in the same
   `.bss.a733_cached_tables` section, so every image that names the EL1 root
   names the checksum by the same means.
3. **EL1 entry, every core, MMU and caches off.** Before installing TCR, TTBR
   and SCTLR, `a733_cpu_coherence_enable_el1()` runs `DC CIVAC` over the EL1
   root page and over the level-2 page named by the root's entry 1, then
   `DSB SY`. EL1 with the MMU off addresses the Non-secure space, which is the
   alias the EL1 walker reads, so stale Non-secure lines are removed before
   the first walk. The core then reads both pages back with Device loads
   (512 `LDP`), sums them and compares the sum with the recorded checksum. A
   mismatch returns failure before `SCTLR_EL1.M` is set, and the caller stops
   the core through its existing fault path. The cached single-core images
   enable translation in the patched `platform_init()`
   (`apps/storage-read-single-core/platform.patch`) instead, so that branch
   calls an exported copy of the same sequence, built from one assembler
   macro in `platform/a733/cpu_coherence.S`.
4. **Runtime.** The EL1 walker is the only reader of the EL1 tables. EL1 has
   no data mapping of the monitor interval (EL1 level-2 entry 16 is invalid),
   and EL3 maps the EL1 tables Normal non-cacheable through the Non-secure
   alias but does not access them after preparation.

The maintenance is one new stackless primitive in
`platform/a733/cpu_coherence.S`, `a733_cpu_coherence_flush(base, bytes,
deadline)`. It requires EL3 or EL1 with the MMU and data cache off, masked
exceptions, 64-byte data lines and a range inside `0x40000000`–`0x48000000`.
It runs `DC CIVAC` per line, checks the deadline before and after the loop and
ends with `DSB SY`. `DC CIVAC` never discards data, so a deadline refusal part
way through leaves memory unchanged. The existing `a733_cpu_coherence_publish()`
keeps its clean-only contract, and the signature of `enable_el1` stays as it
is.

**Interrupts and polling.** Nothing here takes or waits for an interrupt. The
only polling is the counter read of the deadline check, bounded by the
startup deadline that the callers already pass.

**Memory placement and cache policy.** The tables stay in the monitor
interval. The EL3 walk and every EL3 data access to the tables stay
non-cacheable, Inner Shareable; `A733_CPU_COHERENCE_TCR_EL3` does not change.
The EL1 walk becomes write-back cacheable, Inner Shareable, so the walker's
lines are coherent with every core's data cache and the DynamIQ Shared Unit
(DSU) L3 cache.

**Synchronization across cores.** No lock is needed, because the tables are
immutable once core 0 returns from preparation, and the existing record
barrier in `continue.c` holds every secondary until then. Each core removes
stale lines and checks the tables before its own first walk. For a stale line
in another core's cache to be removed, `DC CIVAC` issued with the MMU off must
reach every cache in the Outer Shareable domain. The Arm Architecture
Reference Manual states this for Device-type accesses, but no copy of that
manual is pinned. The step 3 check detects a write-back that reached memory
before the check; the dirty-line experiment below bounds later ones.

## Stage-2 level-1 blocks

EL1 level-1 entries 2 and 3 map `0x8000_0000`–`0xFFFF_FFFF` as 1 GiB
write-back blocks: `PA | UXN | PXN | AF | SH = 0b11 | AttrIndx = 1 | 1` with
AP[2:1] = 0b00, that is `0x0060000080000705` and `0x00600000c0000705`. The
EL3 entries stay invalid. `memory.c` writes them under
`#if A733_DEVICE_MEMORY_LAYOUT >= 2`, with no runtime profile test, because
the header constant is 1 and `a733_device_memory_layout_valid()` accepts no
other profile. The entries arrive with the layout owner's header change.

A mapped write-back block receives speculative reads, so inside that branch
`memory.c` first calls `a733_dram_decode_check()`, an EL3 function that the
DRAM-controller package must provide. It reads the controller configuration
on this boot and returns 1 only when DRAM decodes linearly to
`0x3_4000_0000` without aliasing and no protected region, including the
120 MiB secure region that boot0 configures, overlaps
`0x8000_0000`–`0xFFFF_FFFF`. It records the decoded size and the secure
region's base and size in the boot record. `memory.c` refuses preparation
when it returns 0. Calling it on each boot, instead of reading a retained
word, keeps a result from an earlier boot from admitting this one. The
location of the secure region is not yet recorded: no file in this
repository names it, and `docs/designs/dram-controller.md` leaves the boot0
trace that would name it to a later board session.

The layout owner raises the header constant only when the stage-2
prerequisites of `docs/designs/memory-architecture.md` hold. These are the
decode check above and launch evidence that no boot-loader payload, remote
firmware or bus master left running by boot0 uses
`0x8000_0000`–`0x1_0000_0000`. Until then the branch is compiled out.

## Reused and removed

- Reused: the table builder, both enable primitives, the publication
  primitive, the eight-core record barrier, the golden EL1 map in
  `checks/device-memory-capacity.c` with its scan for table writers outside
  the builder, and the Unicorn harness of `checks/a733-cpu-coherence.py`,
  which already traces every `msr`, `dc`, `tlbi` and `dsb` and holds every
  case of the flush and EL1 table-check primitives.
- Removed: the 16 later writes to level-1 entry 0; the EL1 TCR value repeated
  in ten files that describe current images (two assembly sources and eight
  host checks, listed below). Assembly takes the value from
  `include/a733_cpu_coherence.h`, whose C declarations move under
  `#ifndef __ASSEMBLER__`; each `mov`/`movk` triple keeps three instructions.
  Python reads both values through one new reader, `tools/a733_tcr.py`, that
  parses the header and refuses any other selection of
  `A733_CPU_COHERENCE_TCR`.
- Literal sites that change: `platform/a733/cpu_coherence.S`,
  `platform/a733-smp-cached/continue.S`; `checks/a733-cpu-coherence.py` and
  the board, crypto, GPU, NPU, PWM, RTC and thermal single-core checks, whose
  translation model in `checks/storage-read-single-core.py` also reads the
  header value. The coherence check compares the eight-core value and the
  single-core checks the cached value. Board evidence recorded with the old
  value stays valid for the commit it names.
- Literal sites that stay: the non-cacheable continuation profile in
  `platform/a733/cpu_continue.{S,c}` and `checks/a733-cpu-continue.py`
  (`MAIR = 0x4400`, where all RAM is non-cacheable), the frozen
  `apps/gpu-finite/return-check.py`, and every `TCR_EL3` literal. The native
  startup and cache probes also keep the old value:
  `boot/a733/native-el1-cache-probe.S`, `checks/native-startup-probe.py`,
  `tools/run-native-startup-probe.py`, `tools/prepare-primary-cache-probe.py`,
  `tools/secondary-cache-host.py` and `tools/all-core-pre-scheduler-host.py`.
  They enter EL1 through a production eight-core image pinned by SHA-256,
  whose `a733_cached_enter_el1` sets `0x200803019`, the eight-core value that
  this change keeps; the value they compare is that image's, and it changes
  only with a new pin.

The changes to `platform/a733-smp-cached/continue.S` are the TCR triple, which
keeps its length and, in the eight-core images, its value, an `#include "a733_cpu_coherence.h"` on line 2 and the
removal of one of two consecutive blank lines before line 273. Three private builds patch that file
and pin its SHA-256, and their hooks were reviewed against the change. The
observe hook (`platform/a733-smp-observe/continue.patch`, which the
supervised build also applies) adds an SVC 120 dispatch at line 336. The
storage hooks (`platform/a733-storage-maintenance/continue.patch`) change the
fatal path at line 273 and the EL1t vectors at line 307. No hook reads TCR or
the tables, and every hunk applies with `--fuzz=0` at the same offset as
before the change.

Changes outside this package's files, for the primary agent and other owners,
in the same merge:

- `cmake/a733-smp-observe.cmake` line 33, `cmake/a733-smp-supervised.cmake`
  line 34 and `cmake/a733-storage-maintenance.cmake` line 31: replace the
  pinned `continue.S` hash with the new one.
- `platform/a733/platform.c`: run the flush primitive over `level1` and
  `level2` before the first write and again after the last write, before
  `msr ttbr0_el1`. These tables are written with the MMU off and already
  walked cacheably without maintenance.
- `platform/a733-smp-cached/continue.c`: drop the `DC CVAC` publication of
  `__tables_start`–`__tables_end`, which finds no line after step 1.
  `platform/a733-smp-supervised/publication.S`: drop the Non-secure `DC IVAC`
  of the EL1 root and level-2 pages, which step 3 covers.
- `checks/device-memory-capacity.c`: the expected EL1 level-1 entry 0 gains
  `2 << 8`, and the writer scan can then require zero writers. When the
  layout constant becomes 2, the assertion at line 300 that entries 2 to 511
  are invalid admits the two stage-2 values above, and the mapped total
  becomes 2,958 MiB. The interior-fault fixture (mode 1) clears level-2
  entry 100 after preparation; it must subtract that entry from
  `a733_cached_el1_table_sum[0]` as well, or every core refuses at the table
  check before the pool preparation that the case exercises.
- `checks/device-memory-capacity.py` and `checks/cached-device-reservations.py`:
  the table section measures `5 * 4096 + 64` bytes.
- `checks/a733-cpu-coherence.py`: the stub file defines
  `a733_cached_el1_table_sum`, which `cpu_coherence.S` now references, and the
  EL1 enable cases need a root at `0x42000000` whose entry 1 names a level-2
  page, with the recorded sum. This file is the only home of the primitive
  cases. A corrupted checksum and a root entry 1 that lies outside the monitor
  interval, names the root itself or carries attribute bits must leave
  `SCTLR_EL1.M` clear and write no system register. `check_el1` must write no
  system register on success or refusal. The flush primitive must clean every
  line of a partial range at EL3 and EL1 and refuse, with no `DC`, a range
  below or beyond DRAM, an empty range, EL2, unmasked DAIF, `SCTLR.C` set at
  EL3 or EL1 and an expired deadline.
- `checks/a733-smp-cached.py`: the model refuses any CPU access to
  `0x421f0000`–`0x42200000` once a core has enabled EL3 translation. Each core
  now reads the two EL1 table pages and the checksum line at EL1 with the MMU
  off before its own enable, so the rule must admit those reads and keep
  refusing every write.
- DRAM-controller package: `a733_dram_decode_check()` as specified above.

## Performance budgets

| Metric | Baseline | Budget | Measured by |
| --- | --- | --- | --- |
| Table writes after the builder returns | 16 statements (counted) | 0 | capacity check writer scan |
| EL1 TCR value repeated outside the header and its reader | 16 files (counted) | 0 | source scan in new `checks/cpu-table-walks.py` |
| Flush before the first table write in `a733_cached_memory_prepare()` | not checked | flush call precedes the builder call | disassembly scan in `checks/cpu-table-walks.py` |
| `enable_el1` instructions, success path | 75 (counted) | at most 2,500 | coherence check emulation |
| Table-line maintenance per boot, cached eight-core image | 320 `DC CVAC` (counted) | 320 `DC CIVAC` at EL3 plus 128 per core, 1,344; 1,664 until the `continue.c` publication is dropped | coherence check `DC` trace |
| Table-line maintenance per boot, cached single-core image | 0 | 448 | same |
| Table readback per core and boot | 0 | 512 `LDP` (1,024 words) | coherence check emulation |
| DRAM mapped at EL1; table memory | 910 MiB; 20 KiB | 910 MiB until stage 2, then 2,958 MiB; 20 KiB plus 64 B | capacity check golden map |
| Descriptor reads served by DRAM per EL1 walk | every read that misses the walk caches (non-cacheable) | only reads that miss the data caches | board, below |

The added boot work is about 1,000 cache-maintenance operations, an
estimated 0.15 ms at 494 MHz if each costs at most 70 cycles, and 512
uncached loads per core, an estimated 0.1 ms at 200 ns each. Both run once
per boot. Every image that links a changed file is rebuilt, and every patch
applies with fuzz 0. QEMU does not model caches or walk latency, so it shows
only that each image still reaches the scheduler with the header's TCR.

## Implementation results

Counts come from `checks/cpu-table-walks.py`, run on the cached eight-core
diagnostic image built from this change and from the integration commit before
it (`--baseline`). The writer scan is the one in
`checks/device-memory-capacity.py`.

| Metric | Baseline | Budget | Measured |
| --- | --- | --- | --- |
| Table writes after the builder returns | 17 scan hits: 16 statements and one patch context line | 0 | 0 |
| EL1 TCR value repeated in files that describe current images | 10 files | 0 | 0; 11 allowed files remain (header, continuation profile, frozen images) |
| Flush before the first table write | not checked | flush call precedes the builder call | precedes it, and every flush line precedes the first table-section write |
| `enable_el1` instructions, success path | 75 | at most 2,500 | 2,233 |
| `TCR_EL1` installed by the eight-core entry; by the single-core `platform_init()` | `0x200803019`; `0x200803019` | `0x200803019`; `0x200803519` | `0x200803019` in the emulated eight-core entry; the single-core value's low half loaded before every `TCR_EL1` write |
| Table-line maintenance per boot, eight-core image | 320 `DC CVAC` | 1,344; 1,664 until the publication is dropped | 1,345; 1,666 until the publication is dropped |
| Table-line maintenance per boot, single-core image | 0 | 448 | 449 |
| Table readback per core and boot | 0 | 512 `LDP` | 512 `LDP` |
| DRAM mapped at EL1; table memory | 910 MiB; 20,480 B | 910 MiB; 20,544 B | 910 MiB; 20,544 B |

The implementation differs from the design above in these points:

- The EL3 flush covers the whole table section, so it includes the checksum
  line: 321 lines instead of 320. The checksum is a 64-byte array, so the line
  holds nothing else. Clang emits the section's objects in the order in which
  `memory.c` first names them, and `memory.c` names the checksum last; the new
  check verifies that it follows the five pages in each image.
- Only whole table pages get the EL3 Normal noncacheable mapping. The partial
  page that holds the checksum keeps the Device mapping, which leaves the EL3
  map unchanged.
- To stay within the instruction budget, the flush loop handles four lines
  and the readback loop one line per pass.
- The EL1 table check and the EL1 enable sit in their own input section,
  `.text.a733_cpu_coherence_el1`, with their own refusal exit. Both read
  `a733_cached_el1_table_sum`, which only the cached table builder defines,
  so an image that links `cpu_coherence.S` for publication alone, such as the
  microSD single-core image, drops them under `--gc-sections`. A program that
  calls only `a733_cpu_coherence_publish()` links with `ld.lld --gc-sections`
  against this file, and fails with an undefined `a733_cached_el1_table_sum`
  when both functions share the main section. The microSD single-core image
  does not build for an unrelated reason: five objects of its USB2 host
  library do not compile (`hcd.h` is missing and `TUSB_SPEED_SUPER` is
  undeclared). Its link command, run on the other 161 objects, reports only
  the eight USB2 host symbols as undefined. With an object of
  `cpu_coherence.S` that keeps both functions in the main section, it also
  reports `a733_cached_el1_table_sum`.
- `a733_cached_memory_prepare()` takes no deadline argument, so its flush uses
  a 10 ms bound from its own counter read. The single-core `platform_init()`
  also passes 10 ms.
- `checks/cpu-table-walks.py` holds only the cases that need a linked image:
  the TCR literal scan and header values, the flush before the builder and
  before the first table write, the checksum layout and the checksum as the
  last table write, the success-path counts, the writer scan, the eight-core
  `TCR_EL1` value and, in each single-core image, the table check and the
  cached value before the `TCR_EL1` write. The primitive cases are in the
  changes listed above for `checks/a733-cpu-coherence.py`, which is outside
  this package. Those changes and the ones for the capacity, reservation and
  eight-core checks were applied to a copy of this change. In that copy the
  coherence check passes 41 cases and the reservation check and
  eight-core scheduler model pass. The capacity check passes its four modes
  on eight QEMU CPUs with 392 translations and 22,547 preparation
  instructions, the same counts as before.

## Board evidence required

The board runs below are specified here and run only in scheduled board
sessions. Each records the CPU-L, CPU-B and DSU clock rates. Images built from
this change may run on the board now. A Non-secure dirty line that covers an
EL1 table page when a core enters EL1 is written back by that core's flush.
The table check then refuses before any translation register is written, so
the line stops the core at entry and no walk reads the corrupted table. The dirty-line experiment is the first run of the
first board session that uses these images, because it shows whether that
refusal occurs.

- **Non-secure dirty lines over the tables.** On the Cortex-A55, `DC IVAC`
  cleans a dirty line before invalidating it
  (TRM 100442_0200_00, section A6.2.4), and `DC CIVAC` always does. If a
  Non-secure dirty line covers an EL1 table page at entry, step 3 writes it
  back over the descriptors, and the step 3 check then stops the core.
  `docs/cold-entry.md` specifies this experiment for each launch path. EL3,
  with the MMU off, fills the five pages with a known pattern. It cleans them
  with `DC CVAC` through the Non-secure alias, using the temporary mapping of
  `platform/a733-smp-supervised/publication.S`. It then reads them back
  through the Secure alias and records every changed word. The experiment
  passes when no word changes in three cold boots per path. A failure moves
  the Non-secure flush before the first write, which needs an EL3 mapping of
  the Non-secure alias during the build.
- **Boot with the new profile.** Each changed image class boots to the
  scheduler, and its recorded `TCR_EL1` equals its header value: the cached
  value in the single-core images and the uncached value in the eight-core
  images.
- **Core-0 walk A/B run.** Image A is a cached single-core image built from
  the commit before this change and image B the same image built from this
  change. A forced-walk loop, `TLBI VMALLE1`, `DSB`, `ISB` and one load from
  a different 2 MiB block per iteration, records `CPU_CYCLES` (event `0x11`)
  and `DTLB_WALK` (`0x34`) on core 0. A GPU frame, an NPU inference and a
  recognizer inference record the same events and `ITLB_WALK` (`0x35`) on
  core 0. B must not use more cycles than A beyond the spread of five runs;
  otherwise `A733_CPU_COHERENCE_TCR_CACHED_WALK` reverts to the uncached
  value. The estimate is 50 to 75 cycles per DRAM descriptor read at
  494 MHz against 30 to 50 cycles for an L3 hit. Steady-state walks should be
  rare, because the A55's 1,024-entry level-2 TLB holds 2 MiB blocks (TRM
  section A5.2.2) and the EL1 map needs fewer than 470 entries.
- **Core-6 walk A/B run, which admits the eight-core images.** The harness
  is the secondary cached-entry probe, `boot/a733/native-el1-cache-probe.S`
  built with `SUNSTONE_SECONDARY_CACHE`, which completes cached entry on both
  Cortex-A76 cores through a production eight-core image pinned by SHA-256.
  Two steps precede the run. First, the probe gains the forced-walk loop above
  before the scheduler starts and stores `CPU_CYCLES` and `DTLB_WALK` in that
  core's probe record. Second, image B is built from this change with the
  eight-core branch of the header selection set to
  `A733_CPU_COHERENCE_TCR_CACHED_WALK`, and the probe's tools pin it beside
  image A, the eight-core image built from this change. The probe's expected
  `TCR_EL1` is the pinned image's value. The eight-core images take the
  cached value only when B passes the same rule on core 6, for the loop and
  for the three workloads run on core 6.
- **Stage 2.** The decode check reports 12,288 MiB and the secure region's
  location, then EL1 makes one read and one write per 1 GiB block.

## Sources and unproven sequences

- TCR walk attributes: Trusted Firmware-A revision `0892ad4`,
  `lib/xlat_tables_v2/aarch64/xlat_tables_arch.c`, `setup_mmu_cfg()`, sets
  `TCR_RGN_INNER_WBA | TCR_RGN_OUTER_WBA` with Inner Shareable walks whenever
  its tables are cacheable. The archive hash is in
  [docs/cpu-coherence.md](../cpu-coherence.md).
- Device shareability: the same revision's `xlat_tables_core.c` treats Device
  memory as Outer Shareable regardless of the SH field, so the moved
  `SH = 0b10` on level-1 entry 0 does not change behaviour.
- Walk cacheability and PMU events: Cortex-A55 TRM 100442_0200_00
  (SHA-256 `1f71b74d…7dd97`), sections A5.5.1 (walks are cacheable only with
  write-back IRGN and ORGN and the cache enabled), A5.2.2, A6.2.4 and C2.4.
- Unproven and unpinned:
  - the Cortex-A76 equivalents of those TRM sections, for which the core-6
    A/B run supplies a measurement;
  - the Arm Architecture Reference Manual rules that EL3 with the MMU off
    addresses the Secure space, that EL1 with the MMU off addresses the
    Non-secure space, and that maintenance issued with the MMU off reaches
    every core;
  - whether the A733 has a system cache beyond the DSU.

## Left out

- Cacheable EL3 walks: EL3 walks only at boot and on faults, and its tables
  must stay non-cacheable for its data accesses.
- One flush on core 0 instead of one per core: it saves 896 operations per
  boot but needs the EL1 entry order in `continue.c` to change.
- The contiguous hint on `dma32` blocks: the 2 MiB blocks already fit the
  level-2 TLB; it returns if the A/B run shows `DTLB_WALK` costing more than
  1% of `CPU_CYCLES` in a workload.
- Moving the single-core `platform_init()` onto `enable_el1`: it would change
  the sequence that seven emulator checks model, beyond their literal.
- Stage-3 entries 4 to 12: they need launch evidence for
  `0x1_0000_0000`–`0x3_4000_0000`.
