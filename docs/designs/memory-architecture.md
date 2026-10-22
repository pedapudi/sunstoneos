# Design: device memory classes, boot mapping checks and protection

Status: **design**; no code exists yet. Physical behavior is unqualified until
the board evidence at the end exists. Readers are assumed to know AArch64
stage-1 translation, the FreeRTOS SMP port and non-coherent DMA.

Ten images define `SUNSTONE_A733_EXTENDED_DEVICE_MEMORY` (the cached device
diagnostic, GPU, GPU and display, H.264 decode and encode, HEVC, deinterlace,
G2D, DisplayPort and DSI scanout). They have a 768 MiB write-back device pool,
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

**Stage 3.** `TCR_EL1` already has T0SZ = 25 and a 40-bit IPS. The 9 GiB
above 4 GiB therefore need only level-1 entries 4 to 12, as nine 1 GiB block
descriptors: WB, inner shareable, UXN and PXN, AP[2:1] = 0b00 (EL1
read-write, EL0 no access). Page-table memory stays at five pages. The prerequisite is a
DRAM-controller check at EL3 that `0x4000_0000`–`0x3_4000_0000` decodes
linearly to distinct, nonsecure-accessible DRAM. The range is mapped only when
a consumer, such as recognizer models or NPU weights, claims it as a class.

## Boot checks each pool block once; QEMU checks every entry

The QEMU fixture `checks/device-memory-capacity.{c,py}` runs the cached device
diagnostic and compares all 1,024 EL1 level-1 and level-2 entries with a
golden array written from the map: type, address, attributes, access
permissions, shareability, both execute-never bits and the contiguous bit. It
asserts five table pages. An EL0 task executes text, writes its stack and heap
and reads each recognizer range. It must take a translation fault on an
unowned range and on the monitor block. It must take a permission fault on a
write to text, on execution of data, and on any access to `dma32`, `uncached`
or MMIO.

`a733_device_memory_prepare()` keeps one write-only `AT` per 2 MiB block as
the pools' admission check: two `a733_mapping_check_stride()` calls with
`compare_physical`, WB inner shareable for `dma32` (384 `AT`) and Normal-NC for
`uncached` (8 `AT`). Write permission implies read permission through the same
descriptor. `prepared_profile()` runs once, before the check. Preparation
becomes one call that returns 1 or -1, initializes both pools from the header
constants and publishes READY with a release store; getters use one acquire
load. Callers that loop while the result is 0 need no change.

The local pools of the other images get one `AT` per 2 MiB block over the pool
rounded out to blocks: at most two for the device pool and one for the
uncached pool. One `AT` proves a whole block only when the descriptor is a
block descriptor. A new host check, `checks/memory-pool-blocks.c`, runs every
pool table builder (`a733_cpu_coherence_tables()`,
`a733_memory_boot_uncached()` and the level-2 loop in
`platform/a733/platform.c`) and asserts bits[1:0] = 0b01 for every pool block.
The sampled boot check is valid only while that check passes.

The table pages hold no stale or dirty line. The EL3 entry refuses unless
`SCTLR_EL3.M`, `.C` and `.I` are clear, tables are written with caches off,
EL1 does not map them, and EL3 maps them Normal-NC. The remaining condition,
no valid line at entry, is the cold-reset condition `docs/cpu-coherence.md`
already requires.

## Only acquire takes a lock; every other ledger change is one compare-exchange

`struct device_memory_allocation` replaces its `generation` and `state` fields
with one 64-bit `tag` = generation << 2 | state, read and written only with
`__atomic` operations. Generations stop at 2^62; at one acquire per
microsecond that takes 146,000 years. Because the generation is in the same
word, a stale lease cannot change a reused slot.

- **Acquire** (`device_memory_acquire`, `_aligned`) is the only operation that
  turns a FREE slot into an occupied one, so it alone takes the class mutex.
  Under the mutex it reads tags with relaxed loads, writes `owner`, `first` and
  `pages`, then stores the new tag with release ordering.
- **Release** is a compare-exchange from (generation, CPU) to (generation,
  FREE) with release ordering. `first` and `pages` stay intact, so a
  concurrent search sees either the old extent, which only makes it skip
  memory, or FREE.
