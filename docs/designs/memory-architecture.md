# Design: device memory classes, boot mapping checks and protection

Status: **design**; no code exists yet. Physical behavior is unqualified until
the board evidence at the end exists. Readers are assumed to know AArch64
stage-1 translation, the FreeRTOS SMP port and non-coherent DMA.

Ten images have a 768 MiB write-back device pool, `dma32`, and a 16 MiB
Normal non-cacheable pool, `uncached`. Boot checks both pools page by page
with 401,408 address translations (`AT`), acquire scans up to 1,056 slot
pairs, and nothing serializes the pool ledger across cores. Stage 1 checks one
`AT` per 2 MiB block, serializes the ledger with one spinlock per pool, finds
free space through a sorted index, aligns large extents to 2 MiB and moves the
after-acquire clean into the pool. Stage 2 adds a 1 GiB class each for the GPU
and the NPU. Stage 3 maps the DRAM above 4 GiB when a consumer claims it.

## Map and protection

Tasks start at EL0 unless the image selects EL1, and EL0 tasks call kernel
functions directly, so kernel data, stacks and heap stay EL0 read-write. The
device-memory ledger is the exception: it runs only at EL1 (see Ledger).

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

The QEMU fixture `checks/device-memory-capacity.{c,py}` compares all 1,024
EL1 level-1 and level-2 entries with a golden array from this table, for the
extended-layout image and for a local-layout image built by the same fixture.
An EL0 task faults on each forbidden access. Boot keeps one write-only `AT`
per 2 MiB pool block; one `AT` proves a whole block only for a block
descriptor, and the golden comparison fixes each pool entry's descriptor type.
The comparison sees final tables only after the single-builder change below
deletes the two monitor writers that rewrite level-1 entry 0 after
preparation, so that change must land before this package merges.

Each layout has one preparation call on core 0 before the scheduler starts.
It cleans and invalidates `uncached` by address, initializes the pools and
publishes READY with a release store. `platform_init()` calls the local
preparation (a primary-agent change below). Both getters acquire-load READY
and return the pool or NULL, so no pool is ever initialized concurrently. The
EL1 tables do not change after preparation, so READY serves as the mapping
generation. Memory management has no interrupt or polling path: every
operation is a synchronous call from boot code or an EL1 task.

## Allocation: class alignment and a ledger behind one spinlock per pool

**Alignment.** Acquire raises the caller's alignment to the class minimum,
so a caller cannot under-align. The minimum is 2 MiB for `gpu` and `npu` and
for any `dma32` request of at least 2 MiB, 64 KiB for smaller `dma32`
requests, and 4 KiB for `uncached`, which holds control blocks of a few pages.
All three class bases are 2 MiB aligned. First fit with these alignments lets
the GPU and NPU device MMUs map every large extent with 2 MiB entries in place
of 262,144 4 KiB entries per GiB; the GPU and NPU packages adopt them.

**Index.** A 32-bit free mask selects a free slot with one count of trailing
zeros. An index array of at most 32 occupied slots, sorted by first page,
replaces the 33 × 32 overlap scan. Acquire walks the gaps between neighbours
once, rounding each gap start up to the alignment, and takes the first gap
that fits: at most 33 gaps. This is the same lowest-address fit that the
current first fit finds, so placement does not change. Acquire inserts the
slot by shifting at most 31 entries, and release removes it the same way.

**Ledger.** Acquire, describe, release, retain and `device_memory_set_state()`
each run inside the pool's ticket lock, and the slot fields, including
`generation` and `state`, are plain fields read and written only under it.
The lock is a pair of 32-bit counters in its own member of the pool
structure. A caller takes a ticket with one atomic add, then waits with
`SEVL` followed by a loop of `WFE` and an exclusive load-acquire of the
serving counter; the holder releases with a store-release (`STLR`), which
wakes the waiters. Each core masks interrupts for the wait and the hold, so
an interrupt cannot stall a holder while up to seven cores spin. No shared
ticket-lock helper exists, and the counter-clock lock spins with `YIELD`, so
this lock is a static function in `runtime/device_memory_pool.c`; the host
build uses a plain C11 spin.

The mask is the port's `portSET_INTERRUPT_MASK_FROM_ISR()` and
`portCLEAR_INTERRUPT_MASK_FROM_ISR()`. In the `ARM_CR82` port of the extended
SMP image these access `ICC_PMR_EL1` and `DAIF` directly, without an `SVC`,
and both instructions trap as undefined at EL0. Every locked operation,
including pool initialization, therefore runs only at EL1, and a lease may be
handed only to an EL1 task. `aarch64_device_cache()` already reads
`CurrentEL` and is EL1-only, so no consumer that maintains its lines changes
level. The mask raises the priority mask to
`configMAX_API_CALL_INTERRUPT_PRIORITY`, and a handler above it could preempt
a holder and deadlock on the lock. No handler calls the ledger, and the API
comment states that a future handler caller must run at or below that
priority, the rule for FreeRTOS `FromISR` calls.

