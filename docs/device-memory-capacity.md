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
physical-address selection. No additional table pages are needed.

The actual primary continuation prepares the pools after all eight cores have
passed the existing pre-scheduler barrier. Each call checks at most 64 pages,
using actual read and write address-translation instructions for every page.
Each page must resolve to its identity physical address with the expected memory
attribute and matching read/write shareability. Write-back translations require
inner shareability; Non-cacheable translations accept effective outer or inner
shareability, matching the existing native uncached adapter. Calls verify the installed table address, translation
control, MAIR, primary identity, cold scheduler state and admitted startup deadline.
The runtime epoch and deadline must still match the original handoff.

Both singleton pools remain unavailable until all 200,704 pages pass. The final
publication follows initialization of both allocation ledgers and another startup
check. Any failed translation or admitted-profile check retains a preparation
fault. Restoring metadata cannot resume it. Preparation does not clear payload
memory; controllers must initialize their own allocations before device access.

The new mappings provide privileged device backing. An application running at
EL0 needs a concrete privileged service or a separately admitted mapping. The
current DMA demonstration's refusal of an external pool remains necessary until
its application interface and protected-range geometry support this profile.

## Allocation cost depends on live extents

Each existing pool still has 32 parent-allocation records. Increasing capacity
adds no per-page bitmap or metadata array. The allocator preserves first-fit
placement by jumping past intersecting occupied extents. At most 33 candidate
positions are examined, each against at most 32 records.

`device_memory_acquire_aligned()` additionally accepts a power-of-two alignment
of at least 4 KiB. CPU and physical aliases must have the same alignment phase.
The ordinary acquisition API retains 4 KiB alignment. Device and fault-retained
allocations cannot be released through the CPU release API. All callers still
serialize pool operations and consumer transitions under the actual service lock.
Pool metadata provides cooperative ownership, not isolation against privileged
code that can overwrite it.

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

The capacity check compares allocation placement with an independent page-search
oracle and covers alignment, fragmentation, finite records, generation exhaustion
and retained ownership. It checks the real version-2 admission predicates.

The native fixture uses the actual expanded linker, table constructor, cache-enable
helpers, eight-core barrier, pool preparation and singleton constructors. It checks
changed table entries, EL3 block boundaries, EL0 refusal and monitor isolation.
Distinct executions cover successful publication, an interior missing mapping and
a changed deadline after one preparation batch. No accelerator workloads or
performance measurements are part of this check. Affected native consumers and
compact conditional branches are compiled without replaying their device suites.

The recorded software checks passed the allocator/admission sanitizer run and
all three native preparation cases. Nine affected reservation consumers and six
compact conditional units compiled. The production target built successfully;
its actual object files were reused in the instruction fixture. The launcher
policy was checked separately against that production image. These results do
not qualify physical DRAM, coherence, device transactions or sustained workloads.

The architectural source is the project's pinned AArch64 coherence implementation
and native translation helpers. Existing memory ownership and device-specific
constraints remain those of the actual reservation adapters. The GPU sizing
reference is Mesa revision `86c7176d2dd0065555521bf8c61f208bf53c75d6`,
`src/imagination/vulkan/pvr_arch_device.c` and `pvr_arch_job_render.c`.
