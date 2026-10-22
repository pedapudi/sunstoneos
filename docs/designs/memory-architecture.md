# Design: device memory classes, boot mapping checks and protection

Status: **design**; no code exists yet. Physical behavior is unqualified until
the board evidence at the end exists. Readers are assumed to know AArch64
stage-1 translation, the FreeRTOS SMP port and non-coherent DMA.

Ten images have a 768 MiB write-back device pool, `dma32`, and a 16 MiB
Normal non-cacheable pool, `uncached`. Boot checks both pools page by page
with 401,408 address translations (`AT`), and nothing serializes the pool
ledger across cores. Stage 1 checks one `AT` per 2 MiB block, makes the
ledger safe on eight cores and moves the after-acquire clean into the pool.
Stage 2 adds a 1 GiB class each for the GPU and the NPU. Stage 3 maps the
DRAM above 4 GiB when a consumer claims it.

## Map and protection

Tasks start at EL0 unless the image selects EL1, and EL0 tasks call kernel
functions directly, so kernel data, stacks and heap stay EL0 read-write.

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
is execute-never at EL0 and EL1. EL1 maps 910 MiB of DRAM in stage 1 and
2,958 MiB in stage 2, in five table pages. Stage 3 adds level-1 entries 4 to
12 as 1 GiB blocks and no table page.

The QEMU fixture `checks/device-memory-capacity.{c,py}` compares all 1,024
EL1 level-1 and level-2 entries with a golden array written from this table,
and an EL0 task takes the expected fault on each forbidden access. Boot
keeps one write-only `AT` per 2 MiB pool block. One `AT` proves a block only
for a block descriptor, so the new host check `checks/memory-pool-blocks.c`
asserts bits[1:0] = 0b01 for each pool block of every table builder.
Preparation is one call on core 0: it invalidates the `uncached` range by
address, initializes the pools, creates their mutexes and publishes READY
with a release store. The EL1 tables do not change afterwards, so READY
serves as the mapping generation. Memory management has no interrupt or
polling path.

## Ledger: one tag word per slot; only acquire locks

Each ledger slot replaces `generation` and `state` with one 64-bit `tag` =
generation << 2 | state, and all slot fields are `__atomic`, so a stale lease
cannot change a reused slot.

**Holder.** One task at a time holds a lease and alone calls release,
retain and `device_memory_set_state()` and maintains its cache lines. The
acquirer may hand the lease to one task through a happens-before edge (task
creation, a queue message or a release-acquire flag) and then stops using
it. The GPU and NPU reservations acquired on core 0 before the scheduler
starts reach their workers by task creation. Other tasks call only
`device_memory_describe()`.

- **Acquire** takes the class lock, finds a FREE slot with acquire loads,
  issues `atomic_thread_fence(release)`, stores the fields relaxed and
  stores the new tag with release ordering.
- **Release:** one compare-exchange from (generation, CPU) to (generation,
  FREE), release ordering.
- **Retain:** one acquire load of the tag. It succeeds if the tag already
  equals (generation, RETAINED) and refuses if the generation differs or the
  state is FREE. Otherwise it makes one compare-exchange from the loaded
  value; a failure means the lease is stale, so there is no retry.
- **`device_memory_set_state(pool, lease, to)`:** one compare-exchange
  between CPU and DEVICE.
- **Describe:** an acquire load of the tag, relaxed loads of the fields,
  `atomic_thread_fence(acquire)` and a relaxed reload of the tag. It
  succeeds only when both tag loads match the lease. By fence-to-fence
  synchronization with the acquirer's release fence, a describe that reads a
  new owner's field also reads a newer tag, and fails.

Release and retain are the bounded-failure fallbacks and must never be
refused. A mutex with a timeout, or a suspended scheduler, would refuse them
and leave an extent with no recorded owner state, so only acquire, which can
refuse safely, locks. Removing `device_memory_lookup()` and the plain
`state` field turns the 60 remaining uses in 27 consumer files and 15 host
checks into compile errors, which their owners fix in the same merge.

