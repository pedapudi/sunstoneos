# Design: device memory classes, boot mapping checks and protection

Status: **design**; no code exists yet. Physical behavior is unqualified until
the board evidence at the end exists. Readers are assumed to know AArch64
stage-1 translation, the FreeRTOS SMP port and non-coherent DMA.

Ten images, from the cached device diagnostic to DSI scanout, define
`SUNSTONE_A733_EXTENDED_DEVICE_MEMORY`. They have a 768 MiB write-back device pool,
`dma32`, and a 16 MiB Normal non-cacheable pool, `uncached`. Today boot checks
both pools page by page with 401,408 address translations (`AT`), and nothing
serializes the ledger across cores. Stage 1 replaces the page walk with one
`AT` per 2 MiB block, moves the full-map check to QEMU, makes the ledger safe
on eight cores and moves the after-acquire cache clean into the pool. Stage 2
adds a 1 GiB class each for the GPU and the NPU. Stage 3 maps the DRAM above
4 GiB when a consumer claims it.

## Map

Tasks start at EL0 unless the image's privilege predicate selects EL1, and EL0
tasks call kernel functions directly, so kernel data, stacks, heap and the
recognizer's memory stay EL0 read-write.

| Physical range | Use | EL1 descriptor | EL0 |
| --- | --- | --- | --- |
| `0x0`–`0x4000_0000` | MMIO | 1 GiB block, Device-nGnRnE, XN | no access |
| `0x4000_0000`–`0x4100_0000` | boot0 and launcher | 2 MiB blocks, WB, XN | read-write |
| `0x4100_0000`–`0x4120_0000` | text, then read-only data | 1 block, WB, read-only | read-only, executable |
| `0x4120_0000`–`0x4200_0000` | data, BSS, stacks, heap | WB, XN | read-write |
| `0x4200_0000`–`0x4220_0000` | monitor and tables | invalid | none |
| `0x4220_0000`–`0x4800_0000` | boot assets; recognizer models, input, arena | WB, XN | read-write |
| `0x4800_0000`–`0x7800_0000` | class `dma32` | 384 blocks, WB, XN | no access |
| `0x7800_0000`–`0x7900_0000` | class `uncached` | 8 blocks, Normal-NC, XN | no access |
| `0x7900_0000`–`0x8000_0000` | no owner | invalid | none |
| `0x8000_0000`–`0xC000_0000` | class `gpu` (stage 2) | 1 GiB block, WB, XN | no access |
| `0xC000_0000`–`0x1_0000_0000` | class `npu` (stage 2) | 1 GiB block, WB, XN | no access |
| `0x1_0000_0000`–`0x3_4000_0000` | no owner until stage 3 | invalid | none |

WB is Normal Write-Back Write-Allocate, Inner Shareable. Every writable entry
is execute-never at EL0 and EL1, and no translation reaches an invalid range.
EL1 maps 910 MiB of DRAM in stage 1 and 2,958 MiB in stage 2.

**Stage 3.** With T0SZ = 25 and a 40-bit IPS already set, the 9 GiB above
4 GiB need only level-1 entries 4 to 12 as 1 GiB blocks, so page-table memory
stays at five pages. It needs an EL3 check that DRAM decodes linearly up to
`0x3_4000_0000`, and is mapped only when a consumer, such as recognizer models
or NPU weights, claims it as a class.

## Boot checks each pool block once; QEMU checks every entry

The QEMU fixture `checks/device-memory-capacity.{c,py}` runs the cached device
diagnostic and compares all 1,024 EL1 level-1 and level-2 entries with a
golden array written from the map: type, address, attributes, access
permissions, shareability, both execute-never bits and the contiguous bit. It
asserts five table pages. An EL0 task exercises each EL0 column entry and
must take the expected translation or permission fault on each forbidden
access.

