# Design: device memory classes, boot mapping checks and protection

Status: **design**; no code exists yet. Physical behavior is unqualified until
the board evidence at the end exists. Readers are assumed to know AArch64
stage-1 translation, the FreeRTOS SMP port and non-coherent DMA.

Ten images have a 768 MiB write-back device pool, `dma32`, and a 16 MiB
Normal non-cacheable pool, `uncached`. Boot checks both pools page by page
with 401,408 address translations (`AT`), and nothing serializes the pool
ledger across cores. Stage 1 checks one `AT` per 2 MiB block, serializes every
ledger operation with the kernel's critical section, aligns large extents to
the device MMU's largest page and moves the after-acquire clean into the pool,
limited to pages that can hold dirty lines. Stage 2 adds a 1 GiB class each for
the GPU and the NPU. Stage 3 maps the DRAM above 4 GiB when a consumer claims
it.

## Map and protection

Tasks start at EL0 unless the image selects EL1, and EL0 tasks call kernel
functions directly, so kernel data, stacks and heap stay EL0 read-write. Pool
memory has no EL0 access, so every lease holder that touches its memory is an
EL1 task by the map.

| Physical range | Use | EL1 descriptor | EL0 |
| --- | --- | --- | --- |
| `0x0`–`0x4000_0000` | MMIO | 1 GiB block, Device-nGnRnE, XN | none |
| `0x4000_0000`–`0x4100_0000` | boot0 and launcher | 2 MiB blocks, WB, XN | read-write |
| `0x4100_0000`–`0x4120_0000` | text and read-only data | 1 block, WB, read-only | read-only, executable |
| `0x4120_0000`–`0x4200_0000` | data, BSS, stacks, heap | WB, XN | read-write |
| `0x4200_0000`–`0x4220_0000` | monitor and tables | invalid | none |
| `0x4220_0000`–`0x4800_0000` | boot assets; recognizer models, input, arena | WB, XN | read-write |
| `0x4800_0000`–`0x7800_0000` | class `dma32` | 384 blocks, WB, XN | none |
| `0x7800_0000`–`0x7900_0000` | class `uncached` | 8 blocks, Normal-NC, XN | none |
| `0x7900_0000`–`0x8000_0000` | no owner | invalid | none |
| `0x8000_0000`–`0x1_0000_0000` | classes `gpu`, `npu` (stage 2) | two 1 GiB blocks, WB, XN | none |
| `0x1_0000_0000`–`0x3_4000_0000` | no owner until stage 3 | invalid | none |

WB is Normal Write-Back Write-Allocate, Inner Shareable. Every writable entry
is execute-never at EL0 and EL1. The two invalid holes inside DRAM cover
114 MiB, so EL1 maps 910 MiB of DRAM in stage 1, 2,958 MiB in stage 2 and
12,174 MiB in stage 3, in five table pages. Stage 3 adds level-1 entries 4 to
12 as 1 GiB blocks and no table page.

The local layout maps only `0x4000_0000`–`0x4800_0000` of DRAM and differs from
the table above in two places. Its write-back pool, at most 2 MiB, lies after
BSS and below `0x41f0_0000` inside the WB data blocks. When the image links an
`uncached` pool, the level-2 entry for `0x41c0_0000`–`0x41e0_0000` is
Normal-NC and keeps the AP and XN bits of the data block it replaces.

The QEMU fixture `checks/device-memory-capacity.{c,py}` compares all 1,024
EL1 level-1 and level-2 entries with two golden arrays: one from the table
above for the extended-layout image, and one from the local-layout differences
for a local-layout image built by the same fixture. An EL0 task faults on each
forbidden access. Boot keeps one write-only `AT` per 2 MiB pool block; one `AT`
proves a whole block only for a block descriptor, and the golden comparison
fixes each pool entry's descriptor type. The comparison sees final tables only
after the single-builder change below deletes the two monitor writers that
rewrite level-1 entry 0 after preparation, so that change must land before
this package merges.