The locked section touches only the pool structure and makes no call that can
block or fault, so for an EL1 caller release and retain, the bounded-failure
fallbacks, always succeed. Acquire holds the lock for at most about 600
instructions, release for 250 and the other operations for 100. A core
waiting behind seven acquires therefore masks interrupts for at most about
5,000 instructions.

`device_memory_pool_init()` writes every member except the lock and refuses a
pool that is already initialized, so a pool built inside a lease cannot reset
a lock word that another core uses. Describe's pool argument stops being
`const`.

**Holder.** One task at a time holds a lease, calls its transitions and
maintains its cache lines; a hand-off to another task needs a happens-before
edge such as task creation or a queue message. Removing
`device_memory_lookup()` and renaming the slot fields turns the 60 remaining
uses in 27 consumer files and 15 host checks into compile errors, which their
owners fix in the same merge.

## Cache maintenance and placement

A previous holder, or boot code before EL3 entry, can leave dirty lines in a
new extent, and their eviction during a device write would overwrite device
output in DRAM. Every pool records its memory type. `device_memory_pool_init()`
creates a write-back pool, including a pool built inside a lease, and the
`uncached` preparation marks its pool Normal-NC. After the lock is released,
acquire returns the extent in one of two forms:

- **Plain:** on a write-back pool, `DC CVAC` over every line, then one
  `DSB SY`. The clean needs no leading barrier: a `DC CVAC` by address is
  ordered after this core's earlier stores to the same line, and the lock
  release and acquire order it after the previous holder's stores on another
  core. On a Normal-NC pool, nothing.
- **Zeroed** (a `zero` argument of `device_memory_acquire_aligned()`): one
  read of `DCZID_EL0` on the executing core, before the lock, refuses unless
  zeroing is permitted with a 64-byte block. Then `DC ZVA` over the extent,
  which writes each line coherently and so overwrites any stale line. A
  write-back pool then runs the plain clean; a Normal-NC pool runs none. The
  extent is ready for `set_state(DEVICE)`. The zeroed form replaces the
  `memset` calls over fresh extents, on both pools, in
  `platform/a733/gpu_compute_reserved.c`,
  `platform/a733/gpu_graphics_memory.c` and `apps/uart/dma.c`. A single-core
  A/B run on core 6 compares two passes with a fused loop (`DC ZVA` then
  `DC CVAC` on the same line).

If the clean is refused, acquire releases the slot and refuses. A pool built
inside a lease cleans each range it hands out, so its first round of acquires
repeats the clean of the outer acquire; the session-open budget includes both
cleans. `uncached` owners issue `DMB OSHST` before a doorbell and `DMB OSHLD`
after completion. Transitions run at session open, per 60 Hz GPU frame, per
NPU inference and per DMA period of up to 200 Hz. In each, owners maintain
only the (offset, length) write sets declared at admission, checked once with
`device_memory_describe()` and `a733_extent_within()`, and device-to-device
transitions need none. Device to CPU uses `DC CIVAC` with a leading `DSB SY`
after the completion read: the CPU writes none of a DEVICE extent, so this
costs what `DC IVAC` costs and cannot discard data if the holder rule is
broken. The CPU side of GPU and NPU work, including session open, runs on
cores 6 and 7 once those are admitted.

Every form runs with interrupts unmasked. A `DSB` completes only the
maintenance its own core issued, so if a task migrates between its `DC` loop
and the trailing `DSB SY`, correctness rests on a `DSB` in the context-switch
path of `patches/freertos-smp-asm.patch`. Confirming that barrier is a
precondition for every maintenance form in this design.

## Reused and removed

- Reused: the first-fit placement rule, `device_memory_acquire_aligned()`,
  `a733_cpu_coherence_tables()`, the version-2 boot record,
  `a733_mapping_check_stride()`, `aarch64_device_cache()` for invalidation,
  the port's interrupt-mask pair and the QEMU fixture with its golden array.
- Removed: the batched page-scan state machine in `device_memory_layout.c`
  with `a733_device_memory_commit_allowed()`, `same_layout()` and
  `a733_device_memory_tables_admitted()`; the 33 × 32 overlap scan;
  `device_memory_lookup()`, public slot fields and `memset` over a fresh
  extent; the lazy initialization in the local `uncached` getter. The GPU and
  NPU per-page walks become one `describe` and `a733_extent_within()` check
  per lease.

## Performance budgets

