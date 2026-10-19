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
Acquisition returns uninitialized bytes; the CPU owner must initialize every
byte a device may read before submitting work. Drivers maintain caches over the
complete rounded allocation, including unused padding.

The [native initializer](../platform/a733/memory_reservations.c) derives CPU and
physical addresses from the linker symbols. It requires identity translation,
Normal write-back memory, inner shareability and write permission on every page.
It uses the existing AArch64 translation and cache-geometry implementation.
Initialization performs no writes to pool backing and no peripheral MMIO.

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

Every operation uses the caller's common service lock. The caller initializes
the pool once and never resets its state to reclaim memory. A lease from
another pool, owner or generation is refused. Exhausting the 64-bit generation
counter prevents further allocation; generations never wrap into reuse.
Output metadata cannot overlap the pool or allocator state.

A CPU-owned allocation may be released. A device-owned or retained allocation
cannot be released through the allocator API. Retention is permanent for this
pool lifetime. Timeout, a changed controller pointer or a device failure does
not authorize reuse.

The native pool uses write-back mappings. The [GPU firmware constructor](gpu-reserved.md)
adds a separate optional uncached linker block with attributes established before
MMU/cache enable. Both pools remain disjoint inside the existing image reservation.
[NPU convolution](npu-reserved.md) binds one complete session to the write-back pool.
IOMMU tables, camera frames and video buffers still need concrete consumer
bindings and device-address policies. Cached multicore builds use their own
linker geometry, with separate admission for extended device-memory reservations.

## DMA completion controls return to CPU ownership

`a733_memory_dma_init()` supplies the actual linker pool to the existing native
DMA memory policy. The policy continues checking every covered CPU page and
performs whole-allocation cache maintenance. The register window and native
adapter state remain outside the pool.

After native controller initialization and channel claim, bind one
`a733_dma_reserved` service to that channel and a nonzero owner identifier.
All service, controller, callback and pool metadata must remain private and
immutable. The service serializes its operations with the pool's allocation
operations. Callers use this service for submission, polling and cancellation.

Submission accepts leases for descriptors and payload buffers. It derives all
CPU addresses, physical addresses and byte lengths from live allocations.
It copies the segment and mapping metadata before invoking the actual DMA
controller. Each allocation must belong to the bound owner and occupy a distinct
slot. Callers may dispose of their input metadata after submission returns.
They must preserve the service and payload bytes according to ownership state.

The service marks all allocations device-owned before controller submission.
A refusal before controller ownership restores CPU ownership. During execution,
completion or cancellation must pass the controller's idle, pending-event,
descriptor-restoration and destination-cache protocol. Only terminal controller
state together with detachment of the specific copied transfer returns the
allocations to CPU ownership. A channel elsewhere reporting completion cannot
release this transfer's buffers.

Copied active services and changed pool, controller, channel or owner bindings
are refused before device callbacks. A detected mismatch retains the original
leases. Recovery also retains every allocation and prevents further callbacks
through that service. These checks detect accidental copying or rebinding;
they do not provide protection against arbitrary metadata corruption.

Physical reuse depends on the DMA controller's completion contract proving that
all accepted accesses have completed. Software checks do not qualify that
hardware guarantee. FIFO producer shutdown remains a separate responsibility;
for example, I2S capture must stop the producer before stopping DMA.
Independent watchdog recovery, native channel ownership, clocks, routing and
remote-user exclusion remain prerequisites for physical execution.

## Focused validation

The composed check runs the allocator, reservation service, actual DMA controller
and memory policy against modeled registers and separate CPU/device byte arrays.
It checks complete page extents, stale and cross-pool leases, allocation and
generation exhaustion, copied submission metadata, complete cache publication,
completion, cancellation, mapping refusal and retained recovery.
Copying an active service or substituting another controller must produce no
device callbacks and must retain the original buffers.

One AddressSanitizer/UndefinedBehaviorSanitizer run passed. Native AArch64
compilation also passed. The companion runner compares allocated
sections, bytes, existing symbol addresses and load-segment extents with the
previous linker script when no pool is requested. It also checks the explicit
pool's placement and linker refusal cases. The real mimus image also preserves every allocated section byte, existing
symbol and load-segment extent after relinking with the default pool size.
It matches the previously built application image. A separate image links the
allocator, native initializer, DMA service, native adapter and cache operations
with a 64 KiB pool. Linking does not establish execution of the initializer.

Evidence is under `~/.cache/sunstoneos/evidence/memory-reservations-20261016/`.
`batch1/results.json` records the focused ownership, native-object and linker
checks. `native-links/result.json` records the actual mimus comparison and native
link closure. The initial freestanding memory-routine compile needed its
`string.h` include path corrected; successful image comparisons were reused.
No physical memory or device access ran in these checks.

Run only the mode affected by a later change:

```sh
~/.cache/sunstoneos/venv/bin/python checks/memory-reservations.py \
  --mode all --output ~/.cache/sunstoneos/evidence/memory-reservations-local
```

The runner accepts `host`, `native`, `link` and the focused `capacity` mode separately and refuses to overwrite
an evidence directory. It saves source inputs before execution, the executed
host binary identity, commands, native objects and result records.