**Lock hook.** Native images link `platform/a733/memory_lock.c`; host checks
link `runtime/device_memory_pool_host.c`, whose hooks return 1. The platform
initializer sets `write_back`, which selects the after-acquire clean, and
`lock_slot`, the pool's static mutex. A pool built with
`device_memory_pool_init()` inside a lease has neither, because the outer
acquire cleaned its backing; the only one, the USB controller's, acquires
its full backing once and never releases it. The native lock takes no mutex
before the scheduler starts, because secondary cores wait in
`a733_cached_el1()` until `vTaskStartScheduler()` releases them. It takes
none for a pool without a mutex. Otherwise it refuses unless
`a733_task_context_any_no_el_check()` passes, which excludes interrupt
handlers, critical sections and a suspended scheduler, then waits at most
two ticks. A refused acquire holds nothing.

That check reads `TPIDR_EL1`, which is undefined at EL0, so an EL0 acquire
of any class stops the system through `a733_cached_cold_fault` before any
ledger change. Device classes have no EL0 mapping, so acquire is an EL1
operation. A local pool getter initializes only on core 0 before the
scheduler starts and otherwise returns NULL, so an image that misses the
early call refuses instead of racing through mutex creation.

## Cache maintenance and placement

A previous holder, or boot code before EL3 entry, can leave dirty lines in a
new extent, and their eviction during a device write would overwrite device
output in DRAM. Acquire of a write-back class returns a clean extent in one
of two forms:

- **Plain:** `DC CVAC` over every line after the lock is released, through
  `aarch64_device_cache()`. Its leading `DSB SY` orders the clean after the
  previous holder's stores, which this core observed only through the tag.
- **Zeroed** (a `zero` argument of `device_memory_acquire_aligned()`):
  `DC ZVA` over the extent, then the plain clean; stores replace `DC ZVA`
  when `DCZID_EL0` prohibits it. `DC ZVA` writes each line coherently, so
  it overwrites any stale line, and the extent is ready for
  `set_state(DEVICE)`. It replaces the callers in
  `platform/a733/gpu_compute_reserved.c`,
  `platform/a733/gpu_graphics_memory.c` and `apps/uart/dma.c` that mark an
  extent DEVICE and then `memset` it.

If the clean is refused, acquire releases the slot and refuses. Preparation
invalidated `uncached`, so its acquire needs no clean; its owners issue
`DMB OSHST` before a doorbell and `DMB OSHLD` after observing completion.
Per transition, owners maintain the (offset, length) write sets declared at
admission, checked once with `device_memory_describe()` and
`a733_extent_within()`. Device to CPU uses `DC CIVAC`: the CPU writes none
of a DEVICE extent, so this costs what `DC IVAC` costs, and it cannot discard
data if the holder rule is broken. Maintenance to the point of coherency
reaches every core's cache, so task migration adds no step. The CPU side of
GPU and NPU work, including session open, runs on cores 6 and 7 once those
are admitted.

| Transition | Rate | `DC` operations | Barriers |
| --- | --- | --- | --- |
| Acquire, plain or zeroed | session open, model load | one per line; zeroed adds one `DC ZVA` per block | 2 `DSB SY` |
| CPU to device | GPU frame at 60 Hz, NPU inference, DMA period up to 200 Hz | one per line written | 2 `DSB SY` |
| Device to device | NPU layer, GPU frame to scanout | 0 | 0 |
| Device to CPU | as CPU to device | one per line of the declared write set | 2 `DSB SY` |

## Reused and removed

- Reused: the first-fit allocator, `device_memory_acquire_aligned()`,
  `a733_cpu_coherence_tables()`, the version-2 boot record,
  `a733_mapping_check_stride()`, `aarch64_device_cache()`, `a733_range.h`,
  `a733_task_context.h` and the QEMU fixture.
- Removed from `device_memory_layout.c`: the batched page-scan state machine
  (cursor, `nc`, `SCANNING`, `COMMITTING`, `busy`,
  `A733_DEVICE_MEMORY_BATCH_PAGES`, `a733_device_memory_commit_allowed()`,
  `same_layout()`), the second `prepared_profile()` call and
  `a733_device_memory_tables_admitted()`. The two `*_commit_verified()`
  functions become `a733_memory_reservations_publish()` and
  `a733_memory_uncached_publish()`.
