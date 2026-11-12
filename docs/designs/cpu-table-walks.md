# Design: cacheable EL1 translation-table walks and one table builder

Status: **implemented**; modeled counts are **host-tested**. **Qualified on the
board** for the core-0 forced-walk loop on the cached single-core images
(Cortex-A55). **Open:** the core-0 workload runs, the Cortex-A76 core-6 run
that admits the eight-core images, the Non-secure dirty-line experiment and the
stage-2 block access. Readers are assumed to know AArch64 stage-1 translation,
the Secure and Non-secure physical address spaces, and data-cache maintenance
by virtual address.

The cached images build five 4 KiB translation-table pages in the monitor
interval at `0x421f0000`: level 1, level 2 and level 3 for EL3, and level 1 and
level 2 for EL1. The EL1 walker reads descriptors as write-back cacheable
memory in the cached single-core images and as non-cacheable memory in the
eight-core images. The header
[a733_cpu_coherence.h](../../include/a733_cpu_coherence.h) holds both values,
`A733_CPU_COHERENCE_TCR_CACHED_WALK` (`0x200803519`, IRGN0 = ORGN0 = 0b01,
the value `platform_init()` in `platform/a733/platform.c` uses for its own
tables) and `A733_CPU_COHERENCE_TCR_UNCACHED_WALK` (`0x200803019`), and defines
`A733_CPU_COHERENCE_TCR` as the cached value when the target defines
`SUNSTONE_CACHED_SINGLE_CORE` or enables `SUNSTONE_A733_CACHEABLE_TABLE_WALKS`.
The eight-core default leaves that option disabled. Assembly
takes the value from the header, Python reads it through `tools/a733_tcr.py`,
and every user, including the profile check in
`platform/a733/device_memory_layout.c`, compares with its own image's value.
The eight-core images stay uncached until the core-6 comparison passes.
They still run the flush and the table check below: with non-cacheable
walks the flush removes a stale line that a later eviction could write over the
tables, and the check refuses tables such a write-back corrupted.

A cacheable walker can hit a stale cache line over a table page, so the images
add two cache-maintenance steps and a per-boot check of the EL1 tables, and
finish every boot EL1 descriptor inside one builder. Application address spaces
have separate cacheable tables and an explicit publication barrier.

## Translation-control consumers

The header is the sole definition of the cached and uncached EL1 values.
`checks/cpu-table-walks.py` scans tracked source for full literals and assembly
instruction triples and permits them only in that header. The sixteen other
consumers use named values, including patch context and computed-value fixtures:

| Consumers | Selection |
| --- | --- |
| `platform/a733/platform.c`, `apps/storage-read-single-core/platform.patch` | Cached walks for the single-core platform |
| `platform/a733/cpu_continue.c`, `platform/a733/cpu_continue.S`, `checks/a733-cpu-continue.py` | Explicit uncached value for the Normal-NC continuation profile |
| `boot/a733/native-el1-cache-probe.S`, `checks/native-startup-probe.py`, `tools/run-native-startup-probe.py` | Explicit uncached value of the digest-pinned native image |
| `apps/gpu-entry-checkpoint/check.py`, `apps/gpu-finite/host-check.py` | Explicit uncached value of their frozen images |
| `apps/gpu-cache-permission/main.c`, `apps/gpu-cache-permission/check.py` | Explicit cached value of the permission diagnostic |
| `apps/board-bundles-cpu/main.c`, `tools/prepare-board-bundles.py` | Both values for the forced-walk comparison |
| `checks/memtest-map-admission.py`, `checks/region-el0.c` | Explicit cached value for the mapping fixtures |

The Python reader exports `CACHED_WALK` and `UNCACHED_WALK` for fixed profiles,
and `SINGLE_CORE` and `EIGHT_CORE` for the corresponding current image defaults.
Frozen fixtures select a fixed profile independently of those defaults.
`SUNSTONE_A733_TABLE_WALKS=cached|uncached` selects either experimental image
through CMake. The default selection follows the header. Each linked image
exports its effective value as the absolute symbol `a733_cpu_coherence_tcr`;
the Python reader's `for_image()` reads it when checking an overridden build.

## Ownership and data flow

1. **Cold entry, core 0, EL3, MMU and caches off.** At the start of
   `a733_cached_memory_prepare()`, core 0 runs `DC CIVAC` over the whole table
   section (the five pages and the checksum line) before the first descriptor
   write. EL3 with the MMU off addresses the Secure space, so this removes any
   Secure dirty line that could later be evicted over a descriptor. The flush
   is bounded by 10 ms.
