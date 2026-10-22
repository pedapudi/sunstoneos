# Design: device memory classes, boot mapping checks and protection

Status: **design**; no code exists yet. Physical behavior is unqualified until
the board evidence at the end exists. Readers are assumed to know AArch64
stage-1 translation, the FreeRTOS SMP port and non-coherent DMA.

Ten images have a 768 MiB write-back device pool, `dma32`, and a 16 MiB
Normal non-cacheable pool, `uncached`. Boot checks both pools page by page
with 401,408 address translations (`AT`), and nothing serializes the pool
ledger across cores. Stage 1 checks one `AT` per 2 MiB block, serializes the
ledger with one spinlock per pool, aligns large extents to 2 MiB and moves the
after-acquire clean into the pool. Stage 2 adds a 1 GiB class each for the GPU
and the NPU. Stage 3 maps the DRAM above 4 GiB when a consumer claims it.

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
is execute-never at EL0 and EL1. The two invalid holes inside DRAM cover
114 MiB, so EL1 maps 910 MiB of DRAM in stage 1, 2,958 MiB in stage 2 and
12,174 MiB in stage 3, in five table pages. Stage 3 adds level-1 entries 4 to
12 as 1 GiB blocks and no table page.

The QEMU fixture `checks/device-memory-capacity.{c,py}` compares all 1,024
EL1 level-1 and level-2 entries with a golden array from this table, and an
EL0 task faults on each forbidden access. Boot keeps one write-only `AT` per
2 MiB pool block. One `AT` proves a block only for a
block descriptor; the golden comparison fixes each pool entry's descriptor
type for the extended layout. The host check `checks/memory-pool-blocks.c`
asserts bits[1:0] = 0b01 only for the builders the comparison does not cover:
the level-2 loop of `platform/a733/platform.c` for the local layout, and the
two monitor writers until the single-builder change below deletes them.

Preparation of the extended layout is one call on core 0. It asserts
`DCZID_EL0` = 4 (zeroing permitted, 64-byte block) and otherwise stops the
system through the image's fatal path, cleans and invalidates `uncached` by
address, initializes the pools and publishes READY with a release store. The
local `uncached` getter cleans and invalidates its 2 MiB the same way, then
initializes the pool under that pool's lock and publishes a flag with a
release store. The maintenance stays outside the lock because repeating it
is harmless, so concurrent first calls cost time but cannot race. The EL1 tables do not change
after preparation, so READY serves as the mapping generation. Memory
management has no interrupt or polling path.

## Allocation: class alignment and a ledger behind one spinlock per pool

**Alignment.** Acquire raises the caller's alignment to the class minimum,
so a caller cannot under-align. The minimum is 2 MiB for `gpu` and `npu` and
for any `dma32` request of at least 2 MiB, 64 KiB for smaller `dma32`
requests, and 4 KiB for `uncached`, which holds control blocks of a few pages.
All three class bases are 2 MiB aligned. First fit with these alignments lets
the GPU and NPU device MMUs map every large extent with 2 MiB entries in place
of 262,144 4 KiB entries per GiB; the GPU and NPU packages adopt them.

**Ledger.** Each slot replaces `generation` and `state` with one 64-bit
`tag` = generation << 2 | state, and all slot fields are `__atomic`.
Acquire, release, retain and `device_memory_set_state()` each run inside the
pool's lock: a ticket pair in the pool structure, zero when the structure is
zero, taken with interrupts masked through the port's save-and-restore pair
(`portSET_INTERRUPT_MASK_FROM_ISR()` and `portCLEAR_INTERRUPT_MASK_FROM_ISR()`).
Inside, a transition checks the tag, stores the fields relaxed after
`atomic_thread_fence(release)` and stores the new tag with release ordering.
The section touches only the pool structure and makes no call that can block
or fault, so the lock never refuses: release and retain, the bounded-failure
fallbacks, always succeed. It needs no creation step, so it works before the
scheduler starts, in an interrupt handler and for a pool built inside a lease.