Each layout has one preparation call on core 0 before the scheduler starts.
It initializes the pools and sets a plain READY flag. `platform_init()` calls
the local preparation (a primary-agent change below). The getters return NULL
until READY is set. Starting the scheduler orders preparation before every
task on every core, so READY needs no atomic access, and no pool is ever
initialized concurrently. The EL1 tables do not change after preparation, so
READY serves as the mapping generation. Memory management has no interrupt or
polling path: every operation is a synchronous call from boot code or a task.

## Allocation: device-MMU alignment and a ledger inside the kernel critical section

**Alignment.** Acquire raises the caller's alignment to the class minimum,
so a caller cannot under-align. The minimum follows the largest page of the
device MMU that maps the class:

- `gpu`: 2 MiB for requests of at least 2 MiB, 64 KiB for requests of at
  least 64 KiB, otherwise 4 KiB. The GPU MMU maps 4 KiB, 64 KiB and 2 MiB
  pages (`docs/gpu-vm.md`).
- `npu`: 16 MiB for requests of at least 16 MiB, otherwise 1 MiB. The NPU MMU
  maps 4 KiB, 64 KiB, 1 MiB and 16 MiB pages and has no 2 MiB page
  (`docs/npu-mmu.md`). A 1 GiB extent then takes 64 directory entries in
  place of 1,024 with 1 MiB pages.
- `dma32`, which holds GPU and NPU buffers until stage 2: 2 MiB for requests
  of at least 2 MiB, 64 KiB for requests of at least 64 KiB, otherwise 4 KiB.
- `uncached`: 4 KiB; it holds control blocks of a few pages.

The class bases `0x4800_0000`, `0x7800_0000`, `0x8000_0000` and
`0xC000_0000` are 16 MiB aligned. The local-layout pools and pools built
inside a lease, such as the USB host driver's private pool, carry no class
minimum and keep the caller's alignment, so their placement does not change.

**Ledger.** `device_memory_pool_init()`, acquire, describe, release, retain
and `device_memory_set_state()` form the ledger. Every operation except
initialization reads and writes its slot inside `taskENTER_CRITICAL()` and
`taskEXIT_CRITICAL()`, and the slot fields, including `generation` and
`state`, are plain fields read and written only there. Argument and output
checks run before the critical section. Initialization takes no lock: a boot
pool is initialized before READY is set, and a pool built inside a lease is
initialized by the lease holder before any other task can reach it.

In the SMP image's `ARM_CR82` port, the critical section raises the GIC
priority mask through an `SVC`, so it works at EL0 and EL1, and takes the
kernel's task and ISR spinlocks, which serialize every core. Calls nest through
the kernel's per-core count, so a caller already inside a critical section
stays masked on return. Before the scheduler starts, the critical section has
the same effect as the kernel's own critical sections during task creation
at boot, in both the `ARM_CR82` port and the single-core `ARM_AARCH64_SRE`
port. The kernel asserts if the critical section
is entered from an interrupt handler; no handler calls the ledger. The
header's rule that callers serialize the ledger under their own service lock
is removed.

Acquire keeps the current first fit: a scan for a free slot, then at most 33
placement attempts, each comparing against 32 slots. The compiled loop spends
14 instructions per occupied slot, so the worst case with 31 occupied slots
holds the critical section for about 15,000 instructions. Acquire runs only at
admission. Release, retain, describe and `set_state` hold it for about 100
instructions. The locked section touches only the pool structure and makes no
call that can block or fault, so release and retain, the bounded-failure
fallbacks, always complete.

The ledger accepts these state changes and refuses every other one, leaving
the slot unchanged:

| Operation | Accepted source state | Result |
| --- | --- | --- |
| `set_state(DEVICE)` | CPU | DEVICE |
| `set_state(CPU)` | DEVICE | CPU |
| retain | CPU, DEVICE | RETAINED |
| retain | RETAINED | unchanged, success |
| release | CPU | FREE |

