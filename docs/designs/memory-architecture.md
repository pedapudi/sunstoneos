# Design: device memory classes, boot mapping checks and protection

Status: **design**; no code exists yet. Physical behavior is unqualified until
the board evidence listed at the end exists. Readers are assumed to know
AArch64 stage-1 translation, the FreeRTOS AArch64 port and non-coherent DMA.

The cached eight-core image maps DRAM up to `0x7900_0000`. GPU, NPU and 32-bit
DMA buffers share one 768 MiB write-back pool, `dma32`, beside a 16 MiB
Normal non-cacheable pool, `uncached`. Boot checks both pools page by page
with 401,408 address-translation (`AT`) instructions. Consumers change buffer
ownership by writing the allocator's state word directly, with no lock shared
between services. The work lands in two stages:

- Stage 1, layout 1: replace the per-page check with an exact EL3 compare of
  every EL1 descriptor, add a locked API for the shared pools that keeps cache
  maintenance outside the lock, and document the protection the map gives.
- Stage 2, layout 2: add one 1 GiB class each for the GPU and the NPU. It is
  implemented only after the DRAM-controller check and the launch
  coordinator's reservation evidence for the new range exist, so no code
  ships whose only possible result is a refusal.

## Every task except one runs at EL0, and the map keeps that working

The FreeRTOS non-MPU AArch64 port starts tasks at EL0;
`patches/freertos-smp-privileged.patch` starts only the NPU service worker at
EL1, and device owners such as the camera service require EL1 through
`a733_task_context_owner_el1()`. EL0 tasks call kernel functions directly and
enter EL1 only through supervisor calls that mask interrupts or yield, so
kernel data, task stacks and the heap must stay readable and writable at EL0.
The OS image's speech recognizer task reads its models, recorded input and
arena at EL0. Separating kernel from task memory therefore needs the MPU port,
which is reserved for the primary agent and is left out.

| Physical range | Use | EL1 descriptor | EL0 consumers |
| --- | --- | --- | --- |
| `0x0`–`0x4000_0000` | MMIO | 1 GiB block, Device-nGnRnE, EL1 RW, XN | none; device owners run at EL1 |
| `0x4000_0000`–`0x4100_0000` | boot0 and launcher | 2 MiB blocks, WB, EL0/EL1 RW, XN | not audited; access kept |
| `0x4100_0000`–`0x4120_0000` | text, then read-only data | 1 block, WB, AP = `11` (read-only at EL0 and EL1), executable at both | every task |
| `0x4120_0000`–`0x4200_0000` | data, BSS, stacks, heap | WB, EL0/EL1 RW, XN | every task |
| `0x4200_0000`–`0x4220_0000` | monitor and tables | invalid | none |
| `0x4220_0000`–`0x4800_0000` | boot assets; recognizer models `0x4300_0000`, input `0x4400_0000`, arena `0x4600_0000` | WB, EL0/EL1 RW, XN | recognizer task |
| `0x4800_0000`–`0x7800_0000` | class `dma32` | 384 blocks, WB, EL1 RW, XN | none |
| `0x7800_0000`–`0x7900_0000` | class `uncached` | 8 blocks, Normal-NC, EL1 RW, XN | none |
| `0x7900_0000`–`0x8000_0000` | no owner | invalid | none |
| `0x8000_0000`–`0xC000_0000` | class `gpu` (stage 2) | 1 GiB block, WB, EL1 RW, XN | none |
| `0xC000_0000`–`0x1_0000_0000` | class `npu` (stage 2) | 1 GiB block, WB, EL1 RW, XN | none |
| `0x1_0000_0000`–`0x3_4000_0000` | no owner, 8 GiB | invalid | none |