Acquire holds the lock for the slot scan only, at most 1,056 slot
comparisons or about 15,000 instructions, and runs only at admission, so the
scan needs no index. A core waiting behind seven holders keeps interrupts
masked for at most 120,000 instructions. The 5 ms DMA audio period bounds
this: the 200,000-cycle budget below is 4% of it at 1 GHz.

**Describe** stays lock-free: an acquire load of the tag, relaxed loads of
the fields, `atomic_thread_fence(acquire)` and a relaxed reload of the tag. It
succeeds only when both tag loads match the lease, and by fence-to-fence
synchronization a describe that reads a new owner's field also reads the new
tag, and fails.

**Holder.** One task at a time holds a lease, calls its transitions and
maintains its cache lines; a hand-off to another task needs a happens-before
edge such as task creation or a queue message. Removing
`device_memory_lookup()` and the plain `state` field turns the 60 remaining uses in 27 consumer files and 15
host checks into compile errors, which their owners fix in the same merge.

## Cache maintenance and placement

A previous holder, or boot code before EL3 entry, can leave dirty lines in a
new extent, and their eviction during a device write would overwrite device
output in DRAM. Every pool records its memory type. `device_memory_pool_init()`
creates a write-back pool, including a pool built inside a lease, and the
`uncached` initializers mark theirs Normal-NC. After the lock is released,
acquire returns the extent in one of two forms:

- **Plain:** on a write-back pool, `DC CVAC` over every line through
  `aarch64_device_cache()`, whose leading `DSB SY` orders the clean after the
  previous holder's stores. On a Normal-NC pool, nothing.
- **Zeroed** (a `zero` argument of `device_memory_acquire_aligned()`): `DC
  ZVA` over the extent, which writes each line coherently and so overwrites
  any stale line. A write-back pool then runs the plain clean; a Normal-NC
  pool runs none. The extent is ready for `set_state(DEVICE)`. The zeroed form replaces the
  `memset` calls over fresh extents, on both pools, in
  `platform/a733/gpu_compute_reserved.c`, `platform/a733/gpu_graphics_memory.c`
  and `apps/uart/dma.c`. The A/B run on core 6 compares two passes with a
  fused loop (`DC ZVA` then `DC CVAC` on the same line). Extents of 64 MiB or
  more are split across cores 6 and 7 only if one core misses the 100 ms
  session-open target.

If the clean is refused, acquire releases the slot and refuses. `uncached`
owners issue `DMB OSHST` before a doorbell and `DMB OSHLD` after completion.
Transitions run at session open, per 60 Hz GPU frame, per NPU inference and
per DMA period of up to 200 Hz. In each, owners maintain only the (offset,
length) write sets declared at admission, checked once with
`device_memory_describe()` and `a733_extent_within()`, and device-to-device
transitions need none. Device to CPU uses `DC CIVAC`: the CPU writes none of
a DEVICE extent, so this costs what `DC IVAC` costs and cannot discard data
if the holder rule is broken. The CPU side of GPU and NPU work, including
session open, runs on cores 6 and 7 once those are admitted.

Every form runs with interrupts unmasked. A `DSB` completes only the
maintenance its own core issued, so if a task migrates between its `DC` loop
and the trailing `DSB SY`, correctness rests on a `DSB` in the context-switch
path of `patches/freertos-smp-asm.patch`. Confirming that barrier is a
precondition for every maintenance form in this design.

## Reused and removed

- Reused: the first-fit allocator, `device_memory_acquire_aligned()`,
  `a733_cpu_coherence_tables()`, the version-2 boot record,
  `a733_mapping_check_stride()`, `aarch64_device_cache()`, the port's
  interrupt-mask pair and the QEMU fixture.
- Removed: the batched page-scan state machine in `device_memory_layout.c`
  with `a733_device_memory_commit_allowed()`, `same_layout()` and
  `a733_device_memory_tables_admitted()`; `device_memory_lookup()`, the plain
  `state` field and `memset` over a fresh extent. The GPU and NPU per-page
  walks become one `describe` and `a733_extent_within()` check per lease.