Nothing leaves RETAINED, so a timeout or device failure never authorizes
reuse, as `docs/memory-reservations.md` requires.

**Holder.** One task at a time holds a lease, calls its transitions and
maintains its cache lines. A hand-off to another task needs a happens-before
edge such as task creation or a queue message. Removing
`device_memory_lookup()` and renaming the slot fields turns the 60 remaining
uses in 27 consumer files and 15 host checks into compile errors. Their owners
fix them in the same merge, and each direct state write becomes a
`set_state` call.

The ledger trusts EL0 code in the same way as every kernel object in this map:
an EL0 task that writes a pool structure or the kernel's lock words in `.bss`
can corrupt ownership or hang every core. Protecting the ledger needs an
EL1-only data region (see Left out).

## Cache maintenance and placement

A previous holder can leave dirty lines in a recycled extent, and their
eviction during a device write would overwrite device output in DRAM. Every
pool records its memory type. `device_memory_pool_init()` creates a
write-back pool. The `uncached` preparation marks its pool Normal-NC, and the
USB host driver marks its private pool, which it builds inside an `uncached`
lease, the same way.

Each write-back pool also records a clean watermark: the end of the highest
page it has handed out. A page above the watermark has never been handed out,
and no Non-secure agent writes it. EL1 code writes no pool page before acquire
returns it, EL0 has no mapping of it, and the CPU never creates dirty lines
speculatively. Secure dirty lines left by boot0 or the launcher before EL3
entry cannot be removed by Non-secure maintenance at all; the cold-entry
experiment below covers them. A boot pool starts with the watermark at its
base. A pool built inside a lease also starts at its base, so its builder
initializes it directly after acquire, before writing the lease.

Inside the critical section, acquire records the slot, computes the clean
range as the part of the extent below the watermark, and raises the watermark
to the extent's end. After the critical section, acquire returns the extent in
one of two forms:

- **Plain:** on a write-back pool, `aarch64_device_cache(CLEAN)` over the
  clean range, unchanged. The helper's leading `DSB SY` orders the clean after
  this core's critical-section entry, which observed the previous holder's
  release, so the clean finds any line that holder left dirty. On a Normal-NC
  pool, or with an empty clean range, nothing.
- **Zeroed** (a `zero` argument of `device_memory_acquire_aligned()`): one
  read of `DCZID_EL0` before the critical section refuses unless zeroing is
  permitted with a 64-byte block. Then `DC ZVA` over the extent, followed by
  the plain clean over the whole extent, because `DC ZVA` can leave the zeroed
  lines dirty. A Normal-NC pool runs no clean. The zeroed form replaces the
  `memset` calls over fresh extents, on both pools, in
  `platform/a733/gpu_compute_reserved.c`,
  `platform/a733/gpu_graphics_memory.c` and `apps/uart/dma.c`.

If the clean is refused, acquire releases the slot and refuses; the raised
watermark only causes more cleaning later. A first GPU session open or NPU
model load therefore cleans no lines, and a reopen cleans the outer extent
once while its new inner pool cleans none.

`uncached` owners issue `DMB OSHST` before a doorbell and `DMB OSHLD` after
completion. Transitions run at session open, per 60 Hz GPU frame, per NPU
inference and per DMA period of up to 200 Hz. Each transition is one
`set_state` call plus maintenance of the (offset, length) write sets declared
at admission. The owner checks each write set once at admission with
`device_memory_describe()` and `a733_extent_within()`. Device-to-device
transitions need no maintenance. CPU to device cleans the write set with the
helper. Device to CPU runs `DMB OSHLD` after the completion read, then
`aarch64_device_cache(CLEAN_INVALIDATE)` over the write set. The CPU writes
none of a DEVICE extent, so `DC CIVAC` costs what `DC IVAC` costs and cannot
discard data if the holder rule is broken. The helper's leading `DSB SY` is
stronger than this path needs: under the Arm ARM rules for ordering
data-cache maintenance by address with Device accesses, a `DMB` between the
completion read and the `DC` would suffice. The CPU side of GPU and
NPU work, including session open, runs on cores 6 and 7 once those are
admitted.