- **Retain** loops on compare-exchange from (generation, CPU or DEVICE) to
  (generation, RETAINED). A failed attempt means another transition of the
  same lease completed. The loop returns 1 once the tag is (generation,
  RETAINED) and 0 when the generation differs or the slot is FREE, so retain
  of a valid lease cannot be refused.
- **CPU and DEVICE transitions** use a new `device_memory_set_state(pool,
  lease, to)`, a compare-exchange from the opposite state that returns 0 when
  the lease is stale or the state differs. If two tasks hold copies of a
  lease, exactly one of a transition and a release wins.
- **Describe** loads the tag with acquire ordering, reads the fields, and
  reloads the tag; it succeeds only when both loads match the lease.

The `device_memory_pool.h` contract replaces its single service lock with
these rules. `device_memory_lookup()` and every direct `->state` access go
away, so the
compiler finds each site. In 27 consumer files, 34 plain stores become
`device_memory_set_state()` calls that refuse on 0, and 26 state comparisons
and the direct `generation` reads become `device_memory_describe()` calls.
Consumers stop comparing allocation copies with `memcmp`. The owners make
these edits in the same merge:

- apps: `deinterlace/memory.c`, `dsi-scanout/native.c`, `g2d/memory.c`,
  `gpu-finite/return.c`, `h264-encode/storage.c`, `h264/native.c`, `hdmi-audio/native.c`,
  `hevc/storage.c`, `isp0/memory.c`, `jpeg-encode/native.c`, `jpeg/native.c`,
  `npu-convolution/native.c`, `uart/dma.c`;
- drivers: `camera/a733_camera_native.c`, `crypto/a733_ce_session.c`,
  `display/a733_display_framebuffer.c`, `pci/a733_pcie_epf_test.c`,
  `pci/a733_pcie_nvme.c`, `usb/a733_usb2_native.c`, `usb/a733_xhci.c`;
- `platform/a733/`: `dma_pool_session.c`, `gpu_compute_reserved.c`,
  `gpu_disposal.c`, `gpu_graphics_memory.c`, `gpu_reserved.c`,
  `npu_reserved.c`, `npu_session_reserved.c`;
- 15 host checks that use the ledger fields directly, found by the same
  compile errors.

`struct device_memory_pool` gains one optional pointer to an ops record with
`lock`, `unlock` and `clean`, and a high-water page index `mark`. The native
classes set the pointer from `platform/a733/memory_lock.c`, which holds one
static FreeRTOS mutex per class. Host and emulated checks leave it NULL.

- Before `vTaskStartScheduler()`, `xTaskGetSchedulerState()` returns
  `taskSCHEDULER_NOT_STARTED` and the lock takes no mutex and runs no check.
  Only core 0 can be in the ledger then: in `a733_cached_el1()`
  (`platform/a733-smp-cached/continue.c`) each secondary core waits for
  `ucPrimaryCoreInitDoneFlag`, which the port sets inside
  `vTaskStartScheduler()`.
- With the scheduler suspended, the lock refuses without taking the mutex.
- Otherwise it takes the mutex with a two-tick wait and refuses on timeout.
  A refused acquire holds nothing. Calling acquire from an interrupt handler
  violates the contract, and the lock does not detect it.

## Cache maintenance: the pool cleans after acquire, owners do the rest

**After acquire,** the pool cleans (`DC CVAC`) the part of the new extent
below `mark` and raises `mark` to the extent's end. Acquire reads and raises
`mark` under the mutex; the clean runs after the mutex is released and before
the lease is returned. A previous owner can leave
dirty lines anywhere it wrote, and an eviction during a device write would
overwrite device output in DRAM. Pages above `mark` have never been handed out
since boot and hold no dirty line: the cold-reset condition holds at EL3
entry, EL0 cannot access the pools and no EL1 code writes them without a
lease. A class in an image that does not enter through that EL3 check starts
with `mark` at its end. The native `clean` calls `aarch64_device_cache()`,
which reads `CurrentEL`; an EL0 acquirer of a write-back class therefore
faults on its first acquire in every run, and the emulated check of each image
exercises that acquire. `uncached` has no `clean`. If the clean is refused,
acquire releases the slot and refuses.

