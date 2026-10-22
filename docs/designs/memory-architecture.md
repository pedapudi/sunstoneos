# Design: device memory classes, boot mapping checks and protection

Status: **design**; no code exists yet. Physical behavior is unqualified until
the board evidence listed at the end exists. Readers are assumed to know
AArch64 stage-1 translation, the FreeRTOS AArch64 ports and non-coherent DMA.

Ten images define `SUNSTONE_A733_EXTENDED_DEVICE_MEMORY`: the cached device
diagnostic, the GPU application, the GPU and display image, H.264 decode,
H.264 encode, HEVC, deinterlace, G2D, DisplayPort and DSI scanout. They map
DRAM up to `0x7900_0000`, with a 768 MiB write-back pool, `dma32`, and a
16 MiB Normal non-cacheable pool, `uncached`. Today boot checks both pools page
by page with 401,408 address translations (`AT`), and nothing enforces the
pool's single-lock rule. Stage 1 checks one `AT` per 2 MiB block, moves the
full-map check to QEMU, locks the ledger, aligns large extents and states the
cache rules. Stage 2 adds a 1 GiB class each for the GPU and the NPU.

## The map keeps EL0 tasks working and leaves unowned DRAM invalid

Each task starts at EL0 unless the image's privilege predicate selects EL1.
Images that apply `patches/freertos-smp-privileged.patch` rename the port's
`npu_service_task_privileged` hook to their own predicate through a compile
definition. In the GPU and display image, `gpu_display_task_privileged` starts
the GPU service and the PMIC, output, render and USB tasks at EL1. The cached
device diagnostic does not apply the patch, so its sixteen tasks start at EL0.
EL0 tasks call kernel functions directly, so kernel data, stacks, the heap and
the recognizer's memory stay readable and writable at EL0.

| Physical range | Use | EL1 descriptor | EL0 |
| --- | --- | --- | --- |
| `0x0`–`0x4000_0000` | MMIO | 1 GiB block, Device-nGnRnE, XN | no access |
| `0x4000_0000`–`0x4100_0000` | boot0 and launcher | 2 MiB blocks, WB, XN | read-write |
| `0x4100_0000`–`0x4120_0000` | text, then read-only data | 1 block, WB, read-only, executable | read-only, executable |
| `0x4120_0000`–`0x4200_0000` | data, BSS, stacks, heap | WB, XN | read-write |
| `0x4200_0000`–`0x4220_0000` | monitor and tables | invalid | none |
| `0x4220_0000`–`0x4800_0000` | boot assets; recognizer models, input, arena | WB, XN | read-write |
| `0x4800_0000`–`0x7800_0000` | class `dma32` | 384 blocks, WB, XN | no access |
| `0x7800_0000`–`0x7900_0000` | class `uncached` | 8 blocks, Normal-NC, XN | no access |
| `0x7900_0000`–`0x8000_0000` | no owner | invalid | none |
| `0x8000_0000`–`0xC000_0000` | class `gpu` (stage 2) | 1 GiB block, WB, XN | no access |
| `0xC000_0000`–`0x1_0000_0000` | class `npu` (stage 2) | 1 GiB block, WB, XN | no access |
| `0x1_0000_0000`–`0x3_4000_0000` | no owner, 9 GiB | invalid | none |

WB means Normal Write-Back Write-Allocate, Inner Shareable. Every writable
entry is execute-never at EL0 and EL1. No translation reaches an invalid range,
so speculation cannot either. Stage 1 maps 910 MiB of DRAM at EL1 and stage 2
maps 2,958 MiB. The 9 GiB above 4 GiB stay unmapped: no consumer exists, and
neither accelerator is qualified to address memory there.

## Boot checks each pool block once; QEMU checks every entry

The QEMU fixture in `checks/device-memory-capacity.{c,py}` runs the cached
device diagnostic and gains:

- an exact comparison of all 1,024 EL1 level-1 and level-2 entries with a
  golden array written from the table above, covering type, address,
  attributes, access permissions, shareability, both execute-never bits and
  the contiguous bit;
