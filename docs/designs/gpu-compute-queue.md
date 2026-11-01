# GPU compute queue

Status: **design; not implemented and not reviewed.** Prototype headers:
[`include/gpu_queue.h`](../../include/gpu_queue.h) (application interface),
[`include/pvr_gpu_component.h`](../../include/pvr_gpu_component.h) (worker
function table) and
[`apps/mimus/gpu_linear_job.h`](../../apps/mimus/gpu_linear_job.h) (the job
mimus passes). They compile with `clang -std=c11 -Wall -Wextra -Wpedantic`
for the host and for `aarch64-none-elf`; nothing calls them.

The GPU compute queue is the OS service through which any application runs
compute programs on the A733 GPU. An application creates a context, loads
programs compiled by `apps/gpu-program/compile.py`, leases buffers into the
context's GPU address space, submits jobs and waits for completion records.
It replaces both compute paths that exist today:

- **The mimus session** (`drivers/gpu/pvr_mimus_service.c`,
  `drivers/gpu/pvr_compute_mimus.c`, `platform/a733/gpu_mimus.c`) runs one
  linked kernel over a fixed buffer map and accepts one job record shape,
  `{x_offset, ordinal, rows, x_stride}`, against a tensor table built into
  the session (`include/pvr_mimus_job.h:11-14`,
  `include/pvr_compute_mimus.h:71-80`). While it is configured, the GPU
  service refuses the compute service, graphics and free lists
  (`drivers/gpu/pvr_gpu_service.c:108-109`).
- **The qualification service** (`drivers/gpu/pvr_compute_service.c`) runs
  one job fixed at bind time, the built-in numerical program or one compiled
  package, with at most 4,096 bytes of copied input, and returns at most
  3,968 bytes of output, 32 words per reply
  (`include/pvr_compute_package.h:7-8`, `include/pvr_gpu_service.h:185`).

Finding 6 of [the layering audit](../layering-audit.md) classes both as
defects that block a second application. The completion test of that audit
applies here: an image classifier with a GPU preprocessing kernel must use
the queue by adding files under its own `apps/<name>/` directory only.

## The interface has five objects

| Object | What it is | Lifetime |
| --- | --- | --- |
| Context | One firmware compute context, one GPU address space, one client ring, one timeline word and one completion-record queue | `gpu_queue_context_create` to `gpu_queue_close` |
| Program | USC code copied into two program pages of the context | Load to context close |
| Lease | A buffer of the device-memory pool, or a whole region declared in the application's manifest, mapped into the context's GPU address space | `gpu_queue_lease` to `gpu_queue_release` or context close |
| Job | Program index, up to 8 bindings, a grid, up to 192 argument bytes and a sequence number | Submit to its record |
| Record | 16 bytes: sequence, result, detail, kick-to-done time | Posted once per accepted job |

The interface is in `include/gpu_queue.h`. It is plain C without FreeRTOS
types, so host checks and applications share it. Every call runs in the
context's client task, which the image binds to a GPU service client slot
before the scheduler starts, as `pvr_gpu_service_bind` binds the recognizer
today (`apps/os/gpu.c:203`).

### Lifecycle calls use the worker's request queue

`gpu_queue_context_create`, `gpu_queue_program_load`, `gpu_queue_lease`,
`gpu_queue_release` and `gpu_queue_close` travel through the existing ticket
path of the GPU service (`pvr_gpu_service_send_epoch` and
`pvr_gpu_service_collect`, `include/pvr_gpu_service.h:335-340`). They are
rare: a recognizer creates one context, loads one program and maps three
leases once per boot. Each takes `wait_ticks` for the reply. A reply that
arrives after the wait stays with the client's pending ticket, and the next
lifecycle call on that context collects it before sending.

### Submit and wait bypass the request queue

The per-job path works as the mimus session does, which reaches the kick in
one worker step (`docs/designs/gpu-mimus-compute-measurements.md`,
"Worker steps per job"):

1. `gpu_queue_submit` checks the fields that need no worker state: counts,
   program index range, binding lease indices, argument length and nonzero
   group counts. It copies the 352-byte job into the next free entry of the
   context's submission ring, publishes the entry with a release store and
   wakes the worker. A full ring returns BUSY; a parked device returns
   PARKED. Neither queues anything.
2. The worker copies the entry into its own storage and checks only that
   copy, as `take` does today (`drivers/gpu/pvr_mimus_service.c:222-223`).
3. `gpu_queue_wait` blocks in `xQueueReceive` on the context's record queue,
   which holds `GPU_QUEUE_DEPTH` records. The worker posts a record after
   the completion interrupt, so the client wakes without polling.

### A job binds leases in place

A binding names a lease, the byte offset in it that the program receives as
a GPU address, and a byte range for cache maintenance
(`struct gpu_queue_binding`). The queue copies no buffer data. Maintenance
follows the lease's CPU access class and touches only the declared range:

| Lease class | Before the kick | After the timeline update |
| --- | --- | --- |
| CPU read-write, GPU read | Clean the range to the point of coherency | None |
| CPU read, GPU read-write | None | Clean and invalidate the range (`DC CIVAC`; `DC IVAC` faults on a read-only mapping, `docs/designs/gpu-recognition.md:235`) |
| CPU read, GPU read | None | None |
| CPU none, either GPU access | None | None |