2. **Build.** `a733_cpu_coherence_tables()` and the rest of
   `a733_cached_memory_prepare()` write every entry of the five boot pages;
   these pages remain immutable afterwards. Level-1 entry 0, the MMIO block, carries the
   outer-shareable field (`SH = 0b10`) in both regimes. As its last step core 0
   stores a 64-bit additive checksum of the two EL1 pages in
   `a733_cached_el1_table_sum`, a 64-byte-aligned word in
   `.bss.a733_cached_tables`. Only whole table pages get the EL3 Normal
   non-cacheable mapping; the partial page holding the checksum keeps the
   Device mapping.
3. **EL1 entry, every core, MMU and caches off.** Before installing TCR, TTBR
   and SCTLR, `a733_cpu_coherence_enable_el1()` runs `DC CIVAC` over the EL1
   root page and the level-2 page named by the root's entry 1, then `DSB SY`.
   EL1 with the MMU off addresses the Non-secure space, the alias the walker
   reads, so stale Non-secure lines are removed before the first walk. The core
   then reads both pages back with Device loads (512 `LDP`), sums them and
   compares the sum with the recorded checksum. A mismatch returns failure
   before `SCTLR_EL1.M` is set, and the caller stops the core through its
   existing fault path. The cached single-core images enable translation in the
   patched `platform_init()` (`apps/storage-read-single-core/platform.patch`),
   which calls an exported copy of the same sequence built from one assembler
   macro in `platform/a733/cpu_coherence.S`. `platform_init()` also flushes its
   own `level1` and `level2` pages before the first write and after the last,
   before `msr ttbr0_el1`.
4. **Runtime.** The EL1 walker is the only runtime reader of the boot EL1 tables. EL1 has no
   data mapping of the monitor interval (EL1 level-2 entry 16 is invalid), and
   EL3 does not access the tables after preparation.

### Application tables and mutable mappings

Application admission in `apps/os/large.c` builds an inactive address space with
`app_space_build_l2()` in `kernel/appmem/app_region.c`. Its arena uses Normal
write-back stores. Publication completes `DSB ISH` for cacheable walks, or
`DC CIVAC` over the arena followed by `DSB SY` for uncached walks, before the
release store to `os_space_active`. A fresh ASID is allocated once and never
reused. The task-switch hook installs TTBR0 and the exception return synchronizes
the context; an inactive table with a fresh ASID needs no TLBI.

The CPU cache-policy converter in `kernel/umem/umem_policy.c`, when linked,
changes active descriptors through cacheable mappings. It completes descriptor
stores with `DSB ISH`, broadcasts `TLBI VAAE1IS`, then completes `DSB ISH` and
`ISB`. The make step has its own `DSB ISH` and `ISB`. The split path publishes
the new child page before breaking the parent block. No cache clean substitutes
for these barriers. `checks/umem-policy.py` checks split and conversion order.

The remaining writer audit covers these paths:

| Writer | Publication and scope |
| --- | --- |
| `platform/a733-smp-cached/memory.c`, `platform/a733/cpu_coherence.c` | Cold EL3 stores with translation and caches off; `continue.c` completes `DSB SY` before admission of any walker |
| `platform/a733/platform.c`, `include/a733_memory_boot.h` | Cold EL1 stores, pre/post `DC CIVAC` and `DSB SY` before TTBR0 and TLBI |
| `platform/a733/cpu_continue.c` | Separate Normal-NC profile; cold table stores followed by `DSB SY` |
| `boot/a733/native-ns-publication.S`, `platform/a733-smp-supervised/publication.S` | Inactive temporary EL3 tables, clean and `DSB SY` before TTBR0, TLBI and completion barriers; EL3 walks remain uncached |
| `apps/sd-read-probe/region.c`, `apps/sd-backup-probe/region.c` | Cacheable inactive application tables; code publication and space switch both complete `DSB ISH`; local TLBI completes before execution |
| `apps/os/large_load.c`, `boot/a733/stage2/loader.c` | Payload stores only; neither writes CPU translation tables |

Cold builders retain their MMU-off store contract. Their descriptors already
reach memory before a cacheable walker starts. The linked host cases in
`checks/table-walks.py` verify application publication before space exposure,
reject omitted barriers and uncached cleans, and check both build selections.

The primitive `a733_cpu_coherence_flush(base, bytes, deadline)` in
`platform/a733/cpu_coherence.S` requires EL3 or EL1 with the MMU and data cache
off, masked exceptions, 64-byte data lines and a range inside
`0x40000000`–`0x48000000`. It runs `DC CIVAC` per line, checks the deadline
before and after the loop and ends with `DSB SY`; `DC CIVAC` never discards
data, so a deadline refusal leaves memory unchanged. The publication primitive
keeps its clean-only contract. The EL1 table check and enable sit in their own
input section `.text.a733_cpu_coherence_el1`, because they read
`a733_cached_el1_table_sum`, which only the cached table builder defines; an
image that links `cpu_coherence.S` for publication alone drops them under
`--gc-sections`.