WB means Normal Write-Back Write-Allocate, Inner Shareable. Every writable
entry is execute-never at EL0 and EL1; device classes and MMIO deny EL0. A
range with no owner stays invalid, so a stray access faults and speculation
cannot reach it. Stage 1 changes no descriptor; it verifies them. A QEMU check
runs an EL0 task under the cached tables: it executes text, writes its stack
and heap, reads each recognizer range, and takes a permission fault on `dma32`
and on MMIO.

Both stage-2 classes lie below 4 GiB. The GPU's feature database records
36-bit physical addressing and the NPU vendor source encodes 40-bit addresses,
but neither is qualified above 4 GiB, and the MSI address write in
`drivers/pci/a733_pcie_function.c` is 32 bits wide. A host check refuses any
class that ends above `0x1_0000_0000`.

## Boot compares every EL1 descriptor once at EL3

`a733_device_memory_tables_complete()` runs at EL3 after the base builds the
tables. It gains an exact 64-bit equality compare of all 1,024 EL1 level-1 and
level-2 entries against values derived from the layout constants and the
descriptor rules in `platform/a733/cpu_coherence.c`. Exact equality covers
type, output address, attribute index, `AP[2:1]`, shareability, AF, NS, nG,
the contiguous bit, both execute-never bits and the table pointer. The per-page
check verified read-write access and shareability page by page; the compare
proves the same for every block.

`a733_device_memory_prepare()` becomes one EL1 call returning 1 or -1, with no
`AT`. `prepared_profile()` already compares `TTBR0_EL1`, `TCR_EL1` and
`MAIR_EL1` with their constants; it gains one `SCTLR_EL1` read requiring M, C
and I. Together these make translation walks reach only the compared tables.
`TCR_EL1` selects non-cacheable walks, so the walker sees the entries EL3
wrote with its MMU off. The per-core enable path in
`platform/a733/cpu_coherence.S` issues `TLBI VMALLE1` before setting
`SCTLR_EL1.M`, and no mapping changes after the scheduler starts, so no TLB
entry can differ from the compared tables.

## The shared pools get a locked API with maintenance outside the lock

Each class is one instance of the existing `device_memory_pool` allocator: up
to 32 live extents, leases with 64-bit generations, permanent retention of
device-owned or failed allocations, and a worst-case search of 33 candidate
positions independent of pool size. New owned entry points wrap it per class:
acquire, release, retain, describe, CPU-to-device and device-to-CPU. Each takes
the class's static FreeRTOS mutex only around the ledger read or write. No code
takes any other lock while holding a class mutex, so no lock order exists to
violate. The entry points refuse calls from interrupt context or before the
scheduler starts; pool initialization inside `a733_device_memory_prepare()` is
single-threaded and never takes the mutex.

Cache maintenance for write-back classes runs inside the transition calls,
before or after the locked state change, under the caller's lease. The lease
already gives the caller sole ownership of the extent, so a multi-MiB
invalidate for the camera never delays the display's acquire or transition on
another core. Maintenance runs only when CPU ownership changes:

- CPU to device: clean (`DC CVAC`) the subrange the CPU wrote since the last
  invalidate, then commit DEVICE. A range the CPU did not write holds no dirty
  line, so neither a device read nor a device write there needs maintenance.
- Device to device, such as an NPU layer output feeding the next layer: the
  extent stays DEVICE and no maintenance runs, because the CPU may not access
  a DEVICE extent and only CPU writes create dirty lines.
- Device to CPU: commit CPU, then invalidate (`DC IVAC`) the subrange the CPU
  will read; this also discards lines filled speculatively during the transfer.
  The CPU reads only subranges invalidated since the extent was last DEVICE.

Each maintained subrange must start and end on a 64-byte line boundary;
`aarch64_device_cache()` already refuses anything else. For the `uncached`
class the CPU issues `DMB OSHST` between its last Normal-NC store and the
Device-nGnRnE doorbell write, and `DMB OSHLD` between observing completion
(a status read or a completion word) and reading device-written data.