`a733_device_memory_prepare()` keeps one write-only `AT` per 2 MiB block as
the pools' admission check: two `a733_mapping_check_stride()` calls with
`compare_physical`, WB inner shareable for `dma32` (384 `AT`) and Normal-NC
for `uncached` (8 `AT`). Write permission implies read permission through the
same descriptor. Preparation becomes one call on core 0 that returns 1 or -1,
initializes the pools from the header constants, creates their mutexes and
then publishes READY with a release store; getters use one acquire load.

The local pools of the other images get one `AT` per 2 MiB block over the pool
rounded out to blocks: at most three in total. One `AT` proves a whole block
only when the descriptor is a block descriptor, so a new host check,
`checks/memory-pool-blocks.c`, runs every pool table builder and asserts
bits[1:0] = 0b01 for every pool block. The sampled boot check is valid only
while that check passes.

**Open precondition: no valid or dirty line at EL3 entry.** The cached
image's own code and data depend on it; `docs/cpu-coherence.md` requires it
without establishing it. `docs/cold-entry.md` shows a boot0 set/way clean and
invalidate only in an instruction model, without physical cache geometry or
system-cache coverage. This design skips no maintenance on the strength of
the precondition: the after-acquire clean covers every pool line, and the
table clean requested below covers the tables.

## Only acquire takes a lock; every other ledger change is one compare-exchange

`struct device_memory_allocation` replaces its `generation` and `state` fields
with one 64-bit `tag` = generation << 2 | state; `tag`, `owner`, `first` and
`pages` are accessed only with `__atomic` operations. Because the generation
is in the same word, a stale lease cannot change a reused slot.

Exactly one task, the one that acquired the lease, calls release, retain and
`device_memory_set_state()` for it, and performs its cache maintenance. Other
tasks may hold a copy and call only `device_memory_describe()`.
`include/device_memory_pool.h` states this rule in place of its single
service lock.

- **Acquire** (`device_memory_acquire`, `_aligned`) is the only operation that
  turns a FREE slot into an occupied one, so it alone takes the class lock.
  Under the lock it reads tags with acquire loads, so a slot seen FREE
  synchronizes with the release that freed it. It then issues
  `atomic_thread_fence(release)`, writes `owner`, `first` and `pages` with
  relaxed atomic stores and stores the new tag with release ordering.
- **Release** is a compare-exchange from (generation, CPU) to (generation,
  FREE) with release ordering; `first` and `pages` stay intact.
- **Retain** compare-exchanges from (generation, CPU or DEVICE) to
  (generation, RETAINED). Only the holder writes a valid lease's tag, so the
  first attempt succeeds; a stale lease fails on the generation after at most
  one retry. Retain is never refused for a valid lease.
- **CPU and DEVICE transitions** use a new `device_memory_set_state(pool,
  lease, to)`: a compare-exchange from the opposite state that returns 0 when
  the lease is stale or the state differs.
- **Describe** is a sequence-lock reader: an acquire load of the tag, relaxed
  loads of the fields, `atomic_thread_fence(acquire)`, then a relaxed reload of
  the tag. It succeeds only when both loads match the lease. With the
  acquirer's release fence, a describe that reads a new owner's field also
  reads a tag no older than FREE, and fails.

Removing `device_memory_lookup()` and the plain `state` field makes every
remaining use a compile error (60 sites in 27 consumer files, and 15 host
checks); their owners convert them in the same merge.

**Lock and clean hooks are chosen at link time.** The pool calls
`device_memory_pool_lock()`, `_unlock()` and `_clean()`. Native images,
including the QEMU image, link `platform/a733/memory_lock.c`; host checks link
`runtime/device_memory_pool_host.c`, whose hooks do nothing and return 1.
`struct device_memory_pool` gains two bytes set at init: `write_back`, which
selects whether acquire cleans, and `lock_slot`, the index of the pool's
static mutex or none. Platform pools (the extended classes and the local
device and uncached pools) are initialized on core 0 before
`vTaskStartScheduler()`, and init creates the mutex with
`xSemaphoreCreateMutexStatic()` before READY or the getter's pointer is
published. A pool created later inside a lease, such as the USB controller's
private pool, has no mutex, because only the lease holder uses it. The native
lock:

