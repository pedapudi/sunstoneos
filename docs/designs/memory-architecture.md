# Design: mapping 12 GiB, device reservation classes and memory protection

Status: **design**; no code exists yet. Physical behavior is unqualified until the
board evidence listed at the end exists.

The cached eight-core image maps DRAM only up to `0x79000000`, about 1.9 GiB of
the 12 GiB that boot0 reports. GPU, NPU and DMA buffers share one 768 MiB pool.
Code and read-only data share one executable block. This design maps all DRAM,
gives each large consumer its own reservation, and separates code from data.
Readers are assumed to know AArch64 stage-1 translation and non-coherent DMA.

## The memory map is fixed at build time and checked once at boot

DRAM starts at `0x40000000`, so 12 GiB ends at `0x3_4000_0000`. The existing
translation settings already give a 39-bit virtual and 40-bit physical address
space with level-1 entries of 1 GiB. Every mapping stays an identity mapping.

| Physical range | Size | Use | EL1 attributes | Descriptors |
| --- | --- | --- | --- | --- |
| `0x0`–`0x4000_0000` | 1 GiB | MMIO | Device-nGnRnE, RW, execute-never (XN) | 1 level-1 block, unchanged |
| `0x4000_0000`–`0x4100_0000` | 16 MiB | Low DRAM used by boot0 and the launcher | WB, RW, XN | level-2 blocks, unchanged |
| `0x4100_0000`–`0x4120_0000` | 2 MiB | Text, then read-only data | Text RO executable; read-only data RO XN | 1 new level-3 table |
| `0x4120_0000`–`0x4200_0000` | 14 MiB | Data, BSS, stacks, heap | WB, RW, XN | level-2 blocks, unchanged |
| `0x4200_0000`–`0x4220_0000` | 2 MiB | Monitor and tables | Unmapped at EL1 | unchanged |
| `0x4220_0000`–`0x4800_0000` | 94 MiB | Boot assets, 16 MiB recognizer arena at `0x46000000`, boot0 payload | WB, RW, XN | level-2 blocks, unchanged |
| `0x4800_0000`–`0x7800_0000` | 768 MiB | Class `dma32` | WB, RW, XN | level-2 blocks, unchanged |
| `0x7800_0000`–`0x7900_0000` | 16 MiB | Class `uncached` | Normal non-cacheable, RW, XN | level-2 blocks, unchanged |
| `0x7900_0000`–`0x1_0000_0000` | 2.1 GiB | Free, below 4 GiB | WB, RW, XN | 56 level-2 blocks, 2 level-1 blocks |
| `0x1_0000_0000`–`0x1_4000_0000` | 1 GiB | Class `gpu` | WB, RW, XN | 1 level-1 block |
| `0x1_4000_0000`–`0x1_8000_0000` | 1 GiB | Class `npu` | WB, RW, XN | 1 level-1 block |
| `0x1_8000_0000`–`0x1_c000_0000` | 1 GiB | Class `recognizer` | WB, RW, XN | 1 level-1 block |
| `0x1_c000_0000`–`0x3_4000_0000` | 6 GiB | Free | WB, RW, XN | 6 level-1 blocks |
| from `0x3_4000_0000` | | Not DRAM | Unmapped | none |

WB means Normal Write-Back Write-Allocate, Inner Shareable. No EL1 mapping
grants EL0 access. Memory beyond the reported DRAM size is never mapped. A
Normal mapping of absent memory would allow speculative accesses to it.

Masters that address only 32 bits use `dma32`: DMA0 and DMA1 descriptors,
camera and display I/O spans and, until the address-width survey records
otherwise, the crypto engine. The GPU and NPU address
40-bit physical memory, so their classes start at 4 GiB. That move leaves the
768 MiB pool to 32-bit masters. The provisional 4K budget for display, camera,
codec and DMA buffers needs 333 MiB of it. The `recognizer` class holds buffers that a GPU or
NPU recognition path shares with the CPU. The current CPU recognizer keeps its
fixed 16 MiB arena, whose measured peak is 1,761,840 bytes. Free ranges are mapped
but have no owner. Giving them an owner is a build-time edit to the class table.

The map is one constant table of regions in `platform/a733/memory_map.c`. Each
region has start, end, attribute, permission and class. The primary agent's
table builder emits descriptors from this table. It uses the largest block that
the region's alignment and attributes permit. The 12 GiB profile is version-2
boot-record layout identifier `2`. That identifier is valid only when boot0
reported 12,288 MiB and the DRAM firewall leaves every mapped range accessible
from the nonsecure state. The record keeps its current size and fields.

## Each class has its own allocator instance and lock

Each class reuses the existing `device_memory_pool` allocator unchanged in
behavior. That allocator keeps up to 32 live extents, gives each a lease with a
64-bit generation, and permanently retains device-owned or failed allocations.
A class table entry adds a default alignment of 64 KiB or 2 MiB.
`device_memory_acquire_aligned()` already implements both alignments.

An allocation in one class therefore never fragments another class or the boot
regions. The search examines at most 33 candidate positions, each against at
most 32 extents. The cost is constant in pool size, so a 1 GiB class costs the
same as the 768 MiB pool. A buddy allocator would round a 300 MiB model buffer
to 512 MiB, and allocations occur once per session rather than per job.

`DEVICE_MEMORY_MAX_PAGES` grows from 768 MiB to 4 GiB of 4 KiB pages. Page
indices remain 32-bit. Ledger metadata stays about 1 KiB per class.