- assertions that the tables occupy five pages and that every range with no
  owner is invalid;
- an EL0 task that executes text, writes its stack and heap and reads each
  recognizer range. It must take a translation fault for an unowned range and
  the monitor block, and a permission fault for a write to text, execution of
  data, and any access to `dma32`, `uncached` or MMIO.

The table pages cannot hold a stale or dirty cache line. The EL3 entry
refuses unless `SCTLR_EL3.M`, `.C` and `.I` are clear, and every table is
cleared and written with translation and caches off. Afterwards EL3 maps the
table pages Normal-NC, EL1 does not map them (`l2[16]` is 0), and `TCR_EL1`
selects non-cacheable walks. No cacheable alias exists that could create a
line. The one remaining condition, that no cache holds a valid line for those
addresses at entry, is the cold-reset condition `docs/cpu-coherence.md`
already requires.

`a733_device_memory_prepare()` keeps one translation per block at EL1 as the
pools' admission check, at the 2 MiB granule section 1.5 of the performance
architecture names. It confirms on the board that each address the allocator
hands out translates to itself with the attribute the cache rules assume. Two
`a733_mapping_check_stride()` calls with a 2 MiB step use write-only rules with
`compare_physical`: WB inner shareable for `dma32` (384 `AT`) and Normal-NC
outer or inner shareable for `uncached` (8 `AT`). EL1 write permission implies
read permission through the same descriptor, so a read translation adds
nothing. Then `0x47E0_0000` must translate and `0x7900_0000` must fault.
`prepared_profile()` keeps its `TTBR0_EL1`, `TCR_EL1` and `MAIR_EL1`
comparisons.

The local pools of the other images get the same treatment. Every table
builder for them writes 2 MiB block descriptors: `platform/a733/platform.c`,
`a733_cpu_coherence_tables()` and `a733_memory_boot_uncached()`, which
rewrites one whole block. Each initializer therefore runs one write-only
`AT` per 2 MiB block over its pool rounded out to block boundaries: at most
two for the device pool and one for the uncached pool. Their geometry checks
stay.

Preparation becomes one call returning 1 or -1. After the check it
initializes both pools from the header constants and publishes READY with a
release store; each getter performs one acquire load. Existing callers loop
while the result is 0, so they finish after one call without changes.

## The ledger takes one mutex per class

The pool's contract already requires one lock around every ledger operation,
and section 1.7 of the performance architecture keeps a manager lock for
operations that change state. Seven of the ten images acquire, retain or
release in both pools, for example in `apps/h264-encode/storage.c`,
`apps/g2d/memory.c` and a retain on a failure path of
`apps/dsi-scanout/native.c`. No check enforces that each of those calls runs
before the scheduler or in one task. Without a lock, two concurrent first-fit
searches can claim one free extent.

`struct device_memory_pool` gains one optional pointer to a lock pair. The two
native pools set it; the host checks leave it NULL. `device_memory_acquire`,
`device_memory_acquire_aligned`, `device_memory_release` and
`device_memory_retain` take the lock around the ledger change. A new
`platform/a733/memory_lock.c` holds one static FreeRTOS mutex per pool:

- before `vTaskStartScheduler()`, the lock admits only core 0 and takes no
  mutex, which covers the GPU and HDMI framebuffer acquisitions in `main()`;
- afterwards it takes the mutex with a two-tick wait and refuses on timeout.
  The holder only scans at most 1,056 slot fields, and priority inheritance
  bounds the wait.

After the scheduler starts, the lock calls only FreeRTOS functions, so EL0
and EL1 tasks both work. The `a733_task_context.h` predicates are not used
because they read `CurrentEL` and `DAIF`, which trap at EL0. The contract states
that ledger operations run only in a task or before the scheduler starts,
never in an interrupt handler or a critical section. Transitions between CPU and DEVICE take no lock.
Only the lease holder writes its slot's state word, with relaxed atomic
stores. The locked search reads other slots only to tell FREE from occupied,
which those transitions never change.