Owners do the per-job steps. Maintenance runs at EL1 on 64-byte lines, ranges
rounded out. A transition checks containment with `device_memory_describe()`
and `a733_extent_within()`.

- **CPU to device:** clean the bytes the CPU wrote since the last hand-off,
  then `device_memory_set_state(DEVICE)`.
- **Device to device** (one NPU layer to the next): nothing.
- **Device to CPU:** invalidate (`DC IVAC`) the ranges the job may write, or
  the whole extent when that set is unknown, then set CPU. If the invalidate
  is refused the extent stays DEVICE and the owner retains it.

While an extent is DEVICE the CPU writes none of it, so any line filled
speculatively is clean. In a range the device wrote, that line is stale, and a
later partial-line CPU store would merge stale and new bytes; the invalidate
removes it. Maintenance to the point of coherency acts on every core's cache, so
migration needs no extra step. For `uncached`, the CPU issues `DMB OSHST`
before the doorbell write and `DMB OSHLD` after observing completion.

| Transition | Rate | `DC` operations | Barriers |
| --- | --- | --- | --- |
| After acquire | session open or model load | one per line below `mark` | 2 `DSB SY` |
| CPU to device | GPU frame at 60 Hz, NPU inference, DMA period up to 200 Hz | one per line written | 2 `DSB SY` per transition |
| Device to device | NPU layer; GPU frame to scanout | 0 | 0 |
| Device to CPU | as CPU to device | one per line of the write set | 2 `DSB SY` per transition |

Memory management has no interrupt or polling path. The EL1 tables do not
change after preparation, so READY serves as the mapping generation.

## Reused and removed mechanisms

- Reused: the pool allocator and `device_memory_acquire_aligned()`;
  `a733_cpu_coherence_tables()`; the version-2 boot record;
  `a733_mapping_check_stride()`; `aarch64_device_cache()`; `a733_range.h`; the
  QEMU fixture. Callers that need large device pages pass their alignment to
  `device_memory_acquire_aligned()`; the pool adds no alignment tiers.
- Removed from `device_memory_layout.c`: the cursor and `nc` fields, the
  `SCANNING` and `COMMITTING` states, the pending return, the `busy` atomic,
  `A733_DEVICE_MEMORY_BATCH_PAGES`, `a733_device_memory_commit_allowed()`,
  `same_layout()`, the binding fields only it read, the second
  `prepared_profile()` call and `a733_device_memory_tables_admitted()`.
- Removed from the pool: `device_memory_lookup()` and the plain `state` field.
- Replaced: the two `*_commit_verified()` functions become
  `a733_memory_reservations_publish()` and `a733_memory_uncached_publish()`.
  Seven host checks (`checks/gpu-resource-disposal.py`,
  `gpu-compute-lifecycle-native.py`, `gpu-display-capture-native.py`,
  `gpu-display-native.py`, `displayport-native.py`, `gpu-texture.c`,
  `gpu-batch.c`) call the new names and drop their
  `a733_device_memory_commit_allowed` stub, in the same merge.
- Pending in the GPU and NPU packages: the per-page translation walks in
  `drivers/npu/a733_npu_memory.c` (`live_mappings()`, on publish and finish
  of every job), `drivers/gpu/pvr_compute_service.c` and
  `platform/a733/gpu_memory.c`. This design does not meet the performance
  architecture's "no translation per job" rows. Those packages meet them by
  checking an admitted lease with `device_memory_describe()` and
  `a733_extent_within()`.

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
| Release, retain, transition | none | one compare-exchange; never refused for a valid lease | QEMU count; fixture below |
| Slot comparisons per acquire | at most 1,056 (counted) | unchanged | host replay |
| After-acquire clean, per image's largest extent | not done in the pool | at most 4 cycles per line below `mark`: 65,536 per MiB, 33.6 M (about 19 ms at 1.8 GHz) for a 512 MiB stage-2 extent; 0 on first use | host replay counts lines; board, A55 and A76 |
| Clean or invalidate of a dirty 1 MiB range | to be measured | at most 1.25 times a 1 MiB store loop | board, A55 and A76 |
| `DSB SY` per transition | 2 per range (counted) | 2 per transition | GPU and NPU host models |
| Stage-1 walk cost per GPU frame and NPU inference | not measured | `DTLB_WALK` times two uncached DRAM reads below 1% of the data phase's `CPU_CYCLES` | board, `L2D_TLB_REFILL` and `DTLB_WALK`, cores 0 and 6 |