Every maintenance form runs with interrupts unmasked. A `DSB` completes only
the maintenance its own core issued, so if a task migrates between its `DC`
loop and the trailing `DSB SY`, correctness rests on a `DSB` in the
context-switch path of `patches/freertos-smp-asm.patch`. Confirming that
barrier is a precondition for every maintenance form in this design.

## Reused and removed

- Reused: the first-fit scan and placement rule,
  `device_memory_acquire_aligned()`, the kernel critical section,
  `aarch64_device_cache()` unchanged, `a733_cpu_coherence_tables()`, the
  version-2 boot record, `a733_mapping_check_stride()` and the QEMU fixture
  with its golden array.
- Removed: the batched page-scan state machine in `device_memory_layout.c`
  with `a733_device_memory_commit_allowed()`, `same_layout()` and
  `a733_device_memory_tables_admitted()`; `device_memory_lookup()`, public
  slot fields and `memset` over a fresh extent; the callers' service-lock rule
  for the ledger; the lazy initialization in the local `uncached` and boot-pool
  getters. The GPU and NPU per-page walks become one `describe` and
  `a733_extent_within()` check per lease.

## Performance budgets

Instruction and barrier counts come from the QEMU fixture (`-icount shift=0`,
PMU instructions retired around the call) or host replay counters. Cycles come
from PMU `CPU_CYCLES` on the board and name the core type: core 0 is a
Cortex-A55 on CPU-L, core 6 a Cortex-A76 on CPU-B. Each board measurement
records the CPU-L, CPU-B and DSU rates, and pass or fail is judged on cycles.
Times below are derived from cycles at a stated rate: boot0 leaves CPU-L at
494 MHz, and the CPU-B rate is not yet known. Provisional targets become fixed
after the first measurement.

| Metric | Baseline | Target | Measurement |
| --- | --- | --- | --- |
| Extended pool check | 401,408 `AT` in 3,136 calls (counted); about 17 M instructions (estimated) | 392 `AT` in one call; at most 25,000 instructions; provisional 78,400 A55 cycles | QEMU; board core 0 |
| Local pool check | up to 2,048 `AT` (counted) | at most 3 `AT` | callback count, `checks/memory-reservations.c` |
| DRAM mapped at EL1; table memory | 910 MiB; 20 KiB | 910, 2,958, 12,174 MiB by stage; 20 KiB | QEMU golden comparison |
| Slot comparisons per acquire | 1,056 (counted) | unchanged; placement identical to the current first fit where no class minimum applies | host replay |
| Critical-section hold | no serialization | acquire at most 15,000 instructions (counted from the compiled loop); others at most 100 | QEMU SMP image; board core 0 cycles |
| Critical-section pair per ledger call | none (plain store through `device_memory_lookup()`) | today's port: 2 `SVC`, 2 priority-mask writes each followed by `DSB SY` and `ISB`, and the task and ISR spinlock pairs; after the scheduler budget of `docs/performance-architecture.md` lands: at most 100 instructions, no `DSB SY`, no `SVC` | QEMU SMP image; board cores 0 and 6 |
| Barriers per transition | 2 `DSB SY` per range (counted) | 2 `DSB SY` from the helper plus the critical-section pair of `set_state`: 4 `DSB SY` and 2 `ISB` today, 2 `DSB SY` after the scheduler budget lands | GPU and NPU host models; QEMU |
| Device-MMU entries per admitted extent | 4 KiB pages: 262,144 per GiB | GPU 512 per GiB (2 MiB); NPU 64 per GiB (16 MiB) | GPU and NPU host models |
| Extent alignment | caller's, 4 or 16 KiB | every extent meets its class minimum; in every image's admission order, no request newly refused and the largest free extent no smaller than with 4 KiB alignment | host replay |
| Lines cleaned per acquire | callers clean whole extents | only lines below the watermark: 0 on first use of a page, 0 for an inner pool's first round | host replay counter |
| Plain clean cost | done by callers | provisional 4 cycles per absent line on A55 and A76; 1 GiB is 67 M cycles, 136 ms on core 0 at 494 MHz | board, cores 0 and 6 |
| Zeroed acquire | caller `memset` (counted in three files) | at most 1.25 times a store loop over the same bytes on the same core; caller `memset` 0 | board, cores 0 and 6; GPU and NPU host models |
| GPU session open, NPU model load (stage 2) | none | first open: 0 lines cleaned; reopen: one 1 GiB clean, at most 80 M A76 cycles | host replay counter; board core 6 |
| `DC` per device-to-CPU transition | full READ and WRITE buffers (NPU, counted) | declared write set; no full-extent maintenance per frame or inference | GPU and NPU host models |

