# GPU workload address-space construction

Native GPU page-table construction is **implemented and host-tested** for the
A733 firmware profile. It prepares mappings for command buffers, shader code,
and workload data. These tables have not been attached to a physical GPU.
The [translation-cache invalidation protocol](gpu-flush.md) has host integration
evidence. Allocation, platform cache publication, live mapping changes, job
submission, and physical recovery remain outstanding.

This document assumes familiarity with device drivers and virtual memory.
The [API](../include/pvr_gpu_vm.h) accepts sorted virtual spans with physically
contiguous backing. Scatter/gather allocations must be split into spans.
It supports 40-bit virtual addresses and 36-bit physical addresses, as reported
by the pinned firmware feature record. Physical GPU identity remains unverified.

These GPU tables are separate from the [MIPS firmware tables](gpu-mips.md).
The reference MIPS startup path skips the GPU BIF initialization used by other
firmware processors. GPU workload mappings are not a prerequisite for executing
MIPS firmware itself.

## Three translation levels cover six page sizes

The root catalogue has 1,024 four-byte entries. Each valid entry points to a
directory with 512 eight-byte entries. A directory entry selects a leaf table
and the page size for its 2 MiB virtual region.

| Page size | Leaf entries used per 2 MiB region |
| --- | ---: |
| 4 KiB | 512 |
| 16 KiB | 128 |
| 64 KiB | 32 |
| 256 KiB | 8 |
| 1 MiB | 2 |
| 2 MiB | 1 |

Each table owns a full 4 KiB backing page, including leaf tables with fewer
entries. Only directories and leaves containing mappings are allocated.
An empty address space requires one zeroed root page. Entries and the firmware
context record are written explicitly in little-endian byte order.

A mapping's virtual address, physical address, and length must align to its
page size. Virtual spans must not overlap. Disjoint spans in the same 2 MiB
region must select the same page size. Data spans may share physical backing;
the privileged admission layer must enforce consistent ownership, protection,
and cache policy across aliases.

Mapping flags encode read-only access, cache coherence, system-level cache
bypass, and firmware/parameter-manager protection. The parameter manager is a
GPU hardware consumer of protected memory. Setting the coherence bit does not
establish platform coherence. This entry format has no execute-disable bit.

## Preparation validates inputs before modifying outputs

The [builder](../drivers/gpu/pvr_gpu_vm.c) first computes exact storage needs.
Callers provide CPU storage, a physical-page list, and a separate 32-byte
firmware memory-context buffer. Backing pages must be nonzero, 4 KiB aligned,
strictly increasing, and within the physical address width. Discontiguous pages
are supported. CPU storage follows the supplied physical-page order.

The builder rejects incorrect sizes, unsupported flags, misalignment, address
overflow, conflicting page sizes, and CPU output overlap with inputs or each
other. It also rejects any mapping that intersects page-table backing, including
backing inside a larger data span. This prevents GPU workload mappings from
exposing the translation tables themselves. A refusal leaves all outputs unchanged.

After validation, preparation clears every table page and writes only valid
entries. Unmapped addresses retain invalid entries; unused table bytes remain
zero. The firmware context receives the root physical address and the reference
unallocated catalogue-register marker. Its remaining bytes are zero.

The platform must prove RAM ownership, actual CPU/physical correspondence,
physical aliases with input/context storage, and device addressability.
It must allocate and map the context record in firmware memory separately.
Inputs must remain stable and outputs exclusively owned throughout preparation.
Offline CPU output bytes may be unaligned; typed input records require normal
C alignment.

Preparation performs no allocation, MMIO, or device synchronization. Device-visible
tables cannot be replaced or released until GPU access has stopped or the required
invalidation and completion protocol has passed. Firmware command acceptance
alone does not prove that GPU memory accesses have finished.

## Translation and reference checks cover complete outputs

One [ASan/UBSan check](../checks/pvr-gpu-vm.c) builds eleven spans using all six
page sizes, eighteen mapped pages, and seventeen table pages. It walks the start,
middle, and end of each mapped page and checks unmapped holes. The fixture crosses
2 MiB and 1 GiB boundaries, includes program-execution heap addresses, and reaches
the highest supported virtual and physical pages.

The check verifies permission bits, discontiguous backing above 4 GiB, unaligned
CPU outputs, allocation guards, empty contexts, and 34 atomic refusals.
Rejected cases include table backing inside a larger mapped span, conflicting
page sizes within one directory entry, malformed ranges, and output/input overlap.

The [reference check](../checks/pvr-gpu-vm-reference.py) compiles the pinned
original root, directory, leaf, and permission encoders. It selects each page-size
constant in turn. The original firmware-context structure, layout assertions,
and initializer supply the context reference. Complete native table and context
bytes match, including invalid entries and padding. Source hashes are verified
against the pinned board-source manifest. This comparison proves format agreement;
it does not prove physical translation or cache behavior.

Clang 21 compiled the component for AArch64 with strict alignment and freestanding
options. It contains 1,828 bytes of code/read-only data and no writable static
data. Reported frames are 96 bytes for construction, 80 for the public requirements
function, and 32 for its planning helper; these are individual frames, not total
call-stack bounds.

Commands, input snapshots, hashes, output buffers, reference code, and objects
are under `~/.cache/sunstoneos/evidence/gpu-vm-20261016/`. Existing firmware,
queue, OS, mimus, and NPU evidence was reused. No device access or performance
measurement occurred.