## Extents follow four cache-maintenance rules

Every class is one instance of the existing allocator: up to 32 extents,
leases with 64-bit generations and permanent retention. In the extended pools,
acquire aligns an allocation of 2 MiB or more to 2 MiB, one of 64 KiB or more
to 64 KiB, and others to 4 KiB. A requested alignment can only raise this.
`device_memory_acquire_aligned()` already performs the search. Callers that
pass 4096 or 16384 today, such as JPEG and H.264 encode, need no change. A
local pool of at most 4 MiB keeps 4 KiB alignment, because a device TLB of
about a thousand 4 KiB entries already covers it.

The rules below enter the `device_memory_pool.h` contract for write-back
classes. Maintenance runs at EL1, which `aarch64_device_cache()` enforces
before any `DC` instruction, along with 64-byte alignment. A transition checks
the lease, the DEVICE state and containment through a new predicate in that
header. The predicate is true when `[cpu, cpu + bytes)` lies inside the extent
under `a733_range.h` and `physical == extent.physical + (cpu - extent.cpu)`.
Ranges are rounded out to 64-byte lines.

- **After acquire:** the acquirer's unconditional first step cleans
  (`DC CVAC`) the whole extent. The pool keeps no state for this. An earlier
  owner or boot code can leave dirty lines anywhere in it, and an eviction
  during a device write would overwrite device output in DRAM.
- **CPU to device:** clean the bytes the CPU wrote since the last hand-off,
  then commit DEVICE. A refused clean leaves the extent in CPU state.
- **Device to device,** such as one NPU layer feeding the next: no
  maintenance, because only CPU writes create dirty lines.
- **Device to CPU:** invalidate (`DC IVAC`) only the ranges the job may write,
  then commit CPU. Every owner knows this set, for example the NPU
  `A733_NPU_BUFFER_WRITE` role or the GPU output and result buffers. When the
  set is unknown, invalidate the whole extent. Ranges the device only read
  need nothing, and the CPU may then read or write any byte.

The reason is the partial-line hazard. While an extent is DEVICE the CPU writes
none of it, and every line it wrote earlier is clean, so a speculative fill
during the transfer copies DRAM. In a range the device only read, that copy
is current. In a written range it is stale, and a later partial-line CPU store
would make a dirty line mixing stale and new bytes; the invalidate removes it.
A rounded-out boundary line holds no dirty data, so invalidating it loses
nothing. If the device-to-CPU invalidate is refused, the extent stays DEVICE,
the CPU does not read it, and the owner retains it.

Maintenance by address to the point of coherency acts on every core's cache,
so a task that migrates needs no extra step. For `uncached`, the CPU issues
`DMB OSHST` between its last Normal-NC store and the doorbell write, and
`DMB OSHLD` between observing completion and reading device output.

| Transition | Rate | `DC` operations | Barriers |
| --- | --- | --- | --- |
| After acquire | once per session open or model load | one per line of the extent | 2 `DSB SY` |
| CPU to device | per GPU frame at 60 Hz, per NPU inference, per playback DMA period at up to 200 Hz | one per line the CPU wrote | 2 `DSB SY` for all ranges together |
| Device to device | per NPU layer; GPU frame to scanout | 0 | 0 |
| Device to CPU, read-only range | as CPU to device | 0 | 0 |
| Device to CPU, written range | per readback, inference output or capture period | one per line of the write set | 2 `DSB SY` for all ranges together |
| Ledger operation | admission and teardown only | 0 | one mutex take and give |

A CPU-rendered 8 MiB framebuffer at 60 Hz needs up to 7.9 million `DC CVAC`
per second, and only for rewritten lines. It needs no invalidate, because the
display only reads it.

Memory management has no interrupt or polling path. Exhaustion and lock
timeout are immediate refusals before any device access, and nothing is
retained. A device that may still be writing makes its owner retain the
extent, and only that memory stays held. The EL1 tables do not change after
preparation, so READY serves as the mapping generation.

## Reused and removed mechanisms

