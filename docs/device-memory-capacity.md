# Large device reservations require a qualified boot handoff

The optional cached image reserves 768 MiB of write-back memory and 16 MiB of
Normal Non-cacheable memory. The implementation provides linker reservations,
CPU mappings, bounded boot preparation and allocation ownership. Physical DRAM
capacity, exclusion of remote firmware, coherence and device reachability still
require board qualification. A successful instruction-model check cannot establish
those properties.

| Reservation | Physical and identity CPU range | Capacity |
| --- | --- | --- |
| Write-back | `0x48000000..0x78000000` | 768 MiB |
| Non-cacheable | `0x78000000..0x79000000` | 16 MiB |

Ranges are half-open. Both remain below 2 GiB. Image state remains below
`0x42000000`; the secure monitor and tables remain at
`0x42000000..0x42200000`. Existing mimus model, input and arena ranges below
`0x48000000` remain outside these device reservations.

The build selects `SUNSTONE_A733_EXTENDED_DEVICE_MEMORY` and
`platform/a733-smp-cached/link-devices.ld` together. It includes the actual layout,
pool, cached-memory and boot units in the build identity. The compact linker and
version-1 image retain their original reservations and admission policy. The sealed
observer bundle remains a separate artifact.

## The boot record binds exact linked ranges

The expanded image requires version 2 of the existing 224-byte boot record.
Its six formerly reserved words contain, in order:

1. Layout identifier `1`.
2. The host's DRAM reservation identity.
3. Write-back start.
4. Write-back end.
5. Non-cacheable start.
6. Non-cacheable end.

The existing build identity, boot epoch, generation and inverse generation,
watchdog lease and deadlines bind the complete record. The image compares the
copied record with the actual protected handoff and derives its pool endpoints
from immutable linker symbols before constructing tables. A nonzero reservation
identity is only a required field; it does not prove RAM availability.

The distinct host launcher must authenticate physical-memory and remote-firmware
exclusion evidence for this board, boot epoch, image and exact reservation. The
existing version-1 launcher cannot authorize the expanded image. Qualification
must account for every other CPU or bus master that could access these ranges.
The four-second watchdog lease and existing startup deadline are not extended by
this profile.

## Both CPU regimes map the same attributes

The expanded ranges occupy indices 64 through 455 of the existing RAM level-2
table. Both EL3 and EL1 use identity mappings with matching inner-shareable
attributes. Write-back blocks use the established MAIR value `0xff`;
Non-cacheable blocks use `0x44`. The new blocks are non-executable. Their EL1
permissions deny EL0 access. The EL3 mappings preserve the required non-secure
physical-address selection. No additional table pages are needed. When this
boot's DRAM decode check passes, EL1 also maps `0x8000_0000`–`0x3_4000_0000`
as eleven 1 GiB blocks with no EL0 access, and EL3 maps nothing there. The
complete EL1 map, with the access each range allows, is in
[memory protection](memory-protection.md).

## One call prepares both pools

The primary core calls `a733_device_memory_prepare()` once at EL1, after all
eight cores have passed the pre-scheduler barrier and before the scheduler
starts. The call checks the primary identity, then prepares each pool with
one write address translation per 2 MiB block, 392 in all: each block must
resolve to its identity address with the pool's memory attribute. Write-back
blocks require inner shareability; Non-cacheable blocks accept outer or inner
shareability. It then checks the installed table base, translation control
and MAIR and the admitted epoch and startup deadline once, and sets a READY
flag. Until READY is set both getters return NULL, so a pool initialized
before a refused check stays unreachable.

One translation stands for a whole block because every pool entry is a
level-2 block descriptor. The QEMU fixture fixes that property by comparing
every EL1 level-1 and level-2 entry with the expected map. Two monitor
variants later set the outer-shareable field of level-1 entry 0, the MMIO
block; the capacity check scans the sources and refuses any later table write
of another form, so no later writer changes a pool entry. A refused check
retains the refusal; restoring metadata cannot resume preparation.
Preparation does not clear payload memory.

The EL1 tables do not change after preparation, so READY also marks the
final mapping. Starting the scheduler orders preparation before every task
on every core, so READY is a plain flag and no pool is initialized
concurrently.

The new mappings provide privileged device backing. An application running at
EL0 needs a concrete privileged service or a separately admitted mapping. The
current DMA demonstration's refusal of an external pool remains necessary until
its application interface and protected-range geometry support this profile.

## Allocation is first fit at the class alignment

Each pool has 32 allocation records and no per-page metadata. Acquire scans
for a free record, then tries at most 33 placements, each compared with the
32 records: at most 1,056 record comparisons. Placement is first fit.

Each pool has a class that sets the minimum alignment of its extents, after
the largest page of the device MMU that maps it. Acquire raises a smaller
caller alignment to that minimum:

