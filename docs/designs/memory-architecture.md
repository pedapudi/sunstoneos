# Design: device memory classes, boot mapping checks and protection

Status: **design**; no code exists yet. Physical behavior is unqualified until
the board evidence listed at the end exists. Readers are assumed to know
AArch64 stage-1 translation, the FreeRTOS AArch64 port and non-coherent DMA.

The cached eight-core image maps DRAM up to `0x7900_0000`, with a 768 MiB
write-back pool, `dma32`, and a 16 MiB Normal non-cacheable pool, `uncached`,
for device buffers. Boot checks both pools page by page with 401,408
address-translation (`AT`) instructions. Stage 1 replaces that check with one
`AT` per 2 MiB block, moves the full-map check to QEMU, and states the cache
rules for pool extents. Stage 2 adds a 1 GiB class each for the GPU and the
NPU once the DRAM-controller check and reservation evidence exist.

## The map keeps EL0 tasks working and leaves unowned DRAM invalid

The FreeRTOS non-MPU AArch64 port starts every task except the NPU service
worker at EL0, and EL0 tasks call kernel functions directly. Kernel data,
stacks, the heap and the recognizer's models, input and arena therefore stay
readable and writable at EL0. Separating kernel from task memory needs the
MPU port, which is reserved for the primary agent.

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
entry is execute-never at EL0 and EL1, and speculation cannot reach an invalid
range. The stage-2 classes lie below 4 GiB because neither accelerator is
qualified to address memory above it.

## Boot checks each pool block once; QEMU checks the full map

The EL1 tables come from `a733_cpu_coherence_tables()` and the pool loop in
`platform/a733-smp-cached/memory.c`, whose inputs are link-time constants
already checked at EL3 by admission. A defect in their output is therefore a
build defect. The QEMU fixture in `checks/device-memory-capacity.{c,py}`
gains:

- an exact comparison of all 1,024 EL1 level-1 and level-2 entries with a
  golden array written from the table above, covering type, address,
  attributes, access permissions, shareability, both execute-never bits and
  the contiguous bit;
- assertions that the tables occupy five pages and that every range with no
  owner is invalid;
- an EL0 task that executes text, writes its stack and heap and reads each
  recognizer range, and that takes a permission fault when it writes text,
  executes data, or accesses `dma32`, MMIO or an unowned range.

A build check cannot see what the board's walker reads: `TCR_EL1` selects
non-cacheable walks, so missed maintenance of the table pages would show the
walker other entries. `a733_device_memory_prepare()` therefore keeps one
translation per block at EL1 on the final tables. It calls
`a733_mapping_check_stride()` with a 2 MiB step and a write rule over both
pools (392 `AT`), then translates `0x47E0_0000`, which must succeed, and
`0x7900_0000`, which must fault.
`prepared_profile()` keeps its `TTBR0_EL1`, `TCR_EL1` and `MAIR_EL1`
comparisons; a WB result also shows that `SCTLR_EL1.M` is set.

Preparation becomes one call returning 1 or -1. After the check it
initializes both pools from the header constants and publishes READY with a
release store; each getter performs one acquire load. The retry loops in
`platform/a733-smp-cached/continue.c` and
`apps/storage-read-single-core/runtime.c` end after the first call.

The local pools of images without the extended layout keep their per-page
check, at most 2,048 translations once per boot. Some of those images build
their tables in `platform/a733/platform.c`, which no host check exercises, so
no build check covers their entries.

## Extents follow four cache-maintenance rules

Each class is one instance of the existing `device_memory_pool` allocator:
up to 32 extents, leases with 64-bit generations and permanent retention. The
`device_memory_pool.h` contract gains these rules for write-back classes. The
lease holder performs them at EL1, checked with
`a733_task_context_owner_el1()`.

- **First CPU to device after acquire:** clean (`DC CVAC`) the whole extent.
  An earlier owner or boot code can leave dirty lines anywhere in it; one
  evicted during a device write would overwrite device output in DRAM.
- **Later CPU to device:** clean the bytes the CPU wrote since the last
  invalidate, then commit DEVICE. A refused clean leaves the extent in CPU
  state.
- **Device to device,** such as one NPU layer feeding the next: no
  maintenance, because only CPU writes create dirty lines.
- **Device to CPU:** first validate the lease, the DEVICE state, 64-byte line
  alignment and containment with `a733_range.h`. Then invalidate (`DC IVAC`)
  and commit CPU. Afterwards the CPU reads or writes only lines invalidated
  since the extent was last DEVICE, or invalidates the whole extent when the
  touched set is unknown. A partial-line store into a stale line, filled
  speculatively during the transfer, would create a dirty line that mixes
  stale and new bytes. A maintenance failure moves the extent to RETAINED.

Maintenance by address to the point of coherency acts on every core's cache,
so task migration between cores needs no extra step. For the `uncached`
class the CPU issues `DMB OSHST` between its last Normal-NC store and the
doorbell write, and `DMB OSHLD` between observing completion and reading
device-written data.

A new host model, `checks/device-memory-coherence.c`, simulates per-line
cache state, DRAM and a device writer, with speculative fills and evictions at
every step. It runs each rule, including an owner that releases a written
extent to a second owner who writes only a subrange before the device writes
all of it. It counts `DC` operations and asserts that every CPU read returns
device data.

## The extended-layout images need no ledger lock