CPU read-write leases cannot also be GPU-writable: a CPU store between the
clean and the invalidate would be written back over GPU output. A range of
zero bytes skips maintenance, for data unchanged since the lease was mapped
(mimus's model files). This is the same work the mimus session does today:
it cleans the job's code extent before the kick and cleans and invalidates
the job's tags and sums after the timeline
(`drivers/gpu/pvr_compute_mimus.c:152-157,172-176`).

The range limits maintenance, not access. The GPU MMU confines a program to
the leases mapped in its context; a program can read any byte of any lease
in its context, and write any byte of a GPU-writable one.

### Programs keep the compiler's two-pointer register layout

Profile 3 of `compile.py` admits programs with two 64-bit pointer arguments
in unified registers 0-3, the flat local index in unified register 4, the
workgroup IDs in coefficients 0-2 and the group counts in coefficients 4-6
(`include/pvr_compute_package.h:20-25`, `apps/gpu-program/compile.py:161-167`).
The queue keeps that layout and assigns the two pointers:

- **Unified registers 0-1:** the job's argument block. It holds the GPU
  addresses of bindings 1 to n-1 as 64-bit words, then the job's argument
  bytes.
- **Unified registers 2-3:** the GPU address of binding 0.

This is the layout the mimus kernel already reads: a 32-byte record with the
weight and code addresses first and four shape words after them, and the
output region in the second pointer (`shaders/pvr_mimus_linear.cl:39-42,96`).
The kernel binary therefore does not change.

A program descriptor (`struct gpu_queue_program`) carries the code, its
length (at most 8,128 bytes, the limit of `compile.py --session`), the
compiler's temporary count, coefficient count and local size, and the three
digests the compiler manifest records. The queue checks the descriptor's
shape at load. It cannot check the code's memory behaviour; programs remain
trusted, and source admission remains an offline review step
(`include/pvr_compute_package.h:26-31`).

### The worker repeats every check on its own copy

| Check | Where | Refusal |
| --- | --- | --- |
| Context open, slot generation current | Submit, worker | REFUSED, `CLOSED` |
| Program index loaded | Submit, worker | REFUSED, `PROGRAM` |
| 1 to 8 bindings, each lease mapped in this context | Submit, worker | REFUSED, `BINDING` |
| Binding address and maintenance range inside the lease | Worker | REFUSED, `RANGE` |
| Job access within the lease's GPU access; GPU write only on CPU-read leases | Worker | REFUSED, `PERMISSION` |
| Sequence above the context's last accepted sequence and below 2^32 - 1 | Worker | REFUSED, `SEQUENCE` |
| Each group count nonzero | Submit, worker | REFUSED, `GRID` |
| Argument bytes at most 192 | Submit, worker | REFUSED, `ARGUMENTS` |
| `not_after_us` not passed at the kick | Worker | REFUSED, `DEADLINE` |

All checks are compares against the context's lease table, which is fixed
while the context has jobs in flight. No check translates an address,
hashes a buffer or takes a lock. A refused job produces a REFUSED record in
order with the others and nothing reaches the client ring.

### REFUSED and FAULT reach the application, which decides

The queue never computes a job on the CPU. Results follow section 1.8 of
[the performance architecture](../performance-architecture.md):

- **REFUSED:** nothing reached the device. The context stays usable. The
  application may run the work on the CPU; mimus does so today when the
  session refuses a job (`apps/os/gpu_hook.cc:42-46`).
- **PARKED:** the OS has closed the device for thermal throttling or image
  return. Nothing was queued; the context survives the close and accepts
  jobs again after reopen.
- **FAULT:** a hang past the context's job bound, a firmware context-reset
  event, an unexpected timeline value, a failed cache operation or a
  transport fault. The worker cannot prove the device state, so the service
  fails and retains every resource, as it does today
  (`drivers/gpu/pvr_gpu_service.c:172-192`). Every owed record of every
  context arrives as FAULT, and every later call returns FAULT.

### Each context has a hang bound, and each job may have a kick deadline

- **Context job bound:** `job_us`, 1 µs to 1 s, from the kick to the timeline
  update. The mimus session uses the same rule and ceiling
  (`include/pvr_mimus_service.h:38-40,58-60`). The worker's wait while a job
  is in flight ends at the earliest such deadline.
- **Job kick deadline:** `not_after_us`, optional. A job still queued at that
  time, for example during a performance hold, is refused with `DEADLINE`.
  A recognizer with a fixed service window uses it to bound GPU work it
  would otherwise wait for.
- **Lifecycle deadlines:** each lifecycle request carries its own bound, as
  OPEN and CLOSE do today (`include/pvr_gpu_service.h:175`).

### Contexts, mappings, jobs and fences follow the upstream PowerVR driver

| Upstream (`linux-orangepi` `2ac08e8c7`, `bsp/drivers/gpu/imagination`) | Queue |
| --- | --- |
| `DRM_IOCTL_PVR_CREATE_CONTEXT` with a VM context handle and priority (`include/pvr_drm.h:764`, `pvr_context.c:280-311`) | `gpu_queue_context_create`; one address space per context, priority reserved |
| GEM object mapped by `DRM_IOCTL_PVR_VM_MAP` (`include/pvr_drm.h:645`, `pvr_vm.c:735`), MMU flush on change (`pvr_mmu.c:124`) | `gpu_queue_lease`; tables rebuilt while the context is idle, then an MMU cache flush |
| `struct drm_pvr_job` with a caller-built command stream (`include/pvr_drm.h:1229`) | `struct gpu_queue_job`; the worker builds the CDM command from the program and grid, so applications never author firmware commands |
| Done fence signalled when the queue's timeline UFO reaches the job's seqno, signed compare (`pvr_queue.c:942-962`) | Record posted when the context's timeline word reaches the job's sequence |
| Sync objects between jobs | None; jobs of one context run in order, and no application needs cross-context ordering |

This design draws on the upstream driver source only and on no
information from the firmware binary.

## The worker hosts registered components through a function table

The GPU service worker keeps the firmware, the kernel command queue, power,
the clock, the interrupt, the lifecycle and the request queue. Each
component owns its firmware contexts, their client rings and their jobs. A
component registers before `pvr_gpu_service_init` with an operations table
(`include/pvr_gpu_component.h`):

| Operation | Called | Contract |
| --- | --- | --- |
| `objects` | OPEN, before power | Firmware objects for the MIPS table |
| `prepare` | OPEN, before power | Context bytes, program pages, timeline words |
| `publish` | PUBLISHING phase | One cache or table page per call |
| `poll` | READY, after firmware events | Take queued jobs, kick, observe timelines, post records |
| `event` | Each drained firmware event | 0 for malformed or a reset naming the component |
| `abort` | Service failure | Refuse submits, post FAULT for owed records |
| `close` | Before CLOSE | Seal when nothing is queued, in flight or owed |
| `deadline` | Each worker wait | Earliest deadline, progress value, flags |
| `hold`, `release` | Performance hold | Stop and restart kicking; queued jobs stay |
| `reopen` | After healthy STOPPED | Rebuild mutable firmware bytes, restore timelines |

Up to four components register. The firmware's 19 resources and every
component's objects must fit the 64 entries of the MIPS table
(`include/pvr_fw_state.h:13`, `include/pvr_mips.h:7`). The queue component
reserves 6 objects per context slot, 24 for four slots, so 43 entries are
used and the render service's two still fit. Objects join the table at OPEN
because the firmware table has no live update
(`include/pvr_compute_service.h`, comment above `pvr_compute_service_admit_mode`).

### Each mimus branch becomes a loop over components

| Site in `drivers/gpu/pvr_gpu_service.c` | Today | With components |
| --- | --- | --- |
| 12-23 | Weak references to the mimus functions | Removed; registration links the table |
| 108-109 | mimus refuses compute, graphics and free lists | Removed; registration checks storage and object counts |
| 181, 229 | mimus abort and event | Every component's `abort`, `event` |
| 286-291 | Core-mode check when compute or mimus is configured | When any component has `PVR_GPU_COMPONENT_COMPUTE` |
| 311-318, 559-567 | mimus prepare and publish | Every component's `prepare`, then `publish` until all return 1 |
| 353-358 | Copies the mimus objects into the MIPS table | Every component's `objects` |
| 424-432, 980-983, 966 | mimus submitting state and progress | Every component's `deadline` flags and progress |
| 491-523 | HOLD claims the mimus slot; RELEASE frees it | Every component's `hold`, then `release` |
| 608-611 | mimus poll in READY | Every component's `poll` |
| 804-812 | Reopen only for mimus | Every component's `reopen` |
| 923 | mimus close | Every component's `close` |
| 1027 | mimus job deadline | Earliest component deadline |
| 1102-1104 | Wake ticket `PVR_GPU_SERVICE_MIMUS_WAKE` | Wake ticket 1 + component index |
| 1171, 1180-1184 | Init: the core-mode callback is required with compute or mimus; performance requires mimus | Core-mode callback required with any compute component; performance requires at least one component, and no render or free lists |

The render service, the free lists and the qualification service keep their
direct hooks during migration. The render path is outside this design; it
can take the component form later through the same table.

### Wakes are coalesced so their sends cannot fail

Each component has a `wake` flag. A client sends one wake message only when
the flag was clear, and the worker clears it before it polls that
component, so a submit after the clear sends again. The request queue grows
from 8 messages to 8 + 1 + 4: one per client (each client has one pending
request, `include/pvr_gpu_service.h:234`), one interrupt wake (already
coalesced by `irq_wake_pending`, `drivers/gpu/pvr_gpu_service.c:150-153`) and
one per component. A wake send therefore never finds the queue full. Today
a failed mimus wake send takes the job back and refuses it
(`drivers/gpu/pvr_mimus_service.c:380-386`); that refusal path disappears.

### The performance hold and DVFS no longer name mimus

Today a HOLD action waits until the mimus slot is idle and swaps it to HELD,
and a client that finds HELD retries every tick
(`drivers/gpu/pvr_gpu_service.c:492-501`, `apps/os/gpu.c:482-487`). With
components:

1. HOLD calls every component's `hold` on each worker step. A component stops
   kicking and returns 1 once nothing of it is in flight. Jobs submitted
   during the hold are accepted into their rings and stay queued.
2. When every component returned 1, the worker starts
   `pvr_gpu_clock_hold`, as now.
3. RATE and VOLTAGE run only while held; they already name no component.
4. RELEASE resumes the clock and then calls every component's `release`,
   which kicks the queued jobs.

The client sees a later record instead of a HELD result, so the tick retry
loop goes away. A queued job whose `not_after_us` passes during the hold
gets a REFUSED record at that time, because the worker's wait includes the
earliest queued deadline (`PVR_GPU_COMPONENT_QUEUED`). The hold covers
compute only, so the service still refuses performance actions while render
or free lists are configured.

### Device OPEN and CLOSE belong to the OS GPU runtime

Applications never send OPEN or CLOSE. The OS GPU runtime, the L1 successor
of the lifecycle half of `apps/os/gpu.c`, owns the device session on its own
client slot:

- the first context creation in a COLD or healthy STOPPED device runs the
  existing OPEN sequence before the reply;
- thermal throttling calls `close` on every component and then CLOSE; jobs
  submitted while closed return PARKED;
- return to normal reopens the device; each component's `reopen` restores
  its contexts, and each timeline restarts at its last sequence, as
  `pvr_mimus_service_prepare` does today
  (`drivers/gpu/pvr_mimus_service.c:158-161`);
- the image's return path closes and parks the device as `a733_gpu_mimus_return`
  does today (`apps/os/gpu.c:438-469`).

The healthy-reopen rule (`drivers/gpu/pvr_gpu_service.c:800-812`) becomes
generic: reopen is allowed when every component's `reopen` returns 1.

## The queue component owns four context slots

`drivers/gpu/pvr_compute_queue.c` is the first registered component. A
context slot holds:

- **Firmware objects:** 6 uncached pages, as the mimus session uses today:
  compute context, register state, client-ring control, client ring
  (1 KiB), memory context and timeline (`platform/a733/gpu_mimus.c:16`).
- **Payload:** 21 write-back pages: one job page, two program pages for the
  first program and 18 pages of context-switch programs
  (`include/a733_gpu_mimus.h:11-13`, `platform/a733/gpu_mimus.c:100-127`).
  Each further program takes two more pages.
- **GPU tables:** at most 32 write-back pages from the planner
  (`pvr_gpu_vm_require`); mimus's mappings need 24
  (`include/a733_gpu_mimus.h:17-20`).
- **CPU-side state:** the submission ring of 4 jobs (1,408 bytes), the
  worker's copy of the job being admitted, the lease table and a record queue
  of 4 entries.

### Four job slots on one job page allow four jobs in flight

The client ring transport allows one KICK outstanding at a time, but a KICK
completes when the firmware accepts it, before the GPU runs the work
(`include/pvr_cccb.h`, comments of `pvr_cccb_begin` and `pvr_cccb_poll`).
The mimus session nevertheless runs one job at a time, because every job
reuses the same job words and output region. The queue gives each job its
own 1 KiB slot of the job page:

| Bytes | Contents | Written |
| --- | --- | --- |
| 0-127 | PDS data: task word, group counts, the two pointer constants | Per job |
| 128-191 | PDS dispatch program | At context creation |
| 192-447 | Argument block: binding addresses, then argument bytes | Per job |
| 448-511 | CDM stream: the grid kernel, the fence kernel, the terminate word | Per job |
| 512-575 | Fence PDS program | At context creation |

The per-job bytes are contiguous in bytes 0-511, so one clean of 8 lines
covers them. The 96-byte firmware compute command of each slot points at the
slot's CDM stream and is built once per slot at context creation with
`pvr_compute_job_command` (`include/pvr_compute_job.h:64-65`). Per job the
worker writes the PDS data with `pvr_compute_job_grid_pds`, the argument
block, the CDM stream with `pvr_compute_job_grid_cdm` and the 8-byte timeline
UPDATE (`include/pvr_compute_job.h:66-70`). A slot is reused only after the
timeline passes the job that last used it.

### Completion handles every finished job per interrupt

On each READY step after an interrupt, the component reads each active
context's timeline word, one uncached load. Every in-flight job whose
sequence is at or below the timeline completes in order: the worker cleans
and invalidates its GPU-written ranges, checks the job bound once more, and
posts its record. A timeline value that matches neither the last completed
sequence nor an in-flight one is FAULT `TIMELINE`, the rule the mimus session
applies to its single job (`drivers/gpu/pvr_mimus_service.c:270-282`).

### Context close cleans the firmware context before releasing memory

`gpu_queue_close` seals the submission ring, waits for every record, then
sends the firmware common-context cleanup command and waits for status 1,
as the qualification service does at CLOSE
(`include/pvr_compute_service.h`, `pvr_compute_service_cleanup_begin`).
Only then does the worker reclaim the context's GPU tables
(`a733_gpu_vm_memory_reclaim_begin`, `include/a733_gpu_vm_memory.h:52-58`)
and return every lease. The mimus session has no such step because it closes
only with the whole device.

## Memory and MMU mapping become per lease

### CPU access comes from the region that backs the lease

The CPU translation tables do not change after boot
([device-memory capacity](../device-memory-capacity.md), "One call prepares
both pools"), and no runtime CPU remapping exists. A lease therefore takes
its CPU access from the memory region that backs it. The region table that
finding 9 of the layering audit calls for declares, per GPU-shareable
region: base, size (2 MiB multiples), CPU access class and an optional
clear-at-boot prefix. The platform builds the CPU tables from those entries:

| Today | With the region table |
| --- | --- |
| `platform/a733-smp-cached/memory.c:109-117` makes level-2 entries 54 and 55 (`0x46c00000`-`0x47000000`) read-only at EL1 and EL0 under `A733_GPU_OUTPUT_EL0_READ_ONLY` | The loop over region entries sets AP = 0b11 for each CPU-read region. AArch64 has no encoding that is writable at EL1 and read-only at EL0, so a CPU-read region is read-only at both |
| `apps/os/identity.c:34-47` zeroes the 64 KiB tag area at EL3 with translation off, under `SUNSTONE_A733_OS_GPU_LINEAR` | The EL3 identity step zeroes and publishes the clear-at-boot prefix of every region that declares one. mimus declares 64 KiB at the start of its output region |
| `platform/a733/gpu_mimus.c:74` refuses any arena or output address other than the two constants | No address check against application constants; a lease is a region entry or a pool allocation |

Each GPU-shareable region backs one device-memory pool of class DMA32, which
aligns requests of 2 MiB or more to 2 MiB and of 64 KiB or more to 64 KiB
(`include/device_memory_pool.h:12-17`). A pool lease of `bytes` comes from
the pool of the requested CPU class. A request with a nonzero `region`
adopts that whole manifest region without copying, which is how the host-
loaded model files become a lease. The 768 MiB extended write-back
reservation denies EL0 access (`docs/device-memory-capacity.md:51-60`), so it
backs only CPU-none leases.

### GPU virtual address equals physical address

Every data mapping uses the lease's physical address as its GPU virtual
address. The arena and output region are mapped this way today
(`include/a733_gpu_mimus.h:24-25`); only the two model files are moved, to
`0x43000000` from `0x44000000`, so that they sit in adjacent 8 MiB regions
(`platform/a733/gpu_mimus.c:105-111`). Physical addresses lie below 2^36, and
the firmware PDS, USC and general heaps start at `0xda00000000`,
`0xe000000000` and `0x8000000000` (`platform/a733/gpu_mimus.c:19`), so data
addresses never collide with internal mappings. No GPU address allocator is
needed, and a program's pointers equal the application's CPU pointers.

A lease maps with 2 MiB pages when its base and size are 2 MiB multiples,
else with 64 KiB pages, else 4 KiB. One 2 MiB GPU region uses one page size
(`include/pvr_gpu_vm.h:12-17`), which the pool's class alignment satisfies.

### Each context has its own address space, rebuilt only while idle

A context's mapping list is its internal pages (job page, program pages,
context-switch programs, at their fixed heap addresses) plus one span per
mapped lease, sorted by address. Mapping or releasing a lease:

1. is accepted only while the context has no job queued or in flight;
2. rebuilds the context's tables in place, within its 32-page budget, with
   `pvr_gpu_vm_build`, so the root page and the firmware memory-context
   record keep their addresses;
3. cleans the table pages (`a733_gpu_vm_memory_publish_begin` and `_poll`);
4. if the context has been kicked in this firmware session, issues one MMU
   cache flush at catalogue level and waits for its completion
   (`include/pvr_gpu_flush.h`, `pvr_gpu_flush_init` and `pvr_gpu_flush_poll`);
5. only after the flush, for a release, returns the lease to its pool.

Mappings made before the first kick need no flush, because no GPU MMU cache
holds entries for that address space. A context's mapping changes never
touch another context's tables, so other contexts keep running. The
upstream driver also flushes MMU caches after a VM map change
(`pvr_mmu.c:2299`).

The mimus layout fits this scheme without change of size: the 2 MiB
mappings of the models, arena and output region and the 4 KiB internal
pages need the same 24 table pages as now.

### Resource budget for four contexts

| Resource | One context | Four contexts | Source of the limit |
| --- | --- | --- | --- |
| MIPS firmware objects | 6 | 24 (43 with the firmware's 19) | 64 entries |
| Uncached pages | 6 | 24 | Firmware pool |
| Write-back payload pages | 21 | 84 | GPU write-back pool |
| GPU table pages | at most 32 | at most 128 | GPU write-back pool |
| Pool allocation records per lease pool | 1 per lease | at most 32 leases | `DEVICE_MEMORY_MAX_ALLOCATIONS` |

Today the GPU holds 79 write-back pages (34 firmware, 21 payload, 24 table)
and 51 uncached pages for one session
(`docs/designs/gpu-mimus-compute-measurements.md`, "Pool pages for the
GPU"). Four slots need about 250 write-back and 70 uncached pages, so the
GPU's internal pools move to the extended reservations.

## The interrupt route follows the worker core

The GPU worker and INTID 95 share one image configuration value, the GPU
worker core. Today both use the recognizer's core: the route
`{95, OS_RECOGNITION_CORE, 0xd8, a733_gpu_mimus_irq}`
(`apps/os/irq.c:28`), the worker affinity `1 << core` with
`core = OS_RECOGNITION_CORE` (`apps/os/gpu.c:153,584`), and the link check's
comparison with `os_recognition_core` (`apps/os/dispatch.c:189`).

With the queue, image composition sets `A733_GPU_WORKER_CORE`. By default it
is the core of the first GPU client task in the manifests, so that client
wakes without an inter-processor interrupt. The worker's affinity mask and
the GPU entry of the interrupt table both use it, and the link check
compares the route's core with the worker's affinity mask. The table's
dispatcher already refuses delivery on any other core
(`platform/a733-smp-cached/irq_table.c:31`). The worker's affinity is fixed
when the task is created, so the route never needs to move at run time. A
client on another core receives its record through FreeRTOS's cross-core
yield, at the cost of one inter-processor interrupt per wake.

## mimus becomes an ordinary client

### What moves to `apps/mimus`

| Today | Moves to | Change |
| --- | --- | --- |
| `shaders/pvr_mimus_linear.cl`, `drivers/gpu/pvr_compute_mimus_usc.c` | `apps/mimus/gpu_linear.cl` and a program descriptor generated by `compile.py` | Binary unchanged |
| Tensor admission and job rows (`drivers/gpu/pvr_compute_mimus.c:17-28,106-120`) | `apps/mimus/gpu_linear.c` | Checks against the model lease instead of `PVR_MIMUS_MODEL_GPU` |
| Tag geometry and tag check (`include/pvr_compute_mimus.h:124-139`) | `apps/mimus/gpu_linear_job.h` | Same arithmetic |
| Placement table generator `apps/os/placement.py` and its generated header | `apps/mimus/placement.py` | Unchanged |
| `apps/os/dispatch.c`, `apps/os/dispatch.h` | `apps/mimus/dispatch.*` | Fixed addresses (`dispatch.h:146-157`) become manifest regions; `OS_GPU_CLIENT` and owner constants become manifest identities |
| Engine hook `apps/os/gpu_hook.cc`, including the CPU fallback and the sum check | `apps/mimus/gpu_hook.cc` | Calls `gpu_queue_*` |
| Tensor table, session configuration and recognizer lifecycle in `apps/os/gpu.c` | `apps/mimus/gpu.c` | Context creation, three leases, one program |

These are deleted once mimus uses the queue: `pvr_mimus_service.c/.h`,
`pvr_mimus_job.h`, `pvr_compute_mimus.c/.h`, `platform/a733/gpu_mimus.c`,
`include/a733_gpu_mimus.h`, the `mimus` and `mimus_owner` configuration
fields, `PVR_GPU_SERVICE_MIMUS_WAKE` and the build option
`SUNSTONE_A733_OS_GPU_LINEAR`, which becomes `SUNSTONE_A733_OS_GPU` selected
by any manifest that uses the GPU.

### Setup at the start of recognition

1. `gpu_queue_context_create` with `job_us` = 1,000,000, the current
   `OS_GPU_MAX_JOB_US` (`apps/os/gpu.c:34`).
2. `gpu_queue_program_load` with the linear-layer descriptor.
3. Three leases, each adopting a manifest region:
   - output region, 2 MiB, CPU read and GPU read-write, 64 KiB cleared at
     boot;
   - model files, 16 MiB at their load address, CPU read and GPU read;
   - arena, 12 MiB, CPU read-write and GPU read.
4. Tensor admission over the placement table, as `admit_tensors` does now
   (`apps/os/gpu.c:108-122`), with each weight extent checked against the
   model lease.

### The job record mimus passes

`mimus_gpu_linear_job` in `apps/mimus/gpu_linear_job.h` fills this record
for one slice of `rows` activation rows:

| Field | Value |
| --- | --- |
| `sequence` | The recognizer's next sequence, rising through the boot |
| `program` | The linear-layer program index |
| `groups` | `{⌈n_gpu / 16⌉, ⌈rows / 32⌉, 1}` |
| Binding 0, output | Output lease, GPU read-write, address 0; maintenance `[65536 - 4·tags, 65536 + sums)` |
| Binding 1, weights | Model lease, GPU read, address = weight row 0 of the tensor; no maintenance |
| Binding 2, codes | Arena lease, GPU read, address = `x_offset`; maintenance = the code extent rounded to 64-byte lines |
| Arguments, 16 bytes | K in bits 0-15 and n_gpu in bits 16-31; `rows`; weight stride in bits 0-15 and code stride in bits 16-31; `sequence` |

Here `tags` = ⌈n_gpu / 16⌉ × ⌈rows / 32⌉ × 32 words and `sums` =
`rows × n_gpu × (K > 512 ? 8 : 4)` bytes. The kernel sees its current
32-byte record: the weight address, the code address, then the four shape
words.

### What mimus keeps

mimus keeps every policy that the OS carries for it today:

- one job in flight per call, because its jobs share the output region;
- the sequence and tag check of each OK record;
- the comparison of every sum with a CPU sum;
- CPU computation of refused jobs;
- the end of the boot on FAULT;
- the CPU path while the device is closed for thermal throttling, which the
  queue reports as PARKED.

The queue's REFUSED record takes the place of the session's REFUSED record
(`include/pvr_mimus_job.h:15-20`), and FAULT takes the place of LOST.

## Performance

### Hot paths and their rates

- **Submit and complete,** once per job: 204 jobs per bench recording for
  mimus (`docs/designs/gpu-mimus-compute-measurements.md`).
- **Worker wait,** once per idle period.
- **Lease mapping, program load and context creation,** a few times per
  boot.

### Per-job cost compared with the mimus session

The host model counts these operations per job of the mimus session
(`docs/designs/gpu-mimus-compute-measurements.md`, results table and "Other
host results"): 1 worker step to the kick, 57 transport and 6 maintenance
`DSB SY`, 6 queue operations, 25 critical sections and 27 translations in
the backend's register callbacks. The queue keeps the transport, the kernel command queue, the
backend and the interrupt path, so those counts stay. Its differences:

| Item | mimus session | Queue | Change per job |
| --- | --- | --- | --- |
| Job description copies | 24-byte request, twice | 352-byte job, twice | About 90 more 8-byte loads and stores |
| Admission | At most 13 compares | About 10 compares per binding: about 35 for mimus | About 25 more compares |
| PDS data | Written once at session open | 128 bytes per job | 16 more 8-byte stores |
| Job-word clean | 128 bytes, one call | 512 bytes, one call | 6 more lines, no more `DSB SY` |
| Data maintenance | Code extent; tags and sums | The same ranges, declared by mimus | None |
| Client-ring writes | 184 bytes as single-byte volatile stores (`drivers/gpu/pvr_mimus_service.c:45-47`) | 23 aligned 8-byte stores | 161 fewer uncached stores |
| Wake messages | One send per job; a failed send refuses the job | One send per idle-to-busy transition | At most the same |
| Completion | One timeline load, one record | One timeline load per active context, one record per job | One load per extra context |

The estimate uses 20 cycles per cache line, the time model's assumption
(`docs/designs/gpu-mimus-compute.md:289`), and one cycle per store or
compare. The additions then come to about 250 cycles per job. Allowing four
times that for cache misses on the job copy and mispredicted branches gives
at most 1,000 cycles: 0.5 µs on an A76 at 2,002 MHz and 2 µs on an A55 at
494 MHz.

The board measured these kick-to-timeline times per job
(`docs/designs/gpu-job-session.md:212-223`): 80 to 83 µs, 87 µs for the
first job after the client ring wrapped, and 113 µs for the first job after
an MMU cache flush. That time covers firmware scheduling, the work and the
timeline write, and none of the submit path. The queue's addition is
therefore at most 2.5% of the smallest measured job time. The 161 removed
uncached stores reduce it further by an amount this design does not
estimate.

With up to four jobs in flight per context, the CPU work of submitting job
n + 1 overlaps the GPU's execution of job n. An application with independent
outputs gains from this. mimus keeps one job in flight because its jobs share
the output region, so its timing does not change.

### Budgets

| Metric ([performance architecture](../performance-architecture.md), GPU) | Baseline (mimus session) | Budget |
| --- | --- | --- |
| Submit to kick, worker idle | 1 worker step, no tick wait | 1 worker step, no tick wait, at most one context switch |
| Client wake-ups per job | 1 record | 1 record |
| Worker wake-ups per job beyond interrupts | 1 submit wake | At most 1 |
| Queue round trips to deliver a result | 0 (in place) | 0 (in place) |
| Copies per input or result byte | 0 | 0 |
| `DSB SY` per job, transport and maintenance | 57 and 6 | At most 57 and 6 |
| Translations per job | 27, all in the backend | At most 27; none added |
| Uncached ring stores per job | 184 | At most 23 |
| Added CPU time per job, A76 at 2,002 MHz | Not applicable | At most 1 µs |
| Hang detection | 1 s plus at most one 1 ms tick | The context's `job_us` plus at most one tick |

### Heavier forms used

- **Per-job admission compares** (performance architecture section 1.1):
  every compared field changes per job, because the application writes the
  job.
- **A copy of the job description** (section 1.6): the worker must not read
  fields the client can still change after the checks. Buffer data is never
  copied.
- **An MMU cache flush per mapping change** (section 1.5): required because
  the context's tables changed; it runs only at lease and release.

### Polling that remains

The worker waits for the earliest job deadline while a job is in flight. It
caps that wait at 4 ms only when the image has no interrupt delivery
(`irq.mask` NULL), so a lost interrupt shows up as a FAULT at the job bound
instead of being covered by a periodic wake. Today the 4 ms cap applies in
every image (`drivers/gpu/pvr_gpu_service.c:1034-1037`). The worker keeps its
one-tick wait while a masked event batch remains, because that batch is
already known to be pending (`drivers/gpu/pvr_gpu_service.c:1016`).

### Failure classes

| Failure | Class (performance architecture 1.8) | Retained |
| --- | --- | --- |
| Malformed or inadmissible job | Refused before device access | Nothing; context usable |
| Kick deadline passed while queued | Refused before device access | Nothing |
| Context close with cleanup status 1 | Known terminal state | Nothing; leases return to pools |
| Device CLOSE with acknowledged stop | Known terminal state | Context leases and tables, for reopen |
| Hang, reset event, timeline mismatch, failed cache operation, transport fault | State unknown | Every resource of every context; service FAILED |

### Measurement

- **Host:** a new `checks/gpu-queue.py` replays the bench recording's 204
  jobs through the queue in the existing GPU service host model and counts,
  per job, worker steps, `DSB SY`, queue operations, critical sections,
  translations, cache lines and uncached stores. The same replay through the
  mimus session, as `checks/pvr-mimus-service.py` runs it, gives the
  baseline. The engine model must produce bit-identical outputs through
  both.
- **Board, as a coordinator request:** the eight-job firmware session
  through the queue, recording kick-to-timeline per job on the 24 MHz
  counter, and submit-to-kick and timeline-to-record on the same counter.
  The prediction is 80 to 83 µs kick-to-timeline per job, 113 µs for the
  first, and at most 1 µs of added CPU time per job on an A76.

## Migration: ordered commits

The commits that edit `drivers/gpu`, `include/pvr_*` or the `apps/os` GPU
files start after the GPU bisection branch merges. Each commit leaves every
image building and its host checks passing.

1. **Register components.** Add `pvr_gpu_service_register` and the loops of
   the component table to `pvr_gpu_service.c`; wrap the mimus session as the
   first component with an adapter. Behaviour unchanged; the host counts of
   `checks/pvr-mimus-service.py` are identical.
2. **Generic performance hold.** HOLD and RELEASE call component `hold` and
   `release`; the init check stops requiring mimus.
3. **Generic reopen.** `a733_gpu_mimus_reopen` becomes the mimus component's
   `reopen`; the service's reopen rule calls every component.
4. **Remove the exclusion.** Delete the weak references and the mimus
   exclusion in `allocations()`; check firmware-object totals across
   components; size the request queue at 8 + 1 + 4 and coalesce wakes.
5. **Worker-core route.** One `A733_GPU_WORKER_CORE` value for the worker
   affinity and the INTID 95 route; the link check compares them.
6. **Region table for GPU-shareable regions.** Entries for the current mimus
   regions at their current addresses, with CPU access class and the
   clear-at-boot prefix; `memory.c` and the EL3 identity step use them. The
   generated CPU tables are identical to today's.
7. **Queue component, one context, depth 1.** `drivers/gpu/pvr_compute_queue.c`
   and `include/gpu_queue.h`: context creation, program load, region
   adoption, a fixed map built at creation, submit and complete.
   `checks/gpu-queue.py` replays the bench jobs.
8. **Depth 4 and 8-byte ring stores.** Four job slots; aligned ring writes.
9. **Four context slots and context close.** Firmware context cleanup and
   table reclaim at close.
10. **Lease map and release on an idle context,** with the MMU cache flush.
11. **Pool leases.** `bytes` leases from the region pools.
12. **Program descriptors from `compile.py`.** A `compile --profile 3
    --queue-program` output that writes `struct gpu_queue_program`.
13. **mimus on the queue.** Add `apps/mimus/gpu_linear.c` and move the hook,
    the dispatcher, the placement generator and the tensor table into
    `apps/mimus`. The engine model and the QEMU session cases pass with
    bit-identical outputs.
14. **Board request** through the coordinator: the eight-job session and the
    exact-sum caller through the queue.
15. **Remove the mimus session** and `SUNSTONE_A733_OS_GPU_LINEAR`.
16. **Remove the qualification service.** The qualification images load a
    profile 3 program into a queue context; `COMPUTE_INPUT`,
    `COMPUTE_OUTPUT` and the 4,096 and 3,968-byte package limits go.

## Risks

- **Completion interrupts are unqualified on the board.** Physical GPU
  interrupt behaviour remains unqualified (`docs/hardware.md:1684`). Without
  the 4 ms cap, a lost interrupt turns into a FAULT at the job bound.
- **Several firmware compute contexts have never run together.** Every board
  run so far used one compute context. CDM context switching, which uses the
  context-switch programs, is unexercised.
- **Rebuilding tables of a context that has run jobs is unproven.** The MMU
  cache flush has run on the board only before the first job of a session.
- **One fault stops every application.** The service has no per-context
  recovery, so one application's faulting program ends GPU use for all.
- **GPU read-only mappings are not shown to be enforced.** The board has not
  shown that the GPU MMU refuses a store through a read-only mapping
  (`apps/os/dispatch.c:228-230`). Until it does, the queue cannot rely on a
  read-only GPU mapping to protect a lease.
- **Programs are trusted.** A loaded program can reach every lease of its
  context; the queue checks descriptors only.
- **CPU access is fixed per region.** An application that needs a different
  CPU access for a buffer needs a different region in its manifest.
- **Pool records limit leases.** Each pool has 32 allocation records.
- **Pool growth.** Four context slots need about three times the GPU's
  current internal pool pages, which places them in the extended
  reservations whose board qualification is outstanding
  (`docs/device-memory-capacity.md:1-8`).
- **mimus regressions.** Sequence continuity across thermal close and
  reopen, the tag area's clear at boot and the recognizer's timing move to
  new code; the engine model and the QEMU session cases must cover them
  before a board run.

## Open decisions

1. **Address-space sharing.** One address space per context (proposed), or
   one address space shared by the contexts of one application, as upstream
   allows with a VM context handle.
2. **The 4 ms wait cap.** Remove it when interrupt delivery is configured
   (proposed), or keep it until GPU interrupt delivery is board-qualified.
3. **CPU read-write leases that the GPU writes.** Refused (proposed), or
   allowed for applications that promise not to write during a job.
4. **Context priority.** Reserved (proposed), or exposed as the firmware
   context priority.
5. **Per-context fault recovery.** Whole-service failure (proposed), or
   containment through firmware context-reset events, as upstream recovers
   hardware faults.
6. **Shared context-switch programs.** One copy per context (proposed), or
   one copy of their read-only pages mapped into every context, saving
   part of the 18 pages per context.
7. **Device OPEN timing.** At the first context creation (proposed), or at
   boot when any manifest uses the GPU.
8. **Pointer arguments.** Binding 0 in the second pointer and the others in
   the argument block (proposed, and the mimus kernel's layout), or every
   binding in the argument block.
9. **Capacities.** Four contexts, four jobs in flight, eight bindings and 192
   argument bytes per job.
10. **Zero fill.** Whether pool leases are zeroed at lease time, which costs
    one write of every line of the lease.