The `device_memory_pool.h` contract changes to forbid writing the state word of
a pool returned by `a733_memory_reservations_init()` or
`a733_memory_uncached_init()`. Moving to the locked API is a follow-up for the
owners of every current direct writer: `drivers/usb/a733_usb2_native.c`,
`drivers/usb/a733_xhci.c`, `drivers/camera/a733_camera_native.c`,
`drivers/pci/a733_pcie_epf_test.c`, `drivers/pci/a733_pcie_nvme.c`,
`drivers/crypto/a733_ce_session.c`, `drivers/display/a733_display_framebuffer.c`,
`platform/a733/gpu_reserved.c`, `platform/a733/gpu_compute_reserved.c`,
`platform/a733/gpu_disposal.c`, `platform/a733/npu_reserved.c`,
`platform/a733/npu_session_reserved.c`, `platform/a733/dma_pool_session.c` and
the `g2d`, `h264`, `hdmi-audio` and `uart` applications.

A buffer from a class lease needs no per-buffer address check: its pool was
initialized only after preparation returned 1, and the extent comes from
describe. Follow-ups for the GPU and NPU owners: delete the per-page `AT` walks
in `drivers/gpu/pvr_compute_service.c`, `mapped()` in
`platform/a733/gpu_memory.c` and `translated()` in
`drivers/npu/a733_npu_memory.c`; make one transition call per buffer; and map
extents aligned to 2 MiB or 64 KiB with the largest page size their MMU
supports, each with its own budget.

Memory management has no interrupt or polling path. Exhaustion is an immediate
refusal. An interrupt handler records completion and wakes the owning task,
which performs the transition. Faults are synchronous exceptions reported
through the existing fault record.

## Layout 2 needs reservation evidence and a DRAM check before any code

Layout 2 keeps the 224-byte version-2 boot record. Admission requires profile
word 0 = 2, words 2–5 equal to the `dma32` and `uncached` header constants,
and word 1 nonzero; word 1 keeps its meaning as the operator's DRAM
reservation identity, matched against `a733-ddr-reservation-v1` evidence. The
GPU and NPU bases follow from the profile and come only from header constants,
with no linker symbol or record word. Two prerequisites from other owners must
exist first:

- The DRAM-controller package supplies an EL3 check that `0x4000_0000`–
  `0x1_0000_0000` decodes linearly to distinct DRAM and is accessible from the
  nonsecure state.
- The launch coordinator's reservation evidence covers
  `0x8000_0000`–`0x1_0000_0000` and attests that no boot-loader payload or
  remote firmware lies there and no master left running by boot0 writes there.
  `tools/check-a733-device-launch.py` and `platform/a733-device-launch/launch.c`
  accept profile 2 only with that evidence.

A refusal takes the existing EL3 stop with a new code before any table is
built; a layout-2 image has no fallback to layout 1. Stage 2 also raises
`DEVICE_MEMORY_MAX_PAGES` to 262,144 (1 GiB) and casts the size product in
`device_memory_pool_init()` to `size_t`, with a host check that refuses a pool
one page larger.

## Reused and removed mechanisms

- Reused: the pool allocator and leases, `a733_cpu_coherence_tables()`, the
  version-2 boot record, `aarch64_device_cache()`, `a733_range.h` and
  `a733_task_context.h`.
- Removed from `device_memory_layout.c`: the per-page check, its cursor and
  `nc` fields, the `SCANNING` state, the pending return, the `busy` atomic,
  `A733_DEVICE_MEMORY_BATCH_PAGES`, and `same_layout()` from every stage after
  admission, including `ready()` and `commit_allowed()`. The state reaches
  BOUND only after admission checked the linker layout, and nothing writes the
  binding's layout afterwards.

## Performance budgets

QEMU instruction counts are the host regression check: the native fixture in
`checks/device-memory-capacity.py` runs with `-icount shift=0` and reads the
PMU instructions-retired event around each call. They cannot see the cost of
`DC`, `DSB` or `AT` on the A55 or A76, so cycle budgets are measured on the
board with PMU `CPU_CYCLES`, once on core 0 (A55) and once on core 6 (A76),
and listed as board evidence. Targets marked provisional become fixed after
the first board measurement.

