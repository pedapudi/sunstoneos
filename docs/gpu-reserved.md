# Boot-owned GPU firmware memory

The native GPU constructor reserves firmware code, data and MIPS page tables
from the write-back device pool. It reserves shared firmware records from a
separate uncached pool. It prepares the actual firmware state and MIPS tables,
validates native CPU translations, and returns the existing GPU service
configuration. Physical GPU operation remains unqualified.

The [constructor](../platform/a733/gpu_reserved.c) accepts the pinned
[firmware image](gpu.md). Firmware decoding checks structure and the supported
allocation profile; it does not authenticate the input. The build must verify
its recorded hash. Session metadata, firmware bytes and page lists remain
private and immutable for the service lifetime.

## The initial boot map owns both pools

The [native linker](../platform/a733/link.ld) reserves an optional 2 MiB uncached
block at `0x41c00000..0x41dfffff`. Set `__uncached_pool_size=2097152` at link time
to enable it. Other nonzero sizes are refused. The image, stacks, heap and
write-back pool must finish before that block. The block remains inside the
existing 15 MiB image reservation; it does not use reported, unqualified RAM.

The [boot mapping helper](../include/a733_memory_boot.h) changes that identity
block before enabling the MMU and data cache. It refuses an enabled MMU or data
cache. Neighbors retain their write-back mappings. The native entry sequence
invalidates inherited CPU caches before entering EL1. Remote-user exclusion,
physical coherency and absence of privileged aliases remain platform
prerequisites. Runtime memory-attribute changes are unsupported.

Each pool has its own allocation registry. The
[uncached initializer](../platform/a733/memory_uncached.c) checks every page with
native address-translation instructions. The [write-back pool](memory-reservations.md)
remains available to the NPU, DMA and other admitted consumers. Cached SMP
images require their own proven binding before using these providers.

## Complete parent allocations stay reserved

The firmware constructor owns 34 write-back pages: 100 KiB code, 20 KiB data
and 16 KiB MIPS tables. Its 19 shared firmware resources occupy 45 uncached
pages, totaling 180 KiB. Child descriptors cover both parents without gaps or
overlap; page lists record actual physical addresses.

Both parent leases become device-owned before preparation. The constructor
retains every acquired parent on subsequent failure, including failure to
allocate the second parent. Child release and session teardown are unavailable.
Later service faults preserve the parent reservations even if their allocation
state remains device-owned. A stopped service does not prove external drain.

Initialize session storage once and serialize all pool mutations before the
scheduler starts. The constructor requires session and input storage in the
identity-mapped image prefix before the pools. Full-span translation checks
exclude input aliases into either parent. Copying or rebinding session metadata is unsupported. Ordinary task mappings
currently provide cooperative ownership; they do not isolate hostile tasks.

Pass the returned configuration and embedded memory adapter to the existing
native backend binding and GPU service initialization. Power, interrupt,
recovery, observed identity and independent isolation providers are still
required. The constructor supplies no success callbacks for those conditions.
Queue formatting remains the service's responsibility during startup.

## A separate constructor reserves fixed-compute memory

This constructor supplies firmware resources and MIPS tables. The
[fixed-compute constructor](gpu-compute-reserved.md) adds six uncached
context/control/timeline pages, 88 write-back program/payload pages and
12 write-back pages requested by the actual VM planner. Both constructors
retain their complete parent allocations through the service lifetime.
The compute path also requires freshly observed single-core hardware capability
before firmware startup and submission. Free lists, wider workloads and
graphics require further concrete providers.

## Focused verification

The [instruction check](../checks/gpu-reserved.py) uses the production boot
mapping helper and linker reservation sections. It combines actual reserved NPU
preparation with actual GPU firmware preparation and native cache publication.
It also checks refusal of absent service providers and retained ownership after
uncached capacity exhaustion. QEMU supplies CPU translation/cache behavior;
no GPU or NPU command executes. Physical cache visibility and remote drain
remain unqualified.

The selected instruction modes passed, as did native boot-object compilation.
The linker check preserved all existing allocated sections, load segments and
symbols when the uncached reservation was absent. It accepted the complete
block and refused both an invalid size and overlap with the image/write-back
pool. The provider accepts matching inner or outer effective shareability for
uncached pages, consistent with the existing GPU adapter.

Evidence is stored in
`~/.cache/sunstoneos/evidence/gpu-reserved-20261016/`. The `selected-modes`
directory contains the executed images, dependency snapshots and results;
`layout` contains the linker comparisons. The executed base image has SHA-256
`7c03ba96e9996a5ecf4915f9cde9d53d74984cb04417ba5485f0b8baace3cfba`.
The external `run-built.py` driver performed the final incremental build and
execution; the repository runner provides the equivalent fresh-build procedure.