Two rows are explained regressions. Barriers per transition rise from 2 to 4
`DSB SY` until the kernel critical section is lightened; at 200 Hz the added
pair costs well under 0.1% of one core. Serializing the ledger across cores is
a correctness requirement, and the kernel critical section is the existing
mechanism for it. The 15,000-instruction acquire hold, about 30 µs on core 0
at 494 MHz with every core's kernel critical sections blocked, occurs only at
admission.

## Fixtures

- Host replay (`checks/device-memory-capacity.c`) of the admission order of
  every extended-layout and local-layout image. It counts comparisons per acquire,
  placement against the current first fit, alignment of every extent,
  device-MMU entries, the largest free extent before each later allocation,
  no newly refused request, lines cleaned per acquire, and free extents
  unchanged after every session opens and closes once. The same replay drives
  every pair of state and operation and asserts that each refused change
  leaves the slot unchanged.
- QEMU SMP image: release and retain complete while another core is inside a
  worst-case acquire, and an acquire called inside an enclosing critical
  section returns with interrupts still masked.

## Left out

- Interrupt-handler callers of the ledger: no consumer needs one, and the
  kernel critical section asserts outside handlers.
- A pool-private lock. If a measurement shows the kernel critical section's
  hold hurting scheduling, the pool gets a lock again, built as a shared
  helper together with the counter-clock lock.
- A sorted index of occupied slots, which would cut acquire's hold to about
  600 instructions. It returns only if the measured acquire hold misses its
  budget or delays a running deadline, the 5 ms audio DMA period being the
  tightest today.
- Removing the leading `DSB SY` from `aarch64_device_cache()`. Under
  `docs/shared-helpers.md` a barrier change to a shared helper needs board
  qualification, and the saving is one barrier per transition. It returns if a
  per-frame measurement shows the barrier matters.
- A boot clean of the `uncached` pool. The image enters EL1 with the MMU and
  data cache off, and both the EL3 and the EL1 tables map the pool Normal-NC
  from their first installation, so no Non-secure cacheable alias exists.
- Splitting large zeroing or cleaning across cores 6 and 7: one core is
  expected to meet the session-open target; a missed measurement reopens it.
- Cacheable translation-table walks, the contiguous hint on `dma32` and the
  walk A/B run belong to the separate package described below. These pools
  do not depend on it.
- Execute-never read-only data, guard pages and an EL1-only region for kernel
  and pool structures: all need level-3 tables or a privileged data block.
- `SCTLR_EL1.WXN`: the golden comparison already excludes writable executable
  entries.
- Read-only mapping of admitted NPU models: changes the tables after
  preparation and needs a mapping generation.

## Stage prerequisites