| Metric | Baseline | Target | Measurement |
| --- | --- | --- | --- |
| EL1 boot check | 401,408 `AT`, about 17 million instructions estimated from source | 0 `AT`, at most 200 instructions | QEMU; board cycles |
| EL3 descriptor compare | none | at most 15,000 instructions; at most 250 µs (provisional) | QEMU; board cycles |
| Clean of a dirty 1 MiB range | to be measured | at most 1.25 times a 1 MiB store loop on the same core (provisional) | board cycles, A55 and A76 |
| Clean-and-invalidate or invalidate of 1 MiB | to be measured | at most 65,536 cycles, 4 per line (provisional) | board cycles, A55 and A76 |
| Maintenance on a device-to-device handoff | one call per 4 KiB page in the GPU data phase | 0 `DC` instructions | host model |
| Locked call overhead, uncontended | none | at most 300 instructions over the bare allocator call | QEMU |
| Waits by a second consumer during another's maintenance | not modeled | 0 | host model |
| Worst-case acquire | 1,056 extent comparisons | no increase | host model, QEMU |
| Translation-table pages | 5 | 5 | host build |
| Mapped DRAM with no owner | 0 | 0 | EL3 compare, host model |
| TLB invalidations after scheduler start | to be measured | 0 | QEMU `-d in_asm` after the scheduler-start marker |

The host model runs two `dma32` consumers on two threads through the locked
API: while one is blocked inside a stubbed invalidate, the other must complete
acquire, transition and release.

## Left out of this design

- Mapping the 8 GiB above 4 GiB: no consumer exists and neither accelerator's
  addressing is qualified there.
- Removing EL0 access from low DRAM: every task runs at EL0 and needs it.
  Only the boot0 and launcher range might have no EL0 consumer; it stays
  unchanged until an audit of EL0 accesses supports the change.
- Execute-never read-only data: separating it costs a level-3 table, which
  raises instruction-side TLB refills, or 2 MiB of image padding.
- `SCTLR_EL1.WXN`: the exact compare proves every writable entry is
  execute-never at both levels, and EL1 cannot reach the tables.
- Null-pointer and stack guard pages: page 0 lies in the MMIO block, which
  denies EL0, and guards need 4 KiB mappings across the heap.

## Changes the primary agent must make

- Stage 1, `platform/a733-smp-cached/continue.c`: call
  `a733_device_memory_prepare()` once and stop on any result other than 1.
- Stage 1, `arch/aarch64/device_memory.c`: drop the leading `DSB SY` for clean
  and clean-and-invalidate, whose `DC` operations are ordered after earlier
  stores to the same line. Keep it for invalidate, which must follow the read
  of a different location that observed completion. Keep the trailing `DSB SY`.
  Measure both core types before and after.
- Stage 2, `platform/a733-smp-cached/memory.c`: set EL1 level-1 entries 2 and 3
  to WB block descriptors with bits 53 and 54 set; EL3 entries stay invalid.

Dropping the retry loop in `apps/storage-read-single-core/runtime.c` is a
follow-up for its owner.

## Board evidence that would confirm the design

- The EL3 compare and preparation pass on the board, with their cycle counts.
- An EL0 task, including the recognizer, runs under the tables; a write to
  text, execution from data, and an access to an unowned range each raise the
  expected fault class.
- Cache maintenance cycles per MiB on an A55 and an A76 meet the budgets.
- Stage 2 only: the DRAM check passes for 12,288 MiB with linear decode;
  distinct patterns at the first and last 4 KiB of each 256 MiB from
  `0x8000_0000` read back without aliasing into low DRAM; and the GPU and NPU
  complete checksummed jobs in buffers inside their classes.