Ownership and data flow are unchanged. A CPU-owned allocation becomes
device-owned only through its consumer's completion and cache protocol, and
returns to the CPU only after verified drain. The allocator never writes
payload memory and performs no cache maintenance.

## Protection lives in descriptors and costs nothing per access

- Text is read-only and executable. Read-only data, data, stacks, heap, device
  classes and MMIO are execute-never. The linker aligns the text end to 64 KiB.
  Uniform 64 KiB runs in the level-3 table carry the contiguous hint, so the
  2 MiB code window needs at most 32 TLB entries plus the boundary run.
- `SCTLR_EL1.WXN` makes every writable mapping execute-never in hardware. A
  host check of the region table also refuses any writable executable region.
- Kernel memory is not separated from task memory. The pinned FreeRTOS SMP port
  (`ARM_CR82`) implements task isolation with a Cortex-R82 protection unit. The
  A55 and A76 have a translation-table MMU instead, so the port's mechanism does
  not apply. All tasks run at EL1.

## Synchronization across cores needs no runtime TLB maintenance

Core 0 builds the tables at EL3 before releasing the secondaries; all eight
cores install the same tables. No mapping changes after the scheduler starts,
so no break-before-make sequence or broadcast TLB invalidation is required.
The region and class tables are constant data and need no lock.

Each class ledger is guarded by its consumer service's lock: the GPU service,
the NPU service and the recognition dispatcher. The `dma32` and `uncached`
classes have several consumers. Each gets one FreeRTOS mutex. A global critical
section would stop interrupts on every core. Allocation happens only in task
context. CPU-only sharing, such as the recognizer arena, relies on the DSU's
hardware coherence and needs no cache maintenance.

There is no interrupt or polling path in memory management. Boot preparation
runs on core 0 under the existing startup deadline. Exhaustion is an immediate
refusal with no retry or wait. Permission and translation faults are
synchronous exceptions reported through the existing fault record.

## Device cache policy follows the consumer's access pattern

Write-back memory serves buffers the CPU reads or writes in bulk. Before a
non-coherent master reads, the consumer cleans the buffer by virtual address;
after the master writes, it invalidates. One `DSB SY` follows the complete range.
The `uncached` class serves small structures that the CPU writes and a device
polls on every submission, such as GPU firmware state and descriptor rings.
Each physical range has one memory attribute in every regime that maps it, so
no cacheable and non-cacheable aliases exist.

## Reused, changed and removed mechanisms

- Reused: the pool allocator and leases, `a733_cpu_coherence_tables()` for MMIO
  and low DRAM, the version-2 boot record, `a733_mapping_check.h` for
  translation checks and `a733_range.h` for overlap tests.
- Removed: the per-page translation check of 200,704 pages, which issues a read
  and a write `AT` per page in 3,136 batches. Every page's translation is fixed
  by its leaf descriptor, so one read and one write check per descriptor tests
  the same thing. The admitted-profile and deadline checks still run on each
call, and a host check of the descriptor data still runs. The
  64-page batching state also goes, because one call completes the check.
- Changed: GPU and NPU stop allocating from the 768 MiB pool.
- EL3 does not map memory above `0x7900_0000`. EL3 never accesses it.

## Performance budgets

| Metric | Baseline | Target | Measurement |
| --- | --- | --- | --- |
| Boot mapping check | 401,408 `AT` for 784 MiB (from source); instruction count to be recorded first | At most 2,100 `AT` and 40,000 instructions for 12 GiB | QEMU instruction-count plugin on the native preparation fixture |
| Translation-table pages | 5 | 6 | Host build of the region table |
| Worst-case acquire | 1,056 extent comparisons | Unchanged per class; at most 6,000 instructions | Host model with 32 adversarial extents; QEMU instruction count at `-O2` |
| Release or describe | Not recorded | At most 100 instructions | QEMU instruction count |
| Locks taken per acquire | 1 shared service lock | 1 per-class lock | Host model |
| Cross-class fragmentation | Shared pool | None | Host property test over random sequences |
| TLB invalidations after scheduler start | Not recorded | 0 | QEMU instruction trace |
| Per-access protection cost | 0 | 0 | By construction |

## Deliberately left out

- EL0 tasks and per-task address spaces: no current task runs untrusted code,
  and each context switch would need an address-space switch.
- A null-pointer guard page: the MMIO block at address 0 would need a level-2
  table, and boot SRAM use in low addresses is not yet inventoried.
- Read-only boot assets: images that load assets from storage write that range.
- Stack guard pages: they need 4 KiB mappings across the 14 MiB heap.
- Runtime remapping, demand paging and DRAM discovery beyond boot0's report.

## Changes the primary agent must make in the cached base

`platform/a733-smp-cached/memory.c` replaces its hand-written device loop with a
loop over the region table, and fills level-1 entries 2 to 12 with 1 GiB blocks.
It adds one EL1 level-3 table for the code block and sets `SCTLR_EL1.WXN`.
`link-devices.ld` aligns the text end to 64 KiB. Admission accepts layout `2`.

## Board evidence that would confirm the design

- Boot0's size word reads 12,288 MiB and the DRAM controller decode agrees.
- Distinct patterns at the first and last 4 KiB of every GiB read back without
  aliasing, and the firewall decode shows nonsecure access to every mapped range.
- A write to text, execution from data and an access above the top each raise
  the expected fault class.
- The GPU and NPU complete checksummed jobs in buffers above 4 GiB.
- PMU TLB refill counts show the code-window budget holds.