- before the scheduler starts, takes no mutex, because each secondary core
  waits in `a733_cached_el1()` for `ucPrimaryCoreInitDoneFlag`, which the
  port sets inside `vTaskStartScheduler()`;
- for a pool without a mutex, takes none;
- with the scheduler suspended, or for a slot whose mutex was not created,
  refuses;
- otherwise waits at most two ticks and refuses on timeout; a refused
  acquire holds nothing. Acquire from an interrupt handler is outside the
  contract and is not detected.

## Cache maintenance: the pool cleans after acquire, owners do the rest

**After acquire,** for a write-back class the pool cleans (`DC CVAC`) the new
extent after releasing the lock and before returning the lease. A previous
owner, or boot code before EL3 entry, can leave dirty lines anywhere in it,
and an eviction during a device write would overwrite device output in DRAM.
The clean always runs, so the native hook always reaches
`aarch64_device_cache()`, which reads `CurrentEL` before it examines its
arguments. An EL0 acquirer of a write-back class therefore takes an undefined
instruction fault on every acquire. The tag is already published, so the slot
stays occupied, which retention accepts. If the clean is refused, acquire
releases the slot and refuses. `uncached` has `write_back` clear.

Owners do the per-job steps. Maintenance runs at EL1 on 64-byte lines, ranges
rounded out. Each GPU and NPU job table and each DMA session declares its
device write set at admission as (offset, length) ranges, each checked once
against the lease with `device_memory_describe()` and `a733_extent_within()`.

- **CPU to device:** clean the bytes the CPU wrote since the last hand-off,
  then `device_memory_set_state(DEVICE)`.
- **Device to device** (one NPU layer to the next): nothing.
- **Device to CPU:** clean and invalidate (`DC CIVAC`) the declared write set,
  then set CPU. Whole-extent maintenance is allowed only at session close and
  in error recovery. If the maintenance is refused, the extent stays DEVICE
  and the owner retains it.

While an extent is DEVICE the CPU writes none of it, so its lines are clean
and `DC CIVAC` costs what `DC IVAC` costs. A line filled speculatively inside
the write set is stale and is removed; outside it a filled line matches DRAM.
`DC CIVAC` also cannot discard data: if a task breaks the single-holder rule
and maintains an extent after the owner set CPU and wrote, the dirty lines
reach DRAM. Maintenance to the point of coherency acts on every core's cache,
so migration needs no extra step. For `uncached`, the CPU issues `DMB OSHST`
before the doorbell write and `DMB OSHLD` after observing completion.

| Transition | Rate | `DC` operations | Barriers |
| --- | --- | --- | --- |
| After acquire | session open or model load | one per line of the extent | 2 `DSB SY` |
| CPU to device | GPU frame at 60 Hz, NPU inference, DMA period up to 200 Hz | one per line written | 2 `DSB SY` |
| Device to device | NPU layer; GPU frame to scanout | 0 | 0 |
| Device to CPU | as CPU to device | one per line of the declared write set | 2 `DSB SY` |

Memory management has no interrupt or polling path. The EL1 tables do not
change after preparation, so READY serves as the mapping generation.

## Reused and removed mechanisms

- Reused: the pool allocator and `device_memory_acquire_aligned()`, which
  serves callers that need large device pages, so the pool adds no alignment
  tiers; `a733_cpu_coherence_tables()`; the version-2 boot record;
  `a733_mapping_check_stride()`; `aarch64_device_cache()`; `a733_range.h`;
  the QEMU fixture.
- Removed from `device_memory_layout.c`: the cursor and `nc` fields, the
  `SCANNING` and `COMMITTING` states, the pending return, the `busy` atomic,
  `A733_DEVICE_MEMORY_BATCH_PAGES`, `a733_device_memory_commit_allowed()`,
  `same_layout()`, the binding fields only it read, the second
  `prepared_profile()` call and `a733_device_memory_tables_admitted()`.
- Removed from the pool: `device_memory_lookup()` and the plain `state` field.
- Renamed: the two `*_commit_verified()` functions become
  `a733_memory_reservations_publish()` and `a733_memory_uncached_publish()`;
  seven host checks change with them.