- `dma32`, the write-back pool: 2 MiB for requests of at least 2 MiB,
  64 KiB for requests of at least 64 KiB, otherwise 4 KiB. These are the
  GPU MMU page sizes, so a GPU mapping of an extent uses large pages.
- `uncached`, local-layout pools and pools built inside a lease keep the
  caller's alignment, at least 4 KiB.

Acquire, release and retain each run inside the kernel critical section,
which serializes every core; initialization runs before any other task can
reach the pool and takes no lock. No interrupt handler may call the ledger.
In a two-core QEMU check, a worst-case acquire with 31 occupied records holds
the critical section for 14,206 instructions, release for 31 and retain for
24. Release and retain on one core complete while the other core runs long
acquires.

Describe takes no lock, because drivers call it on every device access.
Acquire publishes a record's generation after its other fields, release
clears the generation first, and describe accepts a copy only when the
generation equals the lease's before and after it. It costs 92 instructions
and one `DMB`, against 71 instructions for the unordered read before; a locked
describe cost 633 instructions with five supervisor calls.

The ledger operations accept these changes and refuse every other one,
leaving the record unchanged:

| Operation | Accepted from | Result |
| --- | --- | --- |
| `device_memory_retain()` | CPU, DEVICE | RETAINED |
| `device_memory_retain()` | RETAINED | unchanged, success |
| `device_memory_release()` | CPU | FREE |

The holder changes its record between CPU and DEVICE, or to RETAINED, by
writing the `state` field that `device_memory_lookup()` returns, under its own
service lock. That write takes no ledger lock and no transition rule checks
it; a holder never writes FREE or any other field. Nothing in the ledger frees
a retained record, so a timeout or device failure never authorizes reuse,
provided holders do not overwrite RETAINED. Pool metadata records cooperative
ownership. Code that can write it, including EL0 code, can corrupt it,
because kernel data is writable at EL0 in this map.

Acquire also cleans recycled memory. Each write-back pool records a
watermark, the end of the highest page it has handed out. A page above it
has never been handed out and holds no dirty line. Acquire cleans the part
of the new extent below the watermark to the point of coherency, after the
critical section, so a line a previous holder left dirty cannot later
overwrite device output. A page below the watermark that was never handed
out, such as one in an alignment gap, is cleaned as well. The Non-cacheable
pool runs no clean. The clean uses EL1-only instructions, so acquire on a
write-back pool must run at EL1; every current caller programs a device and
therefore runs at EL1. The GPU compute, GPU graphics and UART DMA consumers
still zero and clean their whole fresh extent themselves, so on a recycled
extent the lines below the watermark are cleaned twice at admission.

A provisional concurrent 4K budget is about 653 MiB: three XRGB8888 display
frames, three NV16 camera frames, eight NV12 codec frames, 320 MiB for GPU/NPU
backing, 64 MiB for codec/camera scratch, and 32 MiB for other DMA. The latter
allowances require concrete consumer sizing. They do not establish support for
any particular resolution or concurrent workload. The selected pool leaves
about 115 MiB beyond that provisional budget.

Each device must enforce its own complete-span, alignment and address-width
constraints. Current DMA descriptors must remain below `0xfffff800`; CE and NPU
physical fields and GPU mappings retain their existing validators. Camera's
current IOMMU builder covers only four 4 MiB windows and eight spans. Enlarging
the CPU pool does not enlarge that mapping coverage. GPU free-list growth and
future camera or codec modes may require further bounded consumer work.

## Focused validation covers the changed native path

`checks/device-memory-capacity.py` runs a host check and a QEMU fixture.

The host check compares placement in a class-free pool with an independent
page-search oracle and covers alignment, fragmentation, finite records,
generation exhaustion, retained ownership and the version-2 admission
predicates. It then replays the write-back admission order of each
extended-layout image, and of the finite GPU composition, in a `dma32` pool
and in a pool with 4 KiB alignment.
The replay checks that every extent meets its class minimum, that no request
is newly refused, that each acquire makes at most 1,056 record comparisons,
and that closing and reopening the image's last session restores the same
free extents and cleans each recycled page once.

The QEMU fixture uses the production objects of the extended image: linker
script, table builder, cache-enable helpers, eight-core barrier, pool
preparation and getters. It compares all 1,024 EL1 level-1 and level-2
entries with the map in [memory protection](memory-protection.md), with the
DRAM decode check failing and passing, and checks the ranges the builder
passes to the check. It then runs a successful preparation, an interior
missing mapping and a changed admitted deadline, and reacquires a written
extent in both pools, which runs the production clean at EL1 on the
write-back pool. QEMU runs with `-icount shift=0`, so the PMU instruction
counter is exact; a second image wraps the translation helper to count `AT`
instructions. Two one-core runs trace the table builder with the check
passing and failing.

