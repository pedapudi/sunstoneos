# Design: GPU and NPU memory classes, boot mapping checks and EL1 protection

Status: **design**; no code exists yet. Physical behavior is unqualified until the
board evidence listed at the end exists.

The cached eight-core image maps DRAM up to `0x7900_0000`. GPU, NPU and 32-bit
DMA buffers share one 768 MiB write-back pool. Boot checks that pool page by
page with 401,408 address-translation (`AT`) instructions. Every low-DRAM block
grants EL0 access. This design adds one 1 GiB class each for the GPU and the
NPU, replaces the per-page check with one check per descriptor, and removes EL0
access. Readers are assumed to know AArch64 stage-1 translation and
non-coherent DMA.

## The map is a set of fixed constants and maps only owned memory

DRAM starts at `0x4000_0000`; boot0 reports 12 GiB, which ends at
`0x3_4000_0000`. All mappings are identity mappings in the existing 39-bit
virtual, 40-bit physical configuration, with 1 GiB level-1 entries.

| Physical range | Use | EL1 descriptor | Change |
| --- | --- | --- | --- |
| `0x0`–`0x4000_0000` | MMIO | 1 level-1 block, Device-nGnRnE, RW, XN | none |
| `0x4000_0000`–`0x4100_0000` | boot0 and launcher | level-2 blocks, WB, RW, XN | EL0 access removed |
| `0x4100_0000`–`0x4120_0000` | text, then read-only data | 1 level-2 block, read-only, executable at EL1 | EL0 access removed |
| `0x4120_0000`–`0x4200_0000` | data, BSS, stacks, heap | level-2 blocks, WB, RW, XN | EL0 access removed |
| `0x4200_0000`–`0x4220_0000` | monitor and tables | invalid | none |
| `0x4220_0000`–`0x4800_0000` | boot assets, 16 MiB recognizer arena, boot0 payload | level-2 blocks, WB, RW, XN | EL0 access removed |
| `0x4800_0000`–`0x7800_0000` | class `dma32` | 384 level-2 blocks, WB, RW, XN | none |
| `0x7800_0000`–`0x7900_0000` | class `uncached` | 8 level-2 blocks, Normal non-cacheable, RW, XN | none |
| `0x7900_0000`–`0x8000_0000` | no owner | invalid | none |
| `0x8000_0000`–`0xC000_0000` | class `gpu` | 1 level-1 block, WB, RW, XN | new |
| `0xC000_0000`–`0x1_0000_0000` | class `npu` | 1 level-1 block, WB, RW, XN | new |
| `0x1_0000_0000`–`0x3_4000_0000` | no owner, 8 GiB | invalid | none |

WB means Normal Write-Back Write-Allocate, Inner Shareable. No EL1 descriptor
grants EL0 access, and every descriptor except the text block is XN at both
EL0 and EL1. A range with no owner stays invalid, so a stray access faults and
speculation cannot reach it. Giving a range an owner is a build-time edit that
adds the class constant and its descriptor together.

The `gpu` and `npu` bases join `A733_DEVICE_WB_*` and `A733_DEVICE_NC_*` in
`include/a733_device_memory_layout.h`, which stays the only source of these
addresses. Both new classes lie below 4 GiB. The GPU's feature database records
36-bit physical addressing; the vendor source for the NPU encodes 40-bit
addresses. Neither width is qualified above 4 GiB on the board. Keeping every
class below 4 GiB means no extent can be truncated by a 32-bit submission path,
such as the MSI address write in `drivers/pci/a733_pcie_function.c`. A host check
refuses any class whose end exceeds `0x1_0000_0000`. A class above 4 GiB
requires a fail-closed address-width check in every master's submission path
first.

## Layout 2 is admitted only on decoded, firewall-cleared DRAM

A build option selects layout identifier `2`; layout 1 stays the default. The
224-byte version-2 boot record keeps its six words. For layout 2 the DRAM
reservation identity word holds the first 64 bits of SHA-256 over the four
class ranges, their attributes and the 12,288 MiB DRAM size. The build computes
the same constant, and `a733_device_memory_boot_admit()` compares the two.