The pool cleans the whole extent below `mark` because it keeps no per-line
dirty record. A release-side clean would cost the same and depend on every
releaser. If an image's admission clean exceeds its budget, the next step is
one bit per 2 MiB block recording whether any lease has covered it, which
limits the clean to those blocks; this design does not add it.

If the walk cost crosses 1%, the primary agent switches `TCR_EL1` to
IRGN0 = ORGN0 = 0b01 and issues `DC CIVAC` on the five table pages at EL3
before enabling the tables.

`checks/device-memory-capacity.c` replays the GPU and display image's
admission order. Each acquire must make at most 1,056 slot comparisons, the
largest free extent must cover each later allocation, and free extents must be
unchanged after each session opens and closes once. The QEMU fixture also calls acquire with the scheduler suspended and expects a
refusal, and holds a class mutex past the wait while another task releases and
retains leases, which must both succeed.

## Left out of this design

- Replacing the leading `DSB SY` in `aarch64_device_cache()`. Completion reads
  from Device memory need only `DMB OSHLD` before maintenance; the `DSB` stays
  because the SMP port's ordering of a migrating task's stores is not yet
  audited. Both are inputs to a later barrier change by the primary agent.
- The contiguous hint on `dma32`; it waits for the walk measurement.
- Execute-never read-only data and guard pages, which need level-3 tables, and
  `SCTLR_EL1.WXN`, which the golden comparison makes unnecessary.

## Stage 2 needs reservation evidence and a DRAM check first

Profile word 0 of the version-2 boot record becomes 2. The GPU and NPU bases
come only from header constants. The DRAM-controller package provides the EL3
decode check, and launch evidence shows that no boot-loader payload, remote
firmware or master left running by boot0 uses `0x8000_0000`–`0x1_0000_0000`.
A refusal takes the existing EL3 stop. Stage 2 adds one `AT` per class,
raises `DEVICE_MEMORY_MAX_PAGES` to 262,144 and casts the size product in
`device_memory_pool_init()` to `size_t`.

## Changes the primary agent must make

- `arch/aarch64/device_memory.c`: a multi-range `aarch64_device_cache()` with
  one leading and one trailing `DSB SY`, for GPU frame and NPU transitions.
- `platform/a733/cpu_coherence.c`, `apps/storage-read-single-core/monitor.c`,
  `apps/dsi-scanout/monitor.patch`: set the outer-shareable field of `l1[0]`
  in `a733_cpu_coherence_tables()` and delete the later writes, so the golden
  comparison covers every image's final tables.
- `platform/a733-smp-cached/memory.c`: read the layout header constants
  directly, so `a733_device_memory_tables_admitted()` can be deleted.
- `platform/a733/platform.c`: move the level-2 loop in `platform_init()` into
  a function the host block check can call.
- Stage 2, `platform/a733-smp-cached/memory.c`: EL1 level-1 entries 2 and 3
  become `PA | UXN | PXN | AF | SH=0b11 | AttrIndx=1 | 1`, AP[2:1] = 0b00; EL3
  entries stay invalid. Stage 3 adds entries 4 to 12 the same way.
- If the walk budget is exceeded: the `TCR_EL1` and table-clean change above.

## Board evidence that would confirm the design

- Preparation passes within its cycle budget.
- The recognizer runs at EL0, and each forbidden access raises its expected
  fault class.
- Each maintenance type and the after-acquire clean meet their budgets on an
  A55 and an A76.
- Walk cost per GPU frame and NPU inference is below the 1% threshold.
- Stage 2: the DRAM check passes for 12,288 MiB, patterns written above
  `0x8000_0000` do not alias into low DRAM, and the GPU and NPU complete
  checksummed jobs inside their classes.
