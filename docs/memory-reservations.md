# Linker-owned device memory and DMA allocations

The single-core A733 image can reserve a dedicated pool for device buffers.
The linker places it after code, data, page tables, stacks and the application
heap, within the existing image reservation at `0x41000000..0x41efffff`.
The allocator and DMA ownership adapter are **implemented**. Physical RAM,
cache visibility and device completion remain unqualified.

This document assumes familiarity with virtual memory and device drivers.
A lease identifies one allocation through its pool, owner, slot and generation.
The generation changes whenever an allocation slot is reused.

## The linker fixes the complete pool extent

The default profile accepts `__device_pool_size` in multiples of 4 KiB, at most 2 MiB.
For example, `-Wl,--defsym=__device_pool_size=65536` reserves 64 KiB.
The default reserves no bytes and preserves existing allocated sections and
load-segment extents. The linker refuses partial pages, excessive pool capacity
and an image whose heap plus pool exceeds the image reservation.

The `.device_pool` section is `NOLOAD`. It contributes reserved memory without
adding upload bytes. It lies outside BSS clearing and the general heap.
Acquisition returns uninitialized bytes unless the caller uses
`device_memory_acquire_zeroed()`; otherwise the CPU owner must initialize every
byte a device may read before submitting work. Drivers maintain caches over the
complete rounded allocation, including unused padding.

The [native preparation](../platform/a733/memory_reservations.c),
`a733_memory_reservations_prepare()`, derives CPU and physical addresses from
the linker symbols. It makes one write address translation per 2 MiB block
that holds part of the pool, at most two, and requires identity translation,
Normal write-back memory and inner shareability. Every page of a 2 MiB block
descriptor shares that result, and image RAM is mapped with block descriptors.
The optional uncached pool has its own preparation,
`a733_memory_uncached_prepare()`, with one translation. Preparation performs no
writes to pool backing and no peripheral MMIO, and returns success with no
pool when the image links none. The getter returns NULL until preparation has
succeeded.

`platform_init()` is to call both preparations on core 0 before the scheduler
starts. Until it does, the first getter call prepares its pool inside the
kernel critical section, and later calls read a published flag.

The pool excludes image code, driver state, translation tables, monitor state,
exception stacks and heap allocations by construction. It also remains below
the retained result block at `0x42000000`, mimus models at `0x43000000` and
`0x43800000`, recorded input at `0x44000000`, and recognition arena at
`0x46000000`. Boot SRAM firmware and FEL storage lie outside this DRAM range.
The pool does not use the reported 12 GiB capacity to infer more available RAM.
Its image layout still needs physical qualification after independent recovery.

The large NPU graph profile has a fixed exception: its write-back pool occupies
`0x41600000..0x41a00000`, entirely within existing image mappings. Both EL3 table
preparation and the EL1 allocator require that exact interval. Selecting
`SUNSTONE_NPU_LARGE_GRAPH` also requires the retained single-core IRQ runtime;
finite return and the extended-memory profile are incompatible. Existing mapping,
heap exclusion and optional uncached-pool checks still apply.

The shared checks are implemented and compile for AArch64. Default-profile
executable object sections match the earlier NPU image's objects byte for byte.
The graph candidate's linked staging and pool intervals now pass inspection.
Its actual EL3 table builder produces identity mappings with write-back attributes
for every staging and pool page in both translation regimes. The check restricts
writes to the five table pages and the monitor stack. System-register inputs are
modeled; this does not establish physical translation or cache visibility.
Evidence: `npu-large-memory-20261019/linked-table-result.json`; the unchanged
default-profile comparison remains in `compile-results-fixed.json`. Final graph
integration must also preserve the native allocator and device-mapping evidence.

Link ownership describes this image's allocations. It does not prove that a
remote processor or inherited firmware has stopped accessing the same RAM.
All such users must be excluded before native device use. The platform must
also keep CPU mappings pinned and prevent additional cacheable or uncached
aliases. The current image uses cooperative privileged ownership; it does not
isolate allocations from arbitrary privileged writes or all ordinary task access.

## Allocations retain full pages through device ownership