- Reused: the pool allocator, leases and `device_memory_acquire_aligned()`;
  `a733_cpu_coherence_tables()`; the version-2 boot record;
  `a733_mapping_check_stride()`; `aarch64_device_cache()`; `a733_range.h`; and
  the QEMU fixture.
- Removed from `device_memory_layout.c`: the cursor and `nc` fields, the
  `SCANNING` and `COMMITTING` states, the pending return, the `busy` atomic,
  `A733_DEVICE_MEMORY_BATCH_PAGES`, `a733_device_memory_commit_allowed()` and
  `same_layout()`. Without `same_layout()`, nothing reads the binding fields
  `self`, `layout`, `generation`, `boot_lease` and `dram_reservation`, so they
  go. `a733_device_memory_tables_admitted()` returns the header constants.
  The binding keeps the state word, `epoch` and `startup_deadline`, which
  `prepared_profile()` reads.
- Replaced: the two `*_commit_verified()` functions become
  `a733_memory_reservations_publish()` and `a733_memory_uncached_publish()`.
  Preparation calls each once and each calls `device_memory_pool_init()` with
  the header constants. Seven host checks call or stub the old names:
  `checks/gpu-resource-disposal.py`, `gpu-compute-lifecycle-native.py`,
  `gpu-display-capture-native.py`, `gpu-display-native.py`,
  `displayport-native.py`, `gpu-texture.c` and `gpu-batch.c`. Each calls the
  two publish functions instead and drops its
  `a733_device_memory_commit_allowed` stub. The emulated checks among them also
  hook the two lock functions by name. Their GPU and display owners make these
  edits in the same merge.
- Kept until replaced: the per-page walks in
  `drivers/gpu/pvr_compute_service.c`, `platform/a733/gpu_memory.c` and
  `drivers/npu/a733_npu_memory.c`. The containment predicate replaces them for
  an admitted extent. That meets the GPU and NPU rows of the performance
  architecture: no translation per job or register access.

## Performance budgets

Instruction counts come from the QEMU fixture (`-icount shift=0`, PMU
instructions retired around the call). Cycles come from PMU `CPU_CYCLES` on
the board, on core 0 (A55) and core 6 (A76). Provisional targets become fixed
after the first measurement.

| Metric | Baseline | Target | Measurement |
| --- | --- | --- | --- |
| Extended pool check | 401,408 `AT` in 3,136 calls; about 17 million instructions, estimated | 394 `AT` in one call; at most 25,000 instructions | QEMU count; board cycles on core 0 |
| Local pool check | up to 2,048 `AT` (counted) | at most 3 `AT` | translation callback count in `checks/memory-reservations.c` |
| Memory mapped at EL1 | 910 MiB of DRAM; the performance architecture's target is 12 GiB | 910 MiB in stage 1, 2,958 MiB in stage 2; the 9 GiB above 4 GiB are a recorded deviation | golden comparison in QEMU |
| Page-table memory | 20 KiB, five pages | 20 KiB in both stages | QEMU assertion |
| Ledger lock, uncontended | none | at most 600 instructions per ledger operation; none on a hot path | QEMU count |
| Slot comparisons per acquire | at most 1,056 (counted) | unchanged | host replay below |
| Clean of 1 MiB of clean lines, once per acquire | to be measured | at most 65,536 cycles, 4 per line (provisional) | board, A55 and A76 |
| Clean, or clean-and-invalidate, of a dirty 1 MiB range | to be measured | at most 1.25 times a 1 MiB store loop on the same core (provisional) | board, A55 and A76 |
| Invalidate of 1 MiB | to be measured | at most 65,536 cycles (provisional) | board, A55 and A76 |
| Maintenance call on one line | to be measured | recorded before and after the multi-range form | board, A55 and A76 |
| `DSB SY` per transition | 2 per range (counted) | 2 per transition | GPU and NPU host models |
| `DC` operations, device to device | one call per 4 KiB page in the GPU data phase | 0 | `pvr-gpu-*` and `a733-gpu-vm-memory.c` host models; pending in the GPU packages |
| `DC` operations, device to CPU on a range the device only read | not counted | 0 | `npu-memory.c` and GPU host models; pending in those packages |
| Device page-table entries per MiB of an extent of 2 MiB or more | 256, 4 KiB pages | 0.5 with 2 MiB pages, 16 with 64 KiB pages | GPU and NPU host models; pending in those packages |