- Pending in the GPU and NPU packages: the per-page translation walks in
  `drivers/npu/a733_npu_memory.c`, `drivers/gpu/pvr_compute_service.c` and
  `platform/a733/gpu_memory.c`. Those packages meet the "no translation per
  job" budgets by checking an admitted lease with `device_memory_describe()`
  and `a733_extent_within()`.

## Performance budgets

Instruction counts come from the QEMU fixture (`-icount shift=0`, PMU
instructions retired around the call) or host replay counters. Cycles come
from PMU `CPU_CYCLES` on the board on core 0 (A55) and core 6 (A76).
Provisional targets become fixed after the first measurement.

| Metric | Baseline | Target | Measurement |
| --- | --- | --- | --- |
| Extended pool check | 401,408 `AT` in 3,136 calls; about 17 M instructions (estimate) | 392 `AT` in one call; at most 25,000 instructions | QEMU count; board cycles, core 0 |
| Local pool check | up to 2,048 `AT` (counted) | at most 3 `AT` | callback count in `checks/memory-reservations.c` |
| Memory mapped at EL1 | 910 MiB | 910 MiB stage 1; 2,958 MiB stage 2; 12,288 MiB stage 3 | golden comparison in QEMU |
| Page-table memory | 20 KiB | 20 KiB in every stage | QEMU assertion |
| Acquire lock, uncontended | none | at most 600 instructions | QEMU count |
| Release, retain, transition | none | one compare-exchange; retain at most two | QEMU count; fixtures below |
| Slot comparisons per acquire | at most 1,056 (counted) | unchanged | host replay |
| After-acquire clean, absent lines | not done in the pool | provisional 4 cycles per line: 33.6 M cycles, about 19 ms at 1.8 GHz, for 512 MiB | host replay counts lines; board, A55 and A76 |
| After-acquire clean, dirty lines; any clean or invalidate of a dirty 1 MiB range | to be measured | at most 1.25 times a store loop over the same bytes | board, A55 and A76 |
| Session-open clean, stage-2 class | none | at most 100 ms per GPU session open or NPU model load | board, core 6 |
| `DC` per device-to-CPU transition | whole READ and WRITE buffers (NPU, counted) | lines in the declared write set | GPU and NPU host models |
| Whole-extent maintenance per frame or inference | none counted | 0 | GPU and NPU host models |
| `DSB SY` per transition | 2 per range (counted) | 2 per transition | GPU and NPU host models |
| Translation-table walk cost | uncached walks, not measured | cacheable walks no slower in any A/B pair | board A/B, below |

The per-line figure is unmeasured: an absent line costs one broadcast `DC`
operation, and a dirty line is bounded by DRAM write bandwidth. GPU session
open and NPU model load happen once per session, and their CPU side runs on
cores 6 and 7 once those are admitted, so the clean runs there. If a measured
clean misses its target, the planned response is one bit per 2 MiB block
recording whether any lease has covered it since boot, limiting the clean to
covered blocks; it depends on the open precondition's board evidence.

**Walk measurement.** `TCR_EL1` is `0x2_0080_3019`: IRGN0 = ORGN0 = 0, so
every TLB-miss walk reads descriptors from uncached DRAM. Walks become
cacheable unconditionally (requested below); a board A/B run of `CPU_CYCLES`,
`DTLB_WALK` and `ITLB_WALK` with both settings confirms it for a GPU frame
and an NPU inference on core 0 and recognizer inference on core 6.

## Left out of this design

- The contiguous hint on `dma32`; it waits for the walk measurement.
- Execute-never read-only data and guard pages, which need level-3 tables, and
  `SCTLR_EL1.WXN`, which the golden comparison makes unnecessary.
- Skipping the clean for never-leased memory, until the open precondition has
  board evidence.

## Fixtures for the ledger, lock and clean