## Performance budgets

Instruction counts come from the QEMU fixture (`-icount shift=0`, PMU
instructions retired around the call) or host replay counters; cycles come
from PMU `CPU_CYCLES` on the board. Provisional targets become fixed after the
first measurement.

| Metric | Baseline | Target | Measurement |
| --- | --- | --- | --- |
| Extended pool check | 401,408 `AT` in 3,136 calls (counted); about 17 M instructions (estimated) | 392 `AT` in one call; at most 25,000 instructions; provisional 78,400 cycles | QEMU; board core 0 |
| Local pool check | up to 2,048 `AT` (counted) | at most 3 `AT` | callback count, `checks/memory-reservations.c` |
| DRAM mapped at EL1; table memory | 910 MiB; 20 KiB | 910, 2,958, 12,174 MiB by stage; 20 KiB | QEMU golden comparison |
| Uncached preparation, core 0 | none | 262,144 `DC CIVAC`; provisional 20 cycles per line, 5.3 M cycles; local layout 32,768 lines | QEMU count; board `CPU_CYCLES` |
| Table maintenance before cacheable walks | none | 320 Secure `DC CIVAC` and 128 Non-secure `DC IVAC` | QEMU count |
| Ledger lock | no lock | acquire holds at most 15,000 instructions and 1,056 comparisons; other transitions at most 100; masked interval per core at most 120,000 instructions, provisional 200,000 cycles | QEMU; host replay; board |
| Extent alignment | caller's, 4 or 16 KiB | every extent of at least 2 MiB 2 MiB aligned; smaller `dma32` 64 KiB; largest free extent after the admission order no smaller than with 4 KiB alignment | host replay |
| Plain clean | done by callers | provisional 4 cycles per absent line; 1 GiB within 40 ms on core 6 | host replay counts lines; board, cores 0 and 6 |
| Zeroed acquire | caller `memset` (counted in three files) | at most 1.25 times a store loop over the same bytes; caller `memset` 0; fused and two-pass loops both recorded | board, cores 0 and 6; GPU and NPU host models |
| GPU session open, NPU model load (stage 2) | none | at most 100 ms | board, core 6 |
| `DC` per device-to-CPU transition | full READ and WRITE buffers (NPU, counted) | declared write set; no full-extent maintenance per frame or inference | GPU and NPU host models |
| `DSB SY` per transition | 2 per range (counted) | 2 per transition | GPU and NPU host models |
| Translation-table walks | uncached on the cached SMP path, not measured | cacheable no slower in any A/B pair | board A/B |

The walk A/B run records `CPU_CYCLES`, `DTLB_WALK` and `ITLB_WALK` for a GPU
frame, an NPU inference and a recognizer inference, each on core 0 and core 6.

## Fixtures

- Host replay of the GPU and display image's admission order
  (`checks/device-memory-capacity.c`): comparisons per acquire, alignment of
  every extent, the largest free extent before each later allocation, and
  free extents unchanged after every session opens and closes once.
- `checks/memory-pool-callers.py` lists every caller of a device-class
  acquire or of the zeroed form and asserts that it runs in an EL1 task or in
  an image without EL0 tasks. Device classes have no EL0 mapping, and `DC ZVA`
  at EL0 traps unless `SCTLR_EL1.DZE` is set.
- QEMU native image: the mask pair from an EL0 task, an EL1 task and an
  interrupt handler, and release and retain completing while another core
  holds the lock for a full scan.

## Left out

- The contiguous hint on `dma32`: waits for the walk measurement.
- Execute-never read-only data and guard pages: need level-3 tables.
- `SCTLR_EL1.WXN`: the golden comparison already excludes writable executable entries.
- Read-only mapping of admitted NPU models: changes the tables after sealing and needs a mapping generation.

## Stage prerequisites