A second fixture, `checks/device-memory-capacity-local.c`, links the builder
in the local layout with the production cached linker script, in three
images: 2 MiB write-back and uncached pools, a 64 KiB write-back pool alone,
and both pools with the read-only GPU output view. Each runs on one core with
the check passing and failing under an execution trace. It compares every
EL1 and EL3 entry, checks EL1 and EL0 read and write translations of the text
block, the GPU output region, the new blocks and the unmapped ranges, runs the
boot preparation of both pools and one getter call, and traces
`aarch64_device_translate()` and `aarch64_device_translate_el0()`.

The check also scans the sources for writes to the cached tables outside
their builder. With `--images DIR`, it reads the symbol table of every built
image under `DIR`: which decode check and pool stride definitions it links,
and every branch to a pool preparation. It fails when a linked local
preparation is never called, and when an extended-layout image calls a pool
preparation from any function other than `a733_device_memory_prepare()`.

| Measurement | Before | After |
| --- | --- | --- |
| Translations to prepare both pools | 401,408 in 3,136 calls | 392 in one call |
| Instructions to prepare both pools | 27,474,026 | 22,547 |
| Translations to prepare local pools, 2 MiB write-back and uncached, tables from `platform.c` | 1,536 | 1,024 |
| Translations to prepare local pools, large NPU graph, tables from `platform.c` | 2,048 | 1,536 |
| Translations to prepare local pools, 2 MiB write-back and uncached, tables from the cached builder | 1,024 at the first getter call | 3 (2 for a 2 MiB-aligned pool), once at boot |
| Translations to prepare local pools, large NPU graph, tables from the cached builder | 1,536 at the first getter call | 3, once at boot |
| DRAM mapped at EL1, decode check passing | 910 MiB extended, 126 MiB local | 12,174 MiB in both |
| Table builder instructions, decode check excluded | 20,699 extended, 15,636 local | 20,785 extended, 16,882 local |
| Record comparisons per acquire, worst replayed | 1,056 bound | 160; bound 1,056 |
| GPU MMU entries for the replayed extents | one per 4 KiB page | 24 to 137 per image, against 354 to 4,568 |

The alignment raises fragmentation. Before each allocation in the `dma32`
replay, the largest free extent is compared with the same order at 4 KiB
alignment. It is up to 1,056 pages (4.1 MiB) smaller for the high-profile
H.264 image and up to 390 pages (1.5 MiB) smaller for the GPU image. For the
other seven replayed orders it is at most 14 pages smaller. The gaps stay
available to smaller extents. Two replays clean memory on first use, because
a later small extent fills an alignment gap below the watermark: the GPU
image's render payload cleans 77,824 bytes and the high-profile H.264 image's
1 MiB metadata extent cleans 1,048,576 bytes. The coarse watermark treats
those never-used gap pages as possibly dirty.

The finite GPU composition cleans the fixed range `0x48000000..0x48086000`
at exit. Under `dma32` placement its 88-page payload moves to 64 KiB
alignment and ends at `0x48088000`, so that range would miss two pages. The
composition links frozen pool objects today; a rebuild against this pool must
derive the range from the actual extents first.

`checks/memory-architecture-smp.py` runs the pool in a two-core QEMU image
with the FreeRTOS SMP port and the real kernel critical section. It measures
describe, an xHCI register read and the camera memory check against the pool
source before this profile, and measures the worst-case acquire hold. It
checks that an acquire and a release inside an enclosing critical section keep
the GIC priority mask raised. It then runs 200,000 rounds of release and
retain on one core against long acquires on the other, followed by 100,000
rounds in which one core describes a released lease while the other gives
its slot to another owner and extent. An execution trace counts the
barriers per call: each acquire, release or retain adds a critical-section
pair of 485 instructions with 5 supervisor calls, 27 `DSB` and 7 `ISB`. A
local pool getter call after preparation costs 5 instructions, one acquire
load and no barrier; it cost 17 when the getter also ran the preparation's
fast path.

These results do not qualify physical DRAM, coherence, device transactions,
sustained workloads or any cycle count on the board. Every cycle budget in
the [design](designs/memory-architecture.md#performance-budgets) remains
provisional and unqualified until the board runs listed there, on cores 0
and 6 with the CPU-L, CPU-B and DSU rates recorded, exist.

The architectural source is the project's pinned AArch64 coherence implementation
and native translation helpers. Existing memory ownership and device-specific
constraints remain those of the actual reservation adapters. The GPU sizing
reference is Mesa revision `86c7176d2dd0065555521bf8c61f208bf53c75d6`,
`src/imagination/vulkan/pvr_arch_device.c` and `pvr_arch_job_render.c`.