The [pool implementation](../runtime/device_memory_pool.c) tracks at most
32 live allocations. The default image pool contains at most 512 pages; the
allocator can represent 196,608 pages, or 768 MiB, for separately admitted larger
profiles. These are software capacities, not physical RAM qualification.
It rounds requested sizes up to whole pages and returns CPU address, physical
address and complete byte extent. It searches reusable free extents without
moving existing allocations.

Acquire, describe, set state, release and retain each run inside the kernel
critical section, which serializes every core, and none may be called from an
interrupt handler. The owner initializes the pool once, before any other task
can reach it, and never resets its state to reclaim memory. One task at a time
holds a lease, changes its state and maintains its cache lines; handing a lease
to another task needs an ordering event such as task creation or a queue
message. A lease from
another pool, owner or generation is refused. Exhausting the 64-bit generation
counter prevents further allocation; generations never wrap into reuse.
Output metadata cannot overlap the pool or allocator state.

`device_memory_set_state()` changes a CPU-owned allocation to device-owned and
back; it refuses every other change. A CPU-owned allocation may be released. A
device-owned or retained allocation cannot be released through the allocator
API. Retention is permanent for this
pool lifetime. Timeout, a changed controller pointer or a device failure does
not authorize reuse.

The native pool uses write-back mappings. Acquire cleans the part of an extent
that an earlier holder can have left dirty, the pages below the pool's
watermark of the highest page ever handed out, before returning it. A pool
initialized as Non-cacheable runs no clean. The [GPU firmware constructor](gpu-reserved.md)
adds a separate optional uncached linker block with attributes established before
MMU/cache enable. Both pools remain disjoint inside the existing image reservation.
[NPU convolution](npu-reserved.md) binds one complete session to the write-back pool.
IOMMU tables, camera frames and video buffers still need concrete consumer
bindings and device-address policies. Cached multicore builds use their own
linker geometry, with separate admission for extended device-memory reservations.

## DMA users bind through the pool session

DMA images do not lease individual pool allocations around each transfer. The
[DMA pool session](dma-pool-session.md) takes one parent allocation from this pool
for its lifetime, keeps it device-owned, and registers a fixed catalogue of
descriptor and payload regions with the [DMA service](dma-service.md). The native
DMA memory policy admits only those regions, each for its own role. The service
then holds every registered buffer from submission until verified drain and an
explicit REAP. Physical reuse still depends on the DMA controller's completion
contract, which software checks do not qualify.

## Focused validation

The host check runs the allocator against modeled backing storage under
AddressSanitizer and UndefinedBehaviorSanitizer. It checks complete page
extents, stale and cross-pool leases, retained allocations, allocation and
generation exhaustion, every pair of source state and requested change, the
clean watermark, and zeroed extents on write-back and Non-cacheable pools. A
traced build of the pool confirms that every critical-section entry has its
exit. A second host build links both local preparations with modeled
translations at fixed linker addresses. With an unaligned 2 MiB write-back
pool and the uncached pool, preparation makes 3 translations, against 1,536
before the change; with the large NPU graph pool it makes 3, against 2,048.
A faulting block refuses its pool. Native AArch64 compilation covers the
allocator, both preparations and the translation helper. The link mode
compares allocated sections, bytes, existing symbol addresses and
load-segment extents with the previous linker script when no pool is
requested, and checks the explicit pool's placement and linker refusal cases.

Evidence is under `~/.cache/sunstoneos/evidence/memory-architecture-20261022/`.
An earlier relink of the real mimus image with the default pool size kept every
allocated section byte, existing symbol and load-segment extent; that
comparison is in
`~/.cache/sunstoneos/evidence/memory-reservations-20261016/native-links/result.json`.
No physical memory or device access ran in these checks.

Run only the mode affected by a later change:

```sh
~/.cache/sunstoneos/venv/bin/python checks/memory-reservations.py \
  --mode all --output ~/.cache/sunstoneos/evidence/memory-reservations-local
```

The runner accepts `host`, `native`, `link` and the focused `capacity` mode separately and refuses to overwrite
an evidence directory. It saves source inputs before execution, the executed
host binary identity, commands, native objects and result records.