Nothing here takes or waits for an interrupt; the only polling is the counter
read of the deadline check. The EL3 walk and every EL3 data access to the tables
stay non-cacheable, Inner Shareable (`A733_CPU_COHERENCE_TCR_EL3` is
`0x80823019`); a cached EL1 walk is write-back, Inner Shareable, so walker
lines are coherent with every core's data cache and the DSU L3. No lock is
needed, because the tables are immutable once core 0 returns from preparation
and the record barrier in `continue.c` holds every secondary until then. For a
stale line in another core's cache to be removed, `DC CIVAC` issued with the MMU
off must reach every cache in the Outer Shareable domain, which the Arm
Architecture Reference Manual states for Device-type accesses; the manual is not
pinned. The step 3 check detects a write-back that reached memory before the
check; the dirty-line experiment bounds later ones.

## Stage-2 blocks above the boot image

When `a733_dram_decode_check()` returns 1 in `memory.c`, the builder maps DRAM
above the boot image at EL1 as write-back memory with `A733_EL1_DRAM_BLOCK`. It uses 2 MiB level-2 blocks for `0x48000000`–`0x79000000` (unless the extended device-memory layout already maps it) and 1 GiB level-1 blocks for
`0x80000000` up to `A733_DRAM_MAP_END` (`0x3_4000_0000`). EL3 entries stay
invalid, except the blocks that an OS image's work and output regions need for
the return path. A mapped write-back block receives speculative reads, so the
check runs at EL3 on every boot, rather than reading a retained word that an
earlier boot could have left. The default definition in `memory.c` is weak and
returns 0, so no entry above the boot image is written. An image that sets
`SUNSTONE_A733_DRAM_DECODE_PROBE` links
`drivers/dram/a733_dram_decode_probe.c`, whose address test writes a distinct
token to each probe address and reads every token back to refuse aliased or
absent cells within the tested map (the basis is in
[memory-map-12g.md](memory-map-12g.md)). The location of the 120 MiB secure
region that boot0 configures is not recorded in this repository.

## Qualified figures

Counts come from `checks/cpu-table-walks.py` and the other checks named in the
record; the walk-loop figures are a board measurement.

| Figure | Budget | Result | Evidence |
| --- | --- | --- | --- |
| Table writes after the builder returns | 0 | 0 (17 before) | `cpu-table-walks.jsonl`: `cpu-table-walks-host-budgets` |
| `enable_el1` instructions, success path | at most 2,500 | 2,233 | same |
| Table-line maintenance per boot, eight-core; single-core | 1,344; 448 | 1,345; 449 (the flush includes the checksum line) | same |
| Table readback per core and boot | 512 `LDP` | 512 `LDP` | same |
| Forced-walk loop on core 0, cycles per iteration, uncached; cached | cached no slower | 693.1; 436.1 (spread under 0.2%) | `cpu-table-walks.jsonl`: `cpu-table-walks-board-core0-loop` |

The added boot work is about 1,000 maintenance operations, an estimated 0.15 ms
at 494 MHz if each costs at most 70 cycles, plus 512 uncached loads per core, an
estimated 0.1 ms at 200 ns each; both run once per boot. QEMU does not model
caches or walk latency.

## Constraints

- The non-cacheable continuation profile (`platform/a733/cpu_continue.{S,c}`,
  `MAIR = 0x4400`) and the frozen native and GPU fixtures select the shared
  uncached constant. Every `TCR_EL3` literal retains its separate profile.
  The probes enter EL1 through a production eight-core image pinned by SHA-256.
- Three private builds patch `platform/a733-smp-cached/continue.S` and pin its
  SHA-256 in `cmake/a733-smp-observe.cmake`, `cmake/a733-smp-supervised.cmake`
  and `cmake/a733-storage-maintenance.cmake`; their hooks read neither TCR nor
  the tables.
- `checks/a733-cpu-coherence.py` is the only home of the primitive cases (a
  corrupted checksum or a root entry 1 outside the monitor interval leaves
  `SCTLR_EL1.M` clear and writes no system register; the flush refuses ranges
  outside DRAM, an empty range, EL2, unmasked DAIF, `SCTLR.C` set and an
  expired deadline). `checks/cpu-table-walks.py` holds the cases that need a
  linked image.