The record cannot prove that DRAM exists. Admission at EL3 therefore also
requires a check from the DRAM-controller package. That check reads the
controller's size and decode registers and the DRAM firewall. It passes only
when `0x4000_0000`–`0x1_0000_0000` decodes linearly to distinct DRAM and is
accessible from the nonsecure state, which EL1 uses (`SCR_EL3` = `0x535`).
The linear decode is an assumption until that package establishes it. Until
the check exists, layout 2 is always refused. A refusal takes the existing EL3
stop with a new code before any table is built. There is no fallback to layout
1, because a layout-2 image has no backing for its GPU and NPU services.

## Each class has one allocator instance and one lock

Each class is one instance of the existing `device_memory_pool` allocator: up
to 32 live extents, a lease with a 64-bit generation for each, and permanent
retention of device-owned or failed allocations. Callers already pass 64 KiB or
2 MiB to `device_memory_acquire_aligned()`; a class entry is base and size
only. An allocation in one class never fragments another class or the boot
regions. The worst-case search is 33 candidate positions against 32 extents,
independent of pool size.

`DEVICE_MEMORY_MAX_PAGES` rises from 196,608 to 262,144, the 1 GiB of the
largest class. `device_memory_pool_init()` computes the size limit as a product
of two `unsigned` constants; the product gains a `size_t` cast, as in
`device_memory_acquire_aligned()`. A host check initializes a 1 GiB pool and
refuses a pool one page larger.

The `gpu` and `npu` classes each have one consumer, so the GPU and NPU service
locks serve as their class locks. `dma32` and `uncached` serve several services
in one image, such as display with camera or GPU firmware with display, so
each gets one FreeRTOS mutex. Every operation and state transition (acquire, CPU to device, device to
CPU, release, retain) runs in task context under the owning class's lock. An
interrupt handler records completion and wakes the task; it never calls the
pool. A mutex lets a waiting task block, and separate locks keep one class's
users from waiting on another's.

The lock order is: one service lock, then the `dma32` mutex, then the
`uncached` mutex. No code holds two service locks or takes a service lock while
it holds a class mutex. A GPU session holds leases in `gpu` and `uncached`
under that order. A buffer reaches a second master only as an extent while the
owning service keeps the lease and performs every transition. The
`device_memory_pool.h` contract comment changes to match, and a host model runs
a two-class GPU session against a second `dma32` consumer.

## Device cache maintenance is one helper call per buffer range

Write-back classes follow this sequence, with one `aarch64_device_cache()` call
per buffer range; the helper issues `DSB SY` before and after its loop:

- Before a device reads: clean (`DC CVAC`) the range, then ring the doorbell.
- Before a device writes: clean and invalidate (`DC CIVAC`) the range, so no
  dirty line from an earlier CPU owner can be evicted over device output.
- After completion is observed: invalidate (`DC IVAC`) the range before the CPU
  reads, which also discards lines filled speculatively during the transfer.

Pool extents are page-aligned, so every range is cache-line-aligned. The
`uncached` class holds small structures that the CPU writes and a device polls,
such as GPU firmware state and descriptor rings; it needs no maintenance. Each
physical range has one memory attribute in every regime that maps it.

## Boot checks each descriptor once, and consumers check ranges once

`a733_device_memory_tables_complete()` already runs at EL3 after the base builds
the tables. It gains a compare of all 1,024 EL1 level-1 and level-2 entries
against values derived from the layout constants. The compare checks each
entry's type (block, table pointer or invalid), output address, attribute,
`AP[1]` = 0 and execute-never bits. EL1 cannot read or write the tables, which
lie in the monitor range. The entry for `0x4200_0000` must be invalid.

`a733_device_memory_prepare()` becomes one call at EL1 that returns 1 or -1. It
checks the admitted profile and the startup deadline, then runs one read and
one write `AT` at the base of each of the nine mapped regions. Read-only text
expects a write permission fault, and every other region expects read-write
with its attribute. It checks the deadline again and commits. The `AT` pairs
confirm the EL1 translation registers; the descriptor compare already proved
each entry, so a per-page walk repeats it.

The class constants and the boot check also give consumers a per-range test.
`a733_device_memory_range_admitted(class, cpu, physical, bytes)` returns 1 only
after preparation succeeded and when the identity range lies inside the class.
It replaces per-page `AT` for buffers in a checked class. Adopting it, and one
cache call per buffer range, are follow-ups for the GPU and NPU owners: the
data phase of `drivers/gpu/pvr_compute_service.c`, `mapped()` in
`platform/a733/gpu_memory.c` and `translated()` in
`drivers/npu/a733_npu_memory.c` each work one 4 KiB page at a time.