In the three extended-layout images (the GPU application, the GPU and
display image and the cached device diagnostic), acquire and release run
before the scheduler starts or from GPU code alone. Afterwards each task
writes only the state word of slots it leases. In the GPU and display image,
for example, the GPU and the HDMI framebuffer both acquire in `main()` before
`vTaskStartScheduler()`. The contract states this rule. The first image that
acquires or releases from two tasks adds one static FreeRTOS mutex per class
around those two calls, following the `a733_platform_locks` rules and
reusing their context checks.

Memory management has no interrupt or polling path. Exhaustion is an immediate
refusal. A completion interrupt wakes the owning task, which performs the
device-to-CPU transition.

## Reused and removed mechanisms

- Reused: the pool allocator and leases, `a733_cpu_coherence_tables()`, the
  version-2 boot record, `a733_mapping_check_stride()`,
  `aarch64_device_cache()`, `a733_range.h`, `a733_task_context.h` and the QEMU
  fixture.
- Removed from `device_memory_layout.c`: the cursor and `nc` fields, the
  `SCANNING` and `COMMITTING` states, the pending return, the `busy` atomic,
  `A733_DEVICE_MEMORY_BATCH_PAGES`, `a733_device_memory_commit_allowed()`,
  both `*_commit_verified()` functions and `same_layout()`. The two pool
  objects move into `device_memory_layout.c`. After the EL3 table build
  nothing reads `binding.layout`, because preparation and the getters use the
  header constants that admission compared with the linker symbols.
- Kept until replaced: the per-page walks in
  `drivers/gpu/pvr_compute_service.c`, `platform/a733/gpu_memory.c` and
  `drivers/npu/a733_npu_memory.c` also check caller-supplied buffer
  addresses. Their owners may delete them after adding a per-buffer check:
  `[cpu, cpu + bytes)` lies inside the described extent and
  `physical == extent.physical + (cpu - extent.cpu)`, for each GPU page entry
  too.

## Performance budgets

Instruction counts come from the QEMU fixture (`-icount shift=0`, PMU
instructions retired around the call). Cycles come from PMU `CPU_CYCLES` on
the board, on core 0 (A55) and core 6 (A76); provisional targets become fixed
after the first measurement.

| Metric | Baseline | Target | Measurement |
| --- | --- | --- | --- |
| EL1 pool check | 401,408 `AT` in 3,136 calls; about 17 million instructions, estimated | 394 `AT` in 1 call; at most 25,000 instructions | QEMU count; board cycles on core 0 |
| Clean of a dirty 1 MiB range | to be measured | at most 1.25 times a 1 MiB store loop on the same core (provisional) | board, A55 and A76 |
| Clean-and-invalidate of a dirty 1 MiB range | to be measured | at most 1.25 times a 1 MiB store loop on the same core (provisional) | board, A55 and A76 |
| Invalidate of 1 MiB; clean-and-invalidate of 1 MiB of clean lines | to be measured | at most 65,536 cycles, 4 per line (provisional) | board, A55 and A76 |
| `DC` operations on a device-to-device handoff | one call per 4 KiB page in the GPU data phase | 0 | host model |

A host replay of the GPU and display image's admission order in
`checks/device-memory-capacity.c` asserts the allocator bounds: at most 1,056
slot comparisons per acquire, a largest free extent that covers the image's
largest later allocation, and unchanged free extents after each session
opens and closes once.

## Left out of this design

- Mapping the 9 GiB above 4 GiB: no consumer exists and neither accelerator's
  addressing is qualified there.
- The contiguous hint on `dma32` (24 aligned groups of 16 blocks) and
  cacheable table walks: the CPU touches `dma32` only for staging and
  readback, so both wait for the TLB measurement below.
- Execute-never read-only data and guard pages: they need level-3 tables.
- `SCTLR_EL1.WXN`: the golden comparison shows every writable entry is
  execute-never.

## Layout 2 needs reservation evidence and a DRAM check before any code

Layout 2 keeps the version-2 boot record with profile word 0 = 2; word 1
remains the operator's DRAM reservation identity. The GPU and NPU bases come
only from header constants. Two prerequisites from other owners come first:
an EL3 check from the DRAM-controller package that `0x4000_0000`–
`0x1_0000_0000` decodes linearly to distinct, nonsecure-accessible DRAM; and
launch evidence that no boot-loader payload, remote firmware or master left
running by boot0 uses `0x8000_0000`–`0x1_0000_0000`. A refusal takes the
existing EL3 stop before any table is built. Stage 2 adds one translation per
1 GiB class to the EL1 check, raises `DEVICE_MEMORY_MAX_PAGES` to 262,144 and
casts the size product in `device_memory_pool_init()` to `size_t`.

## Changes the primary agent must make

- `arch/aarch64/device_memory.c`: drop the leading `DSB SY` for clean, whose
  `DC` operations are ordered after the same core's earlier stores to the
  line. For invalidate and clean-and-invalidate, replace it with `DMB OSHLD`,
  which orders the completion read before the `DC` operation as it orders a
  store (Linux relies on this after `readl()`). Keep the trailing `DSB SY`;
  measure before and after on core 0 and core 6.
- `platform/a733/cpu_coherence.c` and `apps/storage-read-single-core/monitor.c`:
  set the outer-shareable field of `l1[0]` in `a733_cpu_coherence_tables()`
  for both regimes, and delete the writes after `a733_cached_memory_prepare()`
  in the monitor. Device memory is outer shareable regardless of the field,
  and the golden comparison then covers the final tables of every image.
- Stage 2, `platform/a733-smp-cached/memory.c`: set EL1 level-1 entries 2 and
  3 to WB block descriptors with both execute-never bits; EL3 entries stay
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
