# Native GPU free lists and render-target memory

The native graphics owner reserves real parameter backing, builds hardware
render-target records, and binds them to the same GPU lifecycle service as the
fixed compute job. It uses the existing boot memory pools and free-list manager.
Graphics and compute publication both precede power activation. This component
prepares resources; physical rendering remains unqualified.

The interfaces are [a733_gpu_graphics_memory.h](../include/a733_gpu_graphics_memory.h)
and [pvr_render_target.h](../include/pvr_render_target.h). Construct the reserved
firmware first, then the native compute owner, then the graphics owner. The
firmware must remain offline and the service uninitialized. After construction,
bind the actual platform backend and initialize the service. The graphics
constructor supplies no power, core-observation, startup or drain substitute.

## Complete parents supply bounded child allocations

The graphics owner acquires four parents from the existing 32-entry ledger:

- Write-back parameter pages for the fixed local list and the global maximum.
- Write-back tile and parameter-management backing for two render targets.
- Non-cacheable free-list tables and one packed firmware page.
- Write-back GPU page tables, sized by the actual VM planner.

Each parent becomes device-owned before any initialization. Failure after the
first acquisition retains every acquired parent and prohibits another attempt.
The privileged worker allocates child pages from the retained parameter parent;
it never allocates another global pool entry. Each admitted child has an epoch,
list token, page index and unique lease. The existing manager retains its
returned descriptor before the next operation.

The local list contains 28 pages. The global initial, maximum, growth and reserve
threshold are explicit constructor inputs. Current native free-list metadata
limits each list to 16,384 pages, or 64 MiB. Mesa's 256 MiB global-list policy is
a future capacity requirement, not a hardware limit or an implemented native
profile. Insufficient pool capacity refuses construction without reducing
resolution or page counts. Larger concurrent media/accelerator configurations
require the separately authenticated expanded reservations.

All child storage remains within authentic parent extents. The constructor calls
a direct native compute-admission function that authenticates the private compute
owner, native callbacks, actual boot pools, leases and mappings. A substituted
success callback cannot establish that proof. The actual write-back pool start
bounds private image metadata. Pairwise checks exclude firmware, service,
compute, pool metadata and every retained parent before preparation writes.

Descriptors and pool bindings remain immutable after admission. Manager-owned
page metadata is the exception. These are cooperative privileged ownership
contracts; they do not protect against arbitrary privileged memory corruption.

## Firmware records describe the selected A733 profile

The initial profile supports one sample, one layer and dimensions from 1 through
8,192 pixels on either axis. It uses 16-pixel tiles and parameter format 2.
The source profile's 8K screen feature determines that bound. Multisampling,
layered rendering and other supported hardware modes remain required work.

The portable builder emits an 88-byte common record and two 384-byte hardware
render-target records. The packed non-cacheable page also contains two 64-byte
free-list records and a 32-byte graphics VM record. One real MIPS object maps
that page at `0xc0900000`; individual records use aligned offsets within it.
The service includes this object alongside the 19 firmware and six compute
objects when it rebuilds the MIPS tables.

The workload VM maps the free-list tables with firmware/parameter-manager
protection. Tile and parameter backing includes the virtual heap, tail-pointer
cache, two memory lists and two region-header arrays. Each region receives its
own page-rounded span. The selected single-layer format needs no render-target
cache or macrotile array. A 3,840 × 2,160 target requires 236 KiB of this backing
under the current global-list limit. A color output buffer is additional storage.

Both hardware render-target records reference the local and global free lists.
The pinned driver links them to the local list for reconstruction. The native
callback writes the actual HWR state and clears HAS_LAST_GEOM in both records,
preserving all other firmware-owned bytes. Global-list reconstruction has no
linked render-target records in this profile.

## Publication is part of the actual lifecycle

OPEN formats the firmware and complete MIPS mapping, then advances initial
free-list construction through bounded manager steps. Once both lists are ready,
the service cleans one graphics payload page per step and publishes the actual
GPU page tables and non-cacheable VM record. It then advances existing compute
publication before calling the platform power provider.

The service observes its shared monotonic clock before and after each publication
step. A final operation that completes after the OPEN deadline still faults.
Failure aborts graphics and compute publication and retains every parent, child
lease and diagnostic. Later graphics calls perform no cache, translation or
shared-memory access. CLOSE preserves the existing free-list response draining;
reopening remains unavailable until verified resource disposal exists.

## Sources and focused verification

Record initialization and reconstruction follow the pinned
[board HWRT driver](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/gpu/imagination/pvr_hwrt.c)
and its free-list implementation. Geometry sizing follows
[Mesa's render-target allocation](https://gitlab.freedesktop.org/mesa/mesa/-/blob/86c7176d2dd0065555521bf8c61f208bf53c75d6/src/imagination/vulkan/pvr_arch_job_render.c).
The implementation preserves the applicable MIT notices.

The [reference check](../checks/pvr-render-target.py) compiles source initializers
and compares complete record bytes and allocation requirements. The
[native composition](../checks/gpu-graphics-memory.py) uses actual firmware,
compute, graphics, VM, allocator and FreeRTOS service implementations. Its power
provider refuses activation after inspecting completed publication. Separate
cases exercise capacity refusal and a late final publication step. Existing
firmware startup, numerical compute and free-list protocol evidence is reused.

The source comparison and all three native cases pass. The native composition
uses the updated bounded-extent allocator with compact reservations. Expanded
reservation admission retains its separate capacity evidence; this check does
not qualify the larger physical range. Executed inputs, native objects, tool
identities and preserved executables are recorded under
`~/.cache/sunstoneos/evidence/gpu-graphics-memory-20261017/` in
`final-inputs.json`, `reference4/` and `native2/`.

Remaining work includes a real render context and geometry/fragment command
rings, context-switch and vertex/fragment programs, executable commands, color
output ownership and acquired output validation. Physical firmware startup,
external request blocking, complete transaction drain and resource disposal
remain prerequisites for qualification. Resource preparation alone establishes
none of those conditions.