## Sources and unproven items

- TF-A `0892ad4` `lib/xlat_tables_v2/aarch64/xlat_tables_arch.c`
  (`setup_mmu_cfg()`) sets `TCR_RGN_INNER_WBA | TCR_RGN_OUTER_WBA` with Inner
  Shareable walks whenever its tables are cacheable; its `xlat_tables_core.c`
  treats Device memory as Outer Shareable regardless of `SH`. The archive hash is
  in [cpu-coherence.md](../cpu-coherence.md).
- Cortex-A55 TRM 100442_0200_00: A5.5.1 (walks are cacheable only with
  write-back IRGN and ORGN and the cache enabled), A5.2.2, A6.2.4 and C2.4.
- Cortex-A76 TRM 100798_0401_00_en, r4p1, A5.5.1: both inner and outer
  write-back encodings make table walks look up the data cache. Other encodings
  access external memory. The [source record](../cpu-table-walks-sources.json)
  pins the PDF and extracted text. This establishes the supported configuration;
  walk cost remains a board measurement.
- Unproven: the Arm Architecture Reference Manual
  rules that EL3 with the MMU off addresses the Secure space, that EL1 with the
  MMU off addresses the Non-secure space, and that maintenance issued with the
  MMU off reaches every core; whether the A733 has a system cache beyond the DSU.

## Not built

Cacheable EL3 walks (EL3 walks only at boot and on faults); one flush on core 0
instead of one per core (saves 896 operations per boot but changes the EL1 entry
order in `continue.c`); the contiguous hint on `dma32` blocks (it returns if a
workload run shows `DTLB_WALK` costing more than 1% of `CPU_CYCLES`); moving the
single-core `platform_init()` onto `enable_el1`; stage-3 entries 4 to 12, which
need launch evidence for `0x1_0000_0000`–`0x3_4000_0000`.

## Core-6 measurement

The OS diagnostic in `apps/table-walks/` runs one pinned EL1 worker on core 6.
Its controller uses the application kit and waits for the CPU clock owner to
reach the granted top points. The image-side privilege hook grants EL1 only
to that diagnostic worker. The linked OS, scheduler and returning lifetime
remain the production paths.

Each image runs one warm-up block and five measured blocks of 1,024 iterations.
Each iteration invalidates the local EL1 TLB, completes `DSB ISH; ISB` and
loads one line from the next mapped 2 MiB DRAM block. The loop skips the monitor
hole and visits 63 blocks. Its measured interval uses registers only, apart from
the target loads. It records cycles, counter ticks, data and instruction walks,
bus accesses and a load digest. Interrupts are masked only around each block.
The decoder checks core identity, implemented events, the cycle/counter ratio,
walk counts and unchanged top clock settings. It reports the five-block median
and spread. An improvement must exceed the larger spread of the two images.

Build with `SUNSTONE_A733_OS=ON`, `SUNSTONE_APPLICATION=table-walks` and an
explicit `SUNSTONE_A733_TABLE_WALKS=uncached` or `cached`. Build the typed
session with `tools/session.py build --clock-grant`. Freeze the standalone decoder
with `apps/table-walks/host.py --freeze PATH` and supply that path to
`tools/session.py build --decoder PATH`. The frozen decoder embeds both fixed
values from the shared header and refuses changes to any TCR field.
The queue runs the uncached image first and the cached image only after a pass.
Any first miss ends the batch. Predictions and timing budgets belong in the
individual evidence records.

## Open work

Each run records the CPU-L, CPU-B and DSU clock rates and runs in a scheduled
board session.

1. **Non-secure dirty-line experiment**, first run of the first session with
   these images (record `cpu-table-walks-dirty-line-experiment`; specified in
   [cold-entry.md](../cold-entry.md)). A failure moves the Non-secure flush
   before the first write.
2. **Core-0 workload A/B** during a GPU frame, an NPU inference and a recognizer
   inference (record `cpu-table-walks-core0-workload-ab`). It also decides the
   contiguous hint.
3. **Core-6 walk A/B on the Cortex-A76** through the OS diagnostic above
   (record `cpu-table-walks-core6-walk-ab`). The eight-core default takes the
   cached value only after a measured gain, followed by returning controls on
   both CPU clusters and recognition against the 422.8 ms, 512-frame reference.
4. **Stage-2 block access**: with the decode check passing, one read and one
   write per 1 GiB block (record `cpu-table-walks-stage2-block-access`).
5. Decide whether the interleaved single-image form of the walk A/B run is
   accepted in place of images built from two commits
   ([board-bundles-next.md](board-bundles-next.md)).