Instruction counts come from the QEMU fixture (`-icount shift=0`, PMU
instructions retired around the call) or host replay counters. Cycles come
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
| Uncached preparation, core 0 | none | 262,144 `DC CIVAC`; provisional 20 A55 cycles per line, 5.3 M cycles (10.6 ms at 494 MHz); local layout 32,768 lines | QEMU count; board |
| Gap checks per acquire | 1,056 slot comparisons (counted) | at most 33; placement identical to the current first fit | host replay |
| Lock hold and masked interval | no lock | hold: acquire 600, release 250, others 100 instructions; masked per core at most 5,000 instructions, provisional 10,000 A55 cycles (20 µs at 494 MHz, 0.4% of a 5 ms DMA audio period) | QEMU; host replay; board core 0 |
| Extent alignment | caller's, 4 or 16 KiB | every extent of at least 2 MiB 2 MiB aligned; smaller `dma32` 64 KiB; largest free extent after the admission order no smaller than with 4 KiB alignment | host replay |
| Plain clean | done by callers | provisional 4 cycles per absent line on A55 and A76; 1 GiB is 67 M cycles, 136 ms on core 0 at 494 MHz | host replay counts lines; board, cores 0 and 6 |
| Zeroed acquire | caller `memset` (counted in three files) | at most 1.25 times a store loop over the same bytes on the same core; caller `memset` 0; fused and two-pass loops both recorded | board, cores 0 and 6; GPU and NPU host models |
| GPU session open, NPU model load (stage 2) | none | at most 160 M A76 cycles, including an outer and an inner clean of 1 GiB; 100 ms needs CPU-B at 1.6 GHz or more | board, core 6 |
| `DC` per device-to-CPU transition | full READ and WRITE buffers (NPU, counted) | declared write set; no full-extent maintenance per frame or inference | GPU and NPU host models |
| `DSB SY` per transition | 2 per range (counted) | 1 per CPU-to-device transition, 2 per device-to-CPU transition | GPU and NPU host models |

## Fixtures

- Host replay of the GPU and display image's admission order
  (`checks/device-memory-capacity.c`): gap checks per acquire, placement
  against the current first fit, alignment of every extent, the largest free
  extent before each later allocation, free extents unchanged after every
  session opens and closes once, and the lock member unchanged across
  `device_memory_pool_init()`.
- `checks/memory-pool-callers.py` lists every caller of acquire, zeroed
  acquire, describe, release, retain, `device_memory_set_state()` and pool
  initialization, and asserts that each runs at boot or in an EL1 task, and
  that every task a lease is handed to is an EL1 task.
- QEMU native image: every locked operation from an EL1 task; release and
  retain completing while another core holds the lock through a worst-case
  acquire; an EL0 task calling release with a valid lease takes an
  undefined-instruction exception at the first `ICC_PMR_EL1` access.

## Left out

- Interrupt-handler callers of the ledger: no consumer needs one.
- Splitting large zeroing or cleaning across cores 6 and 7: one core is
  expected to meet the session-open target; a missed measurement reopens it.
- A shared ticket-lock helper: the ledger is its only consumer with `WFE`.
- Cacheable translation-table walks, the contiguous hint on `dma32` and the
  walk A/B run belong to the separate package described below. These pools
  do not depend on it.
- Execute-never read-only data and guard pages: need level-3 tables.
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
- **Local preparation.** Call the local `uncached` preparation from
  `platform_init()` after the identity map is installed and before
  `vTaskStartScheduler()`.
- **Clean without a leading barrier.** Give `aarch64_device_cache()` a clean
  form with no leading `DSB SY`, as Linux's `dcache_clean_poc` has; keep the
  leading barrier for invalidate and clean-and-invalidate. The multi-range
  call for GPU frame and NPU transitions follows the same rule, with one
  trailing barrier per call.
- **Entry cache state.** A Secure dirty line over the `dma32` or `uncached`
  pool at EL3 entry cannot be removed by Non-secure EL1 maintenance. Specify
  the launcher's dirty-line experiment in `docs/cold-entry.md`; its passing
  result is a precondition for qualifying the pools on the board.
- `platform/a733-smp-cached/memory.c` reads the layout header constants
  directly.
- Stage 2: EL1 level-1 entries 2 and 3 become
  `PA | UXN | PXN | AF | SH=0b11 | AttrIndx=1 | 1` with AP[2:1] = 0b00; EL3
  entries stay invalid. Stage 3 adds entries 4 to 12 the same way.

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
- The GPU and NPU complete checksummed jobs through 2 MiB device-MMU entries.
- Stage 2: the DRAM check passes for 12,288 MiB, patterns written above
  `0x8000_0000` do not alias into low DRAM, and the GPU and NPU complete
  checksummed jobs inside their classes.