- Host, with no-op hooks: `checks/device-memory-capacity.c` replays the GPU
  and display image's admission order. Each acquire makes at most 1,056 slot
  comparisons, the largest free extent covers each later allocation, and free
  extents are unchanged after each session opens and closes once. Under
  ThreadSanitizer, one thread runs describe on a stale lease while another
  releases and re-acquires the slot; a reported race or a successful describe
  fails the check.
- QEMU native image, which links `memory_lock.c`:
  - acquire with the scheduler suspended refuses;
  - while a class mutex is held past the two-tick wait, acquire refuses and
    release and retain by another task succeed;
  - an EL0 acquire of `dma32` takes an undefined-instruction fault and leaves
    the slot occupied;
  - the stale-describe pair runs on cores 1 and 2 for 10^6 iterations with
    no successful describe.

## Stage 2 needs reservation evidence and a DRAM check first

Profile word 0 of the version-2 boot record becomes 2. The GPU and NPU bases
come only from header constants. The DRAM-controller package provides the EL3
decode check, and launch evidence shows that no boot-loader payload, remote
firmware or master left running by boot0 uses `0x8000_0000`–`0x1_0000_0000`.
A refusal takes the existing EL3 stop. Stage 2 adds one `AT` per class,
raises `DEVICE_MEMORY_MAX_PAGES` to 262,144 and casts the size product in
`device_memory_pool_init()` to `size_t`.

## Changes the primary agent must make

- Cacheable walks: set IRGN0 = ORGN0 = 0b01 in `TCR_EL1` (SH0 = 0b11 is
  already set) and, at EL3 before enabling the tables, `DC CIVAC` the five
  table pages, 320 operations. The tables are not written after enable and
  have no cacheable alias (EL1 does not map them; EL3 maps them Normal-NC),
  so every cached copy matches DRAM, and the clean removes any line left from
  before EL3 entry. A path that later writes them must clean and invalidate
  them by address.
- Leading barrier: audit that the port's context switch
  (`patches/freertos-smp-asm.patch`) orders a migrating task's stores before
  the task resumes on another core. With that confirmed, remove the leading
  `DSB SY` in `aarch64_device_cache()` for a clean and use `DMB OSHLD` before
  an invalidate after a completion read.
- `arch/aarch64/device_memory.c`: a multi-range `aarch64_device_cache()` with
  one leading and one trailing barrier, for GPU frame and NPU transitions.
- `platform/a733/cpu_coherence.c`, `apps/storage-read-single-core/monitor.c`,
  `apps/dsi-scanout/monitor.patch`: set the outer-shareable field of `l1[0]`
  in `a733_cpu_coherence_tables()` and delete the later writes, so the golden
  comparison covers every image's final tables.
- `platform/a733-smp-cached/memory.c`: read the layout header constants
  directly, so `a733_device_memory_tables_admitted()` can be deleted.
- `platform/a733/platform.c`: move the level-2 loop in `platform_init()` into
  a function the host block check can call, and call the local pool getters
  once on core 0 before `vTaskStartScheduler()` in every image that links
  them.
- Stage 2, `platform/a733-smp-cached/memory.c`: EL1 level-1 entries 2 and 3
  become `PA | UXN | PXN | AF | SH=0b11 | AttrIndx=1 | 1`, AP[2:1] = 0b00; EL3
  entries stay invalid. Stage 3 adds entries 4 to 12 the same way.

## Board evidence that would confirm the design

- Preparation passes within its cycle budget.
- The recognizer runs at EL0, and each forbidden access raises its expected
  fault class.
- Each maintenance type and the after-acquire clean meet their budgets on an
  A55 and an A76.
- Cacheable walks are no slower than uncached walks in every A/B pair.
- Cache state at EL3 entry: a launcher check writes a pattern through the
  cache into a pool range before handing over, and EL3 reads it from DRAM with
  caches off. This shows that the boot0 clean reaches every level, including
  the system cache.
- Stage 2: the DRAM check passes for 12,288 MiB, patterns written above
  `0x8000_0000` do not alias into low DRAM, and the GPU and NPU complete
  checksummed jobs inside their classes.