- Removed from the pool and its callers: `device_memory_lookup()`, the plain
  `state` field and `memset` over a fresh extent.
- Left to the GPU and NPU packages: their per-page walks become one
  `device_memory_describe()` and `a733_extent_within()` check per lease.

## Performance budgets

Instruction counts come from the QEMU fixture (`-icount shift=0`, PMU
instructions retired around the call) or host replay counters; cycles come
from PMU `CPU_CYCLES` on the board. Provisional targets become fixed after
the first measurement.

| Metric | Baseline | Target | Measurement |
| --- | --- | --- | --- |
| Extended pool check | 401,408 `AT` in 3,136 calls (counted); about 17 M instructions (estimated) | 392 `AT` in one call; at most 25,000 instructions; provisional 200 cycles per `AT`, 78,400 cycles | QEMU; board core 0 |
| Local pool check | up to 2,048 `AT` (counted) | at most 3 `AT` | callback count, `checks/memory-reservations.c` |
| DRAM mapped at EL1; table memory | 910 MiB; 20 KiB | 910, 2,958, 12,288 MiB by stage; 20 KiB | QEMU golden comparison |
| Boot maintenance | none | 262,144 `DC CIVAC` on `uncached`; 128 `DC IVAC` on EL1 tables | QEMU count |
| Acquire | no lock | lock and unlock at most 600 instructions; held at most 15,000 instructions, at most 1,056 slot comparisons | QEMU; host replay |
| Release, retain, `set_state` | plain stores | one acquire load, one compare-exchange | QEMU |
| Plain clean | done by callers | provisional 4 cycles per absent line; 1 GiB within 40 ms on core 6 | host replay counts lines; board, cores 0 and 6 |
| Zeroed acquire | caller `memset` (counted in three files) | at most 1.25 times a store loop over the same bytes; caller `memset` 0 | board, cores 0 and 6; GPU and NPU host models |
| GPU session open, NPU model load (stage 2) | none | at most 100 ms | board, core 6 |
| `DC` per device-to-CPU transition | full READ and WRITE buffers (NPU, counted) | declared write set; no full-extent maintenance per frame or inference | GPU and NPU host models |
| `DSB SY` per transition | 2 per range (counted) | 2 per transition | GPU and NPU host models |
| Translation-table walks | uncached, not measured | cacheable no slower in any A/B pair | board A/B |

Acquire runs only at admission, a few times per session, and holds the lock
for less time than the clean of one 1 MiB extent takes, so the 32-slot scan
needs no index. No mechanism that skips the clean is planned: it would have
to prove that a block holds no dirty line, which needs the cache-state
evidence below, and would help only the first acquisition of each block. The
100 ms session-open target rests on the zeroed form and on cores 6 and 7.

`TCR_EL1` is `0x2_0080_3019`: IRGN0 = ORGN0 = 0, so every TLB-miss walk
reads descriptors from DRAM. The walk A/B run records `CPU_CYCLES`,
`DTLB_WALK` and `ITLB_WALK` with both settings for a GPU frame, an NPU
inference and recognizer inference, each on core 0 and core 6.

## Fixtures

- Host replay of the GPU and display image's admission order
  (`checks/device-memory-capacity.c`): comparisons per acquire, the largest
  free extent before each later allocation, free extents unchanged after
  every session opens and closes once, and no `write_back` or mutex on a
  pool built inside a lease.
- A ThreadSanitizer smoke test races describe on a stale lease against
  release and re-acquire; it detects only non-atomic access. The ordering
  rests on the argument above, and a disassembly check confirms that acquire
  and describe contain `LDAR`, `STLR`, `DMB ISH` and `DMB ISHLD`.
- QEMU native image: acquire refuses with the scheduler suspended and when a
  class mutex is held past two ticks, while other holders' release and
  retain succeed; an EL0 acquire produces the cold-fault record.

## Left out