- Both stages: the DRAM-controller package provides an EL3 check that DRAM
  decodes linearly to `0x3_4000_0000` and that its protected regions allow
  Non-secure access to the whole newly mapped range.
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
- **Cacheable walks on the cached-table path.** Set IRGN0 = ORGN0 = 0b01 in
  `A733_CPU_COHERENCE_TCR` (`0x2_0080_3519`) and use it in
  `platform/a733-smp-cached/continue.S`, `platform/a733/cpu_coherence.S` and
  the comparison in `boot/a733/native-el1-cache-probe.S`;
  `prepared_profile()` already compares it. `platform/a733/cpu_continue.{c,S}`
  keep their own literal `0x2_0080_3019` for their uncached tables. In
  `a733_cached_memory_prepare()`, before the first descriptor write, issue
  `DC CIVAC` over the five table pages (Secure space, MMU off), then `DSB SY`;
  with every cache off nothing reallocates those lines. After
  `a733_cpu_coherence_enable_el3()` on core 0, and before any core enables
  EL1, issue `DC IVAC` on the EL1 level-1 and level-2 pages through the EL3
  Non-secure Normal-NC alias, then `DSB SY`, as
  `platform/a733-smp-supervised/publication.S` does. Update the checks that
  assert the cached-path value: `checks/a733-cpu-coherence.py`,
  `checks/native-startup-probe.py`, `checks/gpu-single-core.py`,
  `checks/{board,crypto,npu,pwm,rtc,thermal}-single-core.py`,
  `apps/gpu-finite/return-check.py` and the host models in
  `tools/{all-core-pre-scheduler-host,prepare-primary-cache-probe,secondary-cache-host,run-native-startup-probe}.py`;
  `checks/a733-cpu-continue.py` keeps the uncached value.
- **Existing cacheable walks.** `platform/a733/platform.c` and the
  non-cached branch of `apps/storage-read-single-core/platform.patch` already
  set IRGN0 = ORGN0 = 1 on tables written with the MMU off. Issue `DC CIVAC`
  over `level1` and `level2` before the first descriptor write, then `DSB SY`.
  Also move the level-2 loop of `platform_init()` into a function that
  `checks/memory-pool-blocks.c` can call.
- **Entry cache state.** Two residual risks remain after this maintenance.
  A Non-secure dirty line over the EL1 tables at EL3 entry could be evicted
  between a descriptor write and the invalidate. A Secure dirty line over the
  `dma32` or `uncached` pool cannot be removed by Non-secure EL1
  maintenance. Specify the launcher's dirty-line and valid-line
  experiments in `docs/cold-entry.md`. Their passing result is a precondition
  for enabling cacheable walks and for qualifying the pools on the board.
- **Multi-range maintenance:** one call over several ranges with one leading
  and one trailing barrier, for GPU frame and NPU transitions.
- **One final table builder:** set the outer-shareable field of `l1[0]` in
  `a733_cpu_coherence_tables()` and delete the later writes in
  `apps/storage-read-single-core/monitor.c` and
  `apps/dsi-scanout/monitor.patch`, so the golden comparison covers every
  extended image's final tables.
- `platform/a733-smp-cached/memory.c` reads the layout header constants
  directly.
- Stage 2: EL1 level-1 entries 2 and 3 become
  `PA | UXN | PXN | AF | SH=0b11 | AttrIndx=1 | 1` with AP[2:1] = 0b00; EL3
  entries stay invalid. Stage 3 adds entries 4 to 12 the same way.

## Board evidence that would confirm the design

- Every board budget above is met on cores 0 and 6.
- The recognizer runs at EL0, and each forbidden access raises its expected
  fault class.
- The GPU and NPU complete checksummed jobs through 2 MiB device-MMU entries.
- Stage 2: the DRAM check passes for 12,288 MiB, patterns written above
  `0x8000_0000` do not alias into low DRAM, and the GPU and NPU complete
  checksummed jobs inside their classes.