- Both stages: the DRAM-controller package provides an EL3 check that DRAM
  decodes linearly to `0x3_4000_0000` and that its protected regions allow
  Non-secure access to the newly mapped range.
- Stage 2: profile word 0 of the version-2 boot record becomes 2, and the GPU
  and NPU bases come only from header constants. Launch evidence shows that
  no boot-loader payload, remote firmware or bus master left running by boot0
  uses `0x8000_0000`–`0x1_0000_0000`. Stage 2 adds one `AT` per class, raises
  `DEVICE_MEMORY_MAX_PAGES` to 262,144 and casts the size product in
  `device_memory_pool_init()` to `size_t`.
- Stage 3: the same launch evidence for `0x1_0000_0000`–`0x3_4000_0000`.

## Changes the primary agent must make

- **Context-switch barrier.** Confirm that the context switch in
  `patches/freertos-smp-asm.patch` executes `DSB SY` before a task resumes on
  another core, or add it.
- **One final table builder (before this package merges).** Set the
  outer-shareable field of `l1[0]` in `a733_cpu_coherence_tables()` and delete
  the later writes in `apps/storage-read-single-core/monitor.c` and
  `apps/dsi-scanout/monitor.patch`.
- **Local preparation.** Call the local preparation from `platform_init()`
  after the identity map is installed and before `vTaskStartScheduler()`.
- **Entry cache state.** A Secure dirty line over any pool at EL3 entry cannot
  be removed by Non-secure EL1 maintenance, and the watermark assumes none
  exists. Specify the launcher's dirty-line experiment in
  `docs/cold-entry.md`; its passing result is a precondition for qualifying
  the pools on the board.
- `platform/a733-smp-cached/memory.c` reads the layout header constants
  directly.
- Stage 2: EL1 level-1 entries 2 and 3 become
  `PA | UXN | PXN | AF | SH=0b11 | AttrIndx=1 | 1` with AP[2:1] = 0b00; EL3
  entries stay invalid. Stage 3 adds entries 4 to 12 the same way.

The ledger's per-transition cost falls when the primary agent meets the
critical-section budget in `docs/performance-architecture.md`; this package
needs no other kernel change.

**Separate package: cacheable translation-table walks.** The cached-table
path walks its tables uncached. Setting IRGN0 = ORGN0 = 0b01 in
`A733_CPU_COHERENCE_TCR` changes `platform/a733-smp-cached/continue.S`,
`platform/a733/cpu_coherence.S`, `boot/a733/native-el1-cache-probe.S` and the
`SUNSTONE_CACHED_SINGLE_CORE` branch of
`apps/storage-read-single-core/platform.patch`. About 15 host checks and tools
repeat the TCR value as a literal; that package first makes them read
`A733_CPU_COHERENCE_TCR` from its header, or from one Python constant shared
by the tools. It cleans the table pages before the first descriptor write and
invalidates the EL1 tables through the Non-secure alias after the last EL1
table write, and adds the same maintenance to the tables `platform_init()`
writes. It specifies the Non-secure dirty-line experiment over the tables and
runs the walk A/B measurement (`CPU_CYCLES`, `DTLB_WALK`, `ITLB_WALK` for a
GPU frame, an NPU inference and a recognizer inference on cores 0 and 6). It
depends on the single-builder change above.

## Board evidence that would confirm the design

- Every board budget above is met on cores 0 and 6, with the CPU-L, CPU-B and
  DSU rates recorded beside each result.
- The recognizer runs at EL0, and each forbidden access raises its expected
  fault class.
- The GPU completes checksummed jobs through 2 MiB device-MMU entries, and the
  NPU through 16 MiB and 1 MiB entries.
- Stage 2: the DRAM check passes for 12,288 MiB, patterns written above
  `0x8000_0000` do not alias into low DRAM, and the GPU and NPU complete
  checksummed jobs inside their classes.