There is no interrupt or polling path in memory management. Exhaustion is an
immediate refusal. No mapping changes after the scheduler starts, so no
break-before-make sequence or TLB broadcast is needed. Permission and
translation faults are synchronous exceptions reported through the existing
fault record.

## Reused, changed and removed mechanisms

- Reused: the pool allocator and leases, `a733_cpu_coherence_tables()`, the
  base's level-2 device loop for `dma32` and `uncached`, the version-2 boot
  record, `a733_mapping_check.h`, `a733_range.h` and `aarch64_device_cache()`.
- Changed: GPU and NPU services allocate from their own classes. The pool limit
  rises to 1 GiB with a widened size product.
- Removed from `device_memory_layout.c`: the per-page check, its cursor and
  `nc` fields, the `SCANNING` state, the pending return, the `busy` atomic
  (only core 0 calls it, once) and `A733_DEVICE_MEMORY_BATCH_PAGES`.
  `apps/storage-read-single-core/runtime.c` drops its retry loop.

## Performance budgets

Instruction counts come from the native fixture in
`checks/device-memory-capacity.py`, run in QEMU with `-icount shift=0`, reading
the PMU instructions-retired event around each call. Baselines are measured
before any change; the targets below derive from about 10 instructions per
entry compare and about 42 per `AT` through `aarch64_device_translate()`.

| Metric | Baseline | Target | Measurement |
| --- | --- | --- | --- |
| EL1 boot mapping check | 401,408 `AT`; about 17 million instructions estimated from source | 18 `AT`, at most 3,000 instructions | QEMU fixture |
| EL3 descriptor compare | none | 1,024 entries, at most 15,000 instructions | QEMU fixture |
| Worst-case acquire | 1,056 extent comparisons | no increase over the measured baseline at 1 GiB | host model with 32 adversarial extents; QEMU fixture |
| Release or describe | to be measured | no increase | QEMU fixture |
| Translation-table pages | 5 | 5 | host build |
| TLB entries for text and read-only data | one 2 MiB block | one 2 MiB block | EL3 compare asserts the level |
| Mapped DRAM with no owner | 0 | 0 | EL3 compare, host model |
| Cache calls per buffer handoff | one per 4 KiB page in the GPU data phase | one per buffer range | host model of each adopting consumer |
| Range admission per buffer | 2 `AT` per page | one range test | host model |
| TLB invalidations after scheduler start | to be measured | 0 | QEMU `-d in_asm` log after the scheduler-start marker |

## Left out of this design

- Mapping the 8 GiB above 4 GiB: no consumer exists, and neither accelerator's
  addressing is qualified there.
- Execute-never read-only data: the bytes are constant. Separating them costs a
  level-3 table, which raises instruction-side TLB refills, or a 2 MiB block,
  which grows flat images by about 2 MiB of padding.
- `SCTLR_EL1.WXN`: the EL3 compare proves no writable entry is executable, and
  EL1 cannot write the tables.
- EL0 tasks, null-pointer guard pages and stack guard pages: no task runs
  untrusted code, page 0 lies in the MMIO block, and guards need 4 KiB
  mappings across the heap.

## Changes the primary agent must make

- `platform/a733/cpu_coherence.c`, EL1 regime: clear `AP[1]` (bit 6) on low-DRAM
  blocks; give the code block AP = `10` (bit 7) and set UXN (bit 54).
- `platform/a733-smp-cached/memory.c`, layout 2 only: set EL1 level-1 entries 2
  and 3 to WB block descriptors with bits 53 and 54 set. EL3 entries stay
  invalid; EL3 never accesses these classes.
- `platform/a733-smp-cached/continue.c`: call `a733_device_memory_prepare()`
  once, without the retry loop.

## Board evidence that would confirm the design

- The DRAM-controller check passes: 12,288 MiB, linear decode, and nonsecure
  firewall access up to 4 GiB.
- Distinct patterns at the first and last 4 KiB of each 256 MiB from
  `0x8000_0000` to 4 GiB read back without aliasing into low DRAM.
- The EL3 compare and preparation pass on the board.
- A write to text, execution from data and an access to an unowned range each
  raise the expected fault class.
- The GPU and NPU complete checksummed jobs in buffers inside their classes.