The GPU and NPU owners map extents of 64 KiB or more with their MMU's largest
supported page size, as the hardware documentation or the pinned reference
driver states, or record why not. A replay of the GPU and display image's
admission order in `checks/device-memory-capacity.c` checks three things,
counting alignment padding. Each acquire makes at most 1,056 slot comparisons.
The largest free extent covers the image's largest later allocation. Free
extents are unchanged after each session opens and closes once.

## Left out of this design

- Mapping above 4 GiB, for the reasons given with the map.
- Replacing the leading `DSB SY` in `aarch64_device_cache()`. It orders stores
  that a task made on another core before migrating, and Device-memory
  completion reads before an invalidate. Weakening it would need a new
  precondition audited at about 27 call sites. The multi-range form already
  limits it to one per transition.
- The contiguous hint on `dma32` and cacheable table walks: the CPU touches
  `dma32` only for staging and readback, so both wait for the TLB measurement
  below.
- Execute-never read-only data and guard pages: they need level-3 tables.
- `SCTLR_EL1.WXN`: the golden comparison shows every writable entry is
  execute-never.

## Layout 2 needs reservation evidence and a DRAM check before any code

Layout 2 keeps the version-2 boot record with profile word 0 = 2; word 1
remains the operator's DRAM reservation identity. The GPU and NPU bases come
only from header constants. Two prerequisites from other owners come first.
The DRAM-controller package must provide an EL3 check that
`0x4000_0000`–`0x1_0000_0000` decodes linearly to distinct, nonsecure-accessible
DRAM. Launch evidence must show that no boot-loader payload, remote firmware
or master left running by boot0 uses `0x8000_0000`–`0x1_0000_0000`. A refusal
takes the existing EL3 stop before any table is built. Stage 2 adds one
translation per 1 GiB class to the EL1 check, raises `DEVICE_MEMORY_MAX_PAGES`
to 262,144 and casts the size product in `device_memory_pool_init()` to
`size_t`. Both classes use the alignment rule above.

## Changes the primary agent must make

- `arch/aarch64/device_memory.c`: add a multi-range form of
  `aarch64_device_cache()` that issues the `DC` operations for every range
  between one leading and one trailing `DSB SY`. Its consumers are the GPU
  frame and NPU inference transitions.
- `platform/a733/cpu_coherence.c`, `apps/storage-read-single-core/monitor.c`
  and `apps/dsi-scanout/monitor.patch`: set the outer-shareable field of
  `l1[0]` in `a733_cpu_coherence_tables()` for both regimes, and delete the
  writes after `a733_cached_memory_prepare()` in the monitor and the patch.
  Device memory is outer shareable regardless of the field, and the golden
  comparison then covers the final tables of every image.
- Stage 2, `platform/a733-smp-cached/memory.c`: set EL1 level-1 entries 2 and
  3 to block descriptors `PA | UXN | PXN | AF | SH=0b11 | AttrIndx=1 | 1`,
  with AP[2:1] = 0b00 (EL1 read-write, no EL0 access). EL3 entries stay
  invalid.

## Board evidence that would confirm the design

- Preparation passes on the board within its cycle budget.
- The recognizer runs at EL0 under the tables, and each access the fixture
  forbids raises its expected fault class.
- Each maintenance type meets its budget on an A55 and an A76.
- PMU `L2D_TLB_REFILL` and `DTLB_WALK` during a GPU or NPU data phase give the
  cost of non-cacheable walks.
- Stage 2: the DRAM check passes for 12,288 MiB, patterns written above
  `0x8000_0000` do not alias into low DRAM, and the GPU and NPU complete
  checksummed jobs inside their classes.