- The contiguous hint on `dma32`, until the walk measurement.
- Execute-never read-only data and guard pages, which need level-3 tables;
  `SCTLR_EL1.WXN`, which the golden comparison makes unnecessary.
- Read-only mapping of admitted NPU models (performance-architecture
  section 1.4), which changes the tables after sealing and needs a mapping
  generation.

## Stage 2 prerequisites

Profile word 0 of the version-2 boot record becomes 2, and the GPU and NPU
bases come only from header constants. The DRAM-controller package provides
an EL3 check that DRAM decodes linearly to `0x3_4000_0000`. Launch evidence
shows that no boot-loader payload, remote firmware or bus master left
running by boot0 uses `0x8000_0000`–`0x1_0000_0000`. Stage 2 adds one `AT`
per class, raises `DEVICE_MEMORY_MAX_PAGES` to 262,144 and casts the size
product in `device_memory_pool_init()` to `size_t`.

## Changes the primary agent must make

- **Cacheable walks.** Set IRGN0 = ORGN0 = 0b01 in `A733_CPU_COHERENCE_TCR`
  and use that constant in place of the literals in
  `platform/a733-smp-cached/continue.S`, `platform/a733/cpu_coherence.S`,
  `platform/a733/cpu_continue.c` and `boot/a733/native-el1-cache-probe.S`;
  `prepared_profile()` in `device_memory_layout.c` already compares it.
  In `a733_cached_memory_prepare()`, after the last EL1 descriptor write and
  before the TLB invalidate and enable, issue `DC IVAC` on the two EL1 table
  pages (128 operations), then `DSB SY`. EL3 writes the descriptors with its
  data cache off, so a clean could write a stale pre-entry line over them;
  `platform/a733-smp-supervised/publication.S` uses `DC IVAC` for the same
  reason. EL3 walks stay uncached. A later writer writes through a Normal-NC
  alias, then issues `DC IVAC`, `DSB` and the TLB invalidate.
- **Leading barrier.** A variant of `aarch64_device_cache()` without the
  leading `DSB SY`, for cleans of lines the calling task wrote. It is valid
  only if the context switch in `patches/freertos-smp-asm.patch` executes a
  `DSB` before a task resumes on another core. The after-acquire clean keeps
  the barrier.
- **Multi-range maintenance:** one call over several ranges with one leading
  and one trailing barrier, for GPU frame and NPU transitions.
- **One final table builder:** set the outer-shareable field of `l1[0]` in
  `a733_cpu_coherence_tables()` and delete the later writes in
  `apps/storage-read-single-core/monitor.c` and
  `apps/dsi-scanout/monitor.patch`, so the golden comparison covers every
  image's final tables.
- `platform/a733-smp-cached/memory.c` reads the layout header constants
  directly. `platform/a733/platform.c` moves the level-2 loop of
  `platform_init()` into a function the block check can call, and calls the
  local pool getters on core 0 before `vTaskStartScheduler()`.
- Stage 2: EL1 level-1 entries 2 and 3 become
  `PA | UXN | PXN | AF | SH=0b11 | AttrIndx=1 | 1` with AP[2:1] = 0b00; EL3
  entries stay invalid. Stage 3 adds entries 4 to 12 the same way.

## Board evidence that would confirm the design

- Preparation and every maintenance form meet their budgets on cores 0 and 6,
  and cacheable walks are no slower than uncached walks in every A/B pair.
- The recognizer runs at EL0, and each forbidden access raises its expected
  fault class.
- Cache state at EL3 entry, on which the image's own code and data depend.
  These launcher checks cover the launcher's handover path; boot0's set/way
  clean has only the instruction model in `docs/cold-entry.md`. Dirty lines:
  the launcher writes a pattern through the cache, and EL3 reads it from DRAM
  with caches off. Valid lines: the launcher reads a line cacheably and
  changes DRAM through a non-cacheable alias, and the first cacheable read
  after EL3 entry returns the new value. The pool and table maintenance above
  does not depend on this result.
- Stage 2: the DRAM check passes for 12,288 MiB, patterns written above
  `0x8000_0000` do not alias into low DRAM, and the GPU and NPU complete
  checksummed jobs inside their classes.
