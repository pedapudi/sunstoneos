# GPU compute queue

Status: **implemented and host-tested; physical queue qualification pending.**

The application interface is [`include/gpu_queue.h`](../../include/gpu_queue.h).
The service worker uses
[`include/pvr_compute_queue.h`](../../include/pvr_compute_queue.h), and the A733
constructor uses [`platform/a733/gpu_queue.c`](../../platform/a733/gpu_queue.c).
`SUNSTONE_A733_OS_GPU` selects the service. The external mimus application and
[`apps/queue-add`](../../apps/queue-add/README.md) use the same application kit.
The reviewed migration comparisons below describe the separation base
`cc8a7957`; their legacy paths identify that historical tree.

## Implementation evidence and departures from the reviewed plan


Review corrections cover zero-job shutdown and cold return, SLC bypass for
CPU/GPU-shared mappings, rounded lease isolation, program-source bounds and
application-supplied startup initialization. Program copies are confined to
the immutable `sunstone_gpu_program` section admitted from application objects.
The manifest and runtime independently refuse lease tails that intersect
another region, including an unleased one. The correction evidence is under
`review-fixes/` in the cache directory below.

The host queue model passes 33 service cases and an external 204-job replay
with 5,870,398 checked sums. Native instruction models pass 16 thermal cases,
10 public-client cases, 17 returning-image cases, 32 constructor-refusal cases
and output-permission checks on two applications. The VM check covers four
admitted page sizes and 36 refusals. It compares tables with the pinned driver.
The ring trace records 23 aligned stores per job, plus five stores when a
padding record wraps the ring. No buffer data is copied.

Raw commands, input snapshots, image hashes and results are under
`~/.cache/sunstoneos/build/gpu-queue-1031/`. The `checks/` subdirectory holds
queue and native fixtures; `program/` holds compiler and vector checks;
`mimus-client/` holds application arithmetic and recorded-recognition evidence.
These models establish software behavior. They do not establish physical GPU
interrupt delivery, performance or recovery.

- **Page size:** BRN 71317 makes 1 MiB and 2 MiB pages unsafe on this GPU.
  The queue maps data with 256 KiB pages, and the shared A733 planner refuses
  larger pages. Lease alignment remains 2 MiB. The mimus map still needs
  24 table pages because table allocation follows 2 MiB virtual regions.
- **CPU storage:** the concrete queue occupies 2,240 bytes on AArch64. The
  prototype's 1,216-byte estimate omitted retained native buffer descriptors,
  creation tickets and the static completion queue. Device allocations remain
  79 write-back pages and 51 uncached pages for the mimus layout.
- **Protected return files:** the build keeps the private preprocessor name
  `SUNSTONE_A733_OS_GPU_LINEAR` for `apps/os/cpu-return.c` and
  `apps/os/cpu-return/terminal.patch`. Their separate owner must rename those
  guards. It is not a CMake option. The compatibility option and application
  GPU session are removed.
- **Physical migration step:** the coordinator receives the requests below.
  Board operations were prohibited, so legacy removal follows host evidence.
  Physical qualification and the independent feature review remain open.
- **Build selection:** the generic option lands with runtime ownership in
  migration commit 3, so independent applications can select the service.
  Migration commit 7 removes the legacy sources and worker configuration.
- **Host followup scope:** the OS check validates native retirement captures
  and the generic decoder. Compound transport, backup and restoration checks
  belong to the external application launcher.

## Coordinator board requests

Both ports remain quarantined. These commands require the coordinator's
recovery procedure, the generic launcher integration and matching prepared
session templates. A captured descriptor alone does not authorize a launch.
The GPU power, clock, voltage and firmware-boot sequence is unchanged from the
base. Only context construction and job publication change.

The eight-job vector image uses `QUEUE_ADD_JOBS=8`; the default example uses
one job. Configure it with the cached `cfg.sh`, `MIMUS_DIR=`,
`SUNSTONE_APPLICATION=queue-add`, `SUNSTONE_A733_OS=ON`,
`SUNSTONE_A733_OS_GPU=ON`, `SUNSTONE_A733_OS_CPU_RETURN=ON`,
`SUNSTONE_A733_OS_CPU_CLOCK=ON`, `A733_A76_ADMITTED=ON` and
`CMAKE_C_FLAGS=-DQUEUE_ADD_JOBS=8`. Build target
`sunstone-a733-os-queue-add` in the directory below.

```sh
os=/home/sunil/git/sunstoneos-worktrees/gpu-queue-1031
b=$HOME/.cache/sunstoneos/build/gpu-queue-1031
p=$HOME/.cache/sunstoneos/venv/bin/python
"$p" "$os/tools/dev-run.py" gpu-return --port 3-1 \
  --image "$b/queue-eight-return/sunstone-a733-os-queue-add.elf" \
  --host-tree "$os" --template "$b/board-templates/queue-eight" \
  --evidence "$b/board-runs/queue-eight"
"$p" "$os/tools/dev-run.py" gpu-return --port 3-1 \
  --image "$b/gpu-return2/sunstone-a733-os.elf" \
  --host-tree "$os" --template "$b/board-templates/exact-sum" \
  --evidence "$b/board-runs/exact-sum"
```

The vector result must contain eight jobs, 65 elements, checksum 8,840,
sequence 8 and eight completion times. Every job rotates its inputs and checks
all 65 outputs. Require acknowledged GPU shutdown, suspended worker, normal ROM
return and restoration of every saved RAM span. Compare kick-to-timeline
measurements with the recorded 80–83 microsecond steady-state range and
113 microsecond first-job observation. A changed result needs investigation;
host timing does not predict physical interrupt latency.

The exact-sum run must produce the reference transcript and checksums, validate
every GPU sum and tag, park the GPU and return normally. Submit-to-kick and
timeline-to-record counter instrumentation remains a coordinator measurement
request; the application record exposes kick-to-completion time only.

The GPU compute queue is the OS service through which an application runs a
compute program on the A733 GPU. The application creates one context with
one program from `apps/gpu-program/compile.py` and up to four leases of its
own manifest regions, submits one job at a time and waits for that job's
completion record. The queue replaces the mimus session
(`drivers/gpu/pvr_mimus_service.c`, `drivers/gpu/pvr_compute_mimus.c`,
`platform/a733/gpu_mimus.c`). That session runs one linked kernel over a
fixed buffer map. It accepts one job record, `{x_offset, ordinal, rows,
x_stride}`, against a tensor table built into the session
(`include/pvr_mimus_job.h:11-14`, `include/pvr_compute_mimus.h:71-80`).

Finding 6 of [the layering audit](../layering-audit.md) classes the mimus
session and the qualification service as defects that block a second
application. The completion test of that audit applies here: an image
classifier with a GPU preprocessing kernel must use the queue by adding
files under its own `apps/<name>/` directory only.

The qualification service (`drivers/gpu/pvr_compute_service.c`), with its
4,096-byte input and 3,968-byte output limits
(`include/pvr_compute_package.h:7-8`), stays as it is. Moving its images onto
the queue is a later change.

## The first version holds one context with one job

| Capacity | Value | Why |
| --- | --- | --- |
| Contexts | 1 | One GPU application per image |
| Jobs in flight per context | 1 | mimus jobs share the output region; the client ring allows one KICK outstanding (`include/pvr_cccb.h`, `pvr_cccb_begin`) |
| Programs per context | 1 | mimus has one kernel |
| Leases per context | 4 | mimus maps its output region, two model files and its arena |
| Bindings per job | 4 | mimus binds three ranges |
| Argument bytes per job | 64 | mimus passes 16 |
| Job record | 160 bytes | 32-byte header, 4 bindings of 16 bytes, 64 argument bytes |

Each limit is a constant in `include/gpu_queue.h`. A second application that
needs more raises the constant, and for more contexts or a deeper queue adds
the per-context and per-slot storage that these limits leave out.

## The application interface

The interface is in `include/gpu_queue.h`. It is plain C without FreeRTOS
types, so host checks and applications share it. The calls run in the
application's GPU client task, which the image binds to a GPU service client
slot before the scheduler starts, as `pvr_gpu_service_bind` binds the
recognizer today (`apps/os/gpu.c:203`). The calls follow the existing pattern
for calls that are safe at EL0. The caller makes an atomic store or load in
shared memory, and the EL1 worker does the device work
([application interface](app-abi.md), "Applications call the OS through
plain functions that are safe at EL0").

### Creation opens the device, maps the leases and loads the program

`gpu_queue_context_create` takes one `struct gpu_queue_context_config`:
the job bound `job_us`, the program descriptor and up to four lease requests.
It travels through the existing ticket path of the GPU service
(`pvr_gpu_service_send_epoch` and `pvr_gpu_service_collect`,
`include/pvr_gpu_service.h:335-340`) and waits at most `wait_ticks` for the
reply. A reply that arrives after the wait stays with the client's pending
ticket, and the next call collects it.

The worker, with the device COLD:

1. checks the program descriptor and every lease request against the
   application's region table (see "Memory and MMU");
2. copies the program into the context's program pages;
3. plans the mappings, acquires the planned GPU table pages and builds the
   context's GPU address space once (see "The map is built once");
4. cleans and invalidates each GPU-written lease once;
5. builds the firmware compute context, as `a733_gpu_mimus_prepare` does for
   the mimus session;
6. runs the existing OPEN sequence and replies when the device is READY.

A context is created once per boot. `gpu_queue_close` refuses new jobs, waits
for an owed record and closes the device. It is final: every page stays
retained, as the GPU service retains them today
(`drivers/gpu/pvr_gpu_service.c:172-192`), and the context cannot be created
again in that boot.

### Submit and wait bypass the request queue

The per-job path works as the mimus session does, which reaches the kick in
one worker step (`docs/designs/gpu-mimus-compute-measurements.md`,
"Worker steps per job"):

1. `gpu_queue_submit` checks the fields that need no worker state: the
   binding count, the binding lease indices, the argument length and nonzero
   group counts. It copies the 160-byte job into the context's slot and
   wakes the worker with the handshake in "The wake cannot be lost". A slot
   that still holds an unanswered job returns BUSY; a closed device returns
   PARKED. Neither queues anything.
2. The worker copies the slot into its own storage and checks only that
   copy, as the session's `take` does today
   (`drivers/gpu/pvr_mimus_service.c:222-223`).
3. `gpu_queue_wait` blocks in `xQueueReceive` on the context's record queue
   of one entry. The worker posts the record after the completion interrupt,
   so the client wakes without polling.

### A job binds leases in place

A binding names a lease, the byte offset in it that the program receives as
a GPU address, and a byte range for cache maintenance
(`struct gpu_queue_binding`). The queue copies no buffer data. Maintenance
follows the access bits of the lease's region and touches only the declared
range, rounded out to 64-byte lines:

| Region access | At creation | Before the kick | After the timeline update |
| --- | --- | --- | --- |
| CPU read and write, GPU read | None | Clean the range to the point of coherency | None |
| CPU read, GPU write | Clean and invalidate the whole lease | None | Clean and invalidate the range (`DC CIVAC`) |
| CPU read, GPU read | None | None | None |

The rules come from how the regions are mapped and written:

- **CPU read and write, GPU read.** This is mimus's arena. The CPU writes it
  through a write-back mapping, so each job's range is cleaned before the
  kick.
- **CPU read, GPU write.** This is mimus's output region. Its CPU mapping is
  read-only at EL1 and EL0, because AArch64 has no access encoding that is
  writable at EL1 and read-only at EL0
  (`platform/a733-smp-cached/memory.c:109-117`). No CPU store reaches it, but
  reads leave clean lines. The clean and invalidate at creation removes lines
  from boot-time accesses. The one after each job removes lines that the CPU
  read before the GPU wrote. `DC IVAC` would fault on the read-only mapping
  (`docs/designs/gpu-recognition.md:235`), so both use `DC CIVAC`, which
  needs read permission only.
- **CPU read, GPU read.** These are INPUT regions, such as mimus's model
  files. The EL3 publication step cleans them before the OS runs, from the
  generated region table of the [application interface](app-abi.md), and
  nothing writes them afterwards.

Creation refuses a region with both CPU write and GPU write: a CPU store
between the clean and the invalidate would be written back over GPU output.
It also refuses a region without CPU read, because `DC CIVAC` needs it. A
range of zero bytes skips maintenance.

This is the work the mimus session does today. It cleans the job's code
extent before the kick, and it cleans and invalidates the job's tags and sums
after the timeline (`drivers/gpu/pvr_compute_mimus.c:152-157,172-176`).

The range limits maintenance only. The GPU MMU confines a program to the
leases of its context; a program can read any byte of any lease, and write
any byte of a GPU-writable one.

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

The mimus kernel already reads this layout: a 32-byte record with the weight
and code addresses first and four shape words after them, and the output
region in the second pointer (`shaders/pvr_mimus_linear.cl:39-42,96`). The
kernel binary therefore does not change.

A program descriptor (`struct gpu_queue_program`) carries the code, its
length (at most 8,128 bytes, the limit of `compile.py --session`), the
compiler's temporary count, coefficient count and local size, and the three
digests that the compiler manifest records. The queue checks the
descriptor's fields at creation. It cannot check the code's memory
behaviour. Programs remain trusted, and source admission remains an offline
review step (`include/pvr_compute_package.h:26-31`).

### The worker repeats every check on its own copy

| Check | Where | Refusal |
| --- | --- | --- |
| Context open | Submit, worker | REFUSED, `CLOSED` |
| 1 to 4 bindings, each lease index below the lease count | Submit, worker | REFUSED, `BINDING` |
| Binding address and maintenance range inside the lease | Worker | REFUSED, `RANGE` |
| Sequence above the context's last sequence and below 2^32 - 1 | Worker | REFUSED, `SEQUENCE` |
| Each group count nonzero | Submit, worker | REFUSED, `GRID` |
| Argument bytes at most 64, and the argument block at most 88 bytes | Submit, worker | REFUSED, `ARGUMENTS` |
| `not_after_us` not passed at the kick | Worker | REFUSED, `DEADLINE` |

Lease access needs no per-job check, because creation already refused the
combinations that maintenance cannot serve. Every check compares against the
context's lease table, which never changes after creation. No check
translates an address, hashes a buffer or takes a lock. A refused job gets a
REFUSED record, and nothing reaches the client ring.

### REFUSED, PARKED and FAULT reach the application, which decides

The queue never computes a job on the CPU. Results follow section 1.8 of
[the performance architecture](../performance-architecture.md):

- **REFUSED:** nothing reached the device. The context stays usable. The
  application may run the work on the CPU; mimus does so today when the
  session refuses a job (`apps/os/gpu_hook.cc:42-46`).
- **PARKED:** the OS has closed the device for thermal throttling or image
  return. Nothing was queued. The context accepts jobs again after the OS
  reopens the device.
- **FAULT:** a hang past `job_us`, a firmware context-reset event, an
  unexpected timeline value, a failed cache operation or a transport fault.
  The worker cannot prove the device state, so the whole service fails and
  retains every resource, as it does today
  (`drivers/gpu/pvr_gpu_service.c:172-192`). An owed record arrives as FAULT,
  and every later call returns FAULT.

### Each context has a hang bound, and each job may have a kick deadline

- **Job bound:** `job_us`, 1 µs to 1 s, from the kick to the timeline
  update. The mimus session uses the same rule and ceiling
  (`include/pvr_mimus_service.h:38-40,58-60`).
- **Kick deadline:** `not_after_us`, optional. A job still in the slot at
  that time, for example during a performance hold, gets a REFUSED record
  with `DEADLINE`. A recognizer with a fixed service window uses it to bound
  GPU work it would otherwise wait for.
- **Creation and close:** each takes `wait_ticks`, as the OPEN and CLOSE
  requests do today (`include/pvr_gpu_service.h:175`).

### The objects follow the upstream PowerVR driver

| Upstream (`linux-orangepi` `2ac08e8c7`, `bsp/drivers/gpu/imagination`) | Queue |
| --- | --- |
| `DRM_IOCTL_PVR_CREATE_CONTEXT` with a VM context handle and priority (`include/pvr_drm.h:764`, `pvr_context.c:280-311`) | `gpu_queue_context_create`; one address space per context; no priority |
| GEM object mapped by `DRM_IOCTL_PVR_VM_MAP` (`include/pvr_drm.h:645`, `pvr_vm.c:735`), MMU flush on change (`pvr_mmu.c:124`) | Leases mapped once at creation; the map never changes, so no flush follows |
| `struct drm_pvr_job` with a caller-built command stream (`include/pvr_drm.h:1229`) | `struct gpu_queue_job`; the worker builds the CDM command from the program and grid, so applications never write firmware commands |
| Done fence signalled when the queue's timeline UFO reaches the job's seqno, signed compare (`pvr_queue.c:942-962`) | Record posted when the context's timeline word reaches the job's sequence |

This design uses the upstream driver source only, and no information from
the firmware binary.

## The queue runs inside the GPU service worker

The GPU service worker keeps the firmware, the kernel command queue, power,
the clock, the interrupt, the lifecycle and the request queue. The queue
(`drivers/gpu/pvr_compute_queue.c`, declared in
`include/pvr_compute_queue.h`) owns the context's firmware objects, its
client ring, its job page and its slot. The worker calls the queue directly,
at each place where it calls the mimus session today.

### Each mimus call site becomes a queue call

| Site in `drivers/gpu/pvr_gpu_service.c` | Today | With the queue |
| --- | --- | --- |
| 12-23 | Weak references to the mimus functions | Weak references to the queue functions |
| 108-109 | mimus refuses compute, graphics and free lists | The queue refuses the same three |
| 181, 229 | mimus abort and event | `pvr_compute_queue_abort`, `_event` |
| 286-291 | Core-mode check when compute or mimus is configured | When compute or the queue is configured |
| 311-318, 559-567 | mimus prepare and publish | `pvr_compute_queue_prepare`, then `_publish` until it returns 1 |
| 353-358 | Copies the mimus objects into the MIPS table | `pvr_compute_queue_objects` |
| 424-432, 980-983, 966 | mimus submitting state and progress | `pvr_compute_queue_submitting`, `_progress` |
| 491-523 | HOLD claims the mimus slot; RELEASE frees it | `pvr_compute_queue_hold`, `_release` |
| 608-611 | mimus poll in READY | `pvr_compute_queue_poll` |
| 804-812 | Reopen only for mimus | `pvr_compute_queue_reopen` |
| 923 | mimus close | `pvr_compute_queue_close` |
| 1027 | mimus job deadline | `pvr_compute_queue_deadline` |
| 1102-1104 | Wake ticket `PVR_GPU_SERVICE_MIMUS_WAKE` | The queue's wake ticket |
| 1171, 1180-1184 | Init: the core-mode callback is required with compute or mimus; performance requires mimus | The same rules with the queue in place of mimus |

The queue's functions have the signatures that a component function table
would hold, so a second component later turns each of these calls into a
loop over a table without changing the queue.

The queue uses 6 firmware objects, as the mimus session does: compute
context, register state, client-ring control, client ring (1 KiB), memory
context and timeline (`platform/a733/gpu_mimus.c:16`). With the firmware's
19 resources they use 25 of the 64 entries of the MIPS table
(`include/pvr_fw_state.h:13`, `include/pvr_mips.h:7`). They join the table at
OPEN, because the firmware table has no live update.

### The job page is written once at creation

The context has one job page in the PDS heap:

| Bytes | Contents | Written |
| --- | --- | --- |
| 0-127 | PDS data: task word, group counts, the two pointer constants | At creation; the binding-0 address and the group counts again only when they change |
| 128-191 | PDS dispatch program | At creation |
| 192-255 | Fence PDS program | At creation |
| 256-315 | CDM stream: the grid kernel, the fence kernel, the terminate word | Per job |
| 320-407 | Argument block: binding addresses, then argument bytes | Per job |

The worker writes the PDS data with `pvr_compute_job_grid_pds` at creation
(`include/pvr_compute_job.h:55-72`). A program that reads no group counts,
such as mimus's, keeps zero counts there, as the mimus session does
(`drivers/gpu/pvr_compute_mimus.c:84-89`); the CDM stream carries each job's
grid. Per job the worker writes the CDM stream with
`pvr_compute_job_grid_cdm`, the argument block and the 8-byte timeline
UPDATE. It rewrites the binding-0 pointer only when the address differs from
the previous job's. It rewrites the group counts only for a program with
seven coefficients, and only when they change. mimus's binding 0 is always the start of its output lease, so
its PDS data never changes after creation.

The per-job bytes are contiguous from byte 256, so one clean covers them: 2
lines for mimus's 32-byte argument block, as the session cleans 128 bytes
today (`include/pvr_compute_mimus.h:36-47`), and at most 3. The 96-byte
firmware compute command points at the CDM stream and is built once at
creation with `pvr_compute_job_command`.

### Completion reads one timeline word

On each READY step after an interrupt, the queue reads the context's
timeline word, one uncached load. When it equals the job's sequence, the
worker cleans and invalidates the job's GPU-written ranges, checks the job
bound once more and posts the record. A timeline value that matches neither
the previous sequence nor the job's is FAULT `TIMELINE`, the rule the mimus
session applies today (`drivers/gpu/pvr_mimus_service.c:270-282`).

### The wake cannot be lost

The slot state, the lost flag and the wake flag are atomics, and every
access to them on both sides is sequentially consistent
(`include/pvr_compute_queue.h`, comment above `pvr_compute_queue_wake`):

1. The client writes the job into the slot, then stores SUBMITTED to the
   slot state.
2. The client exchanges the wake flag with 1. If the old value was 0, it
   sends one wake message to the worker's request queue.
3. The worker, on that message, stores 0 to the wake flag, then loads the
   slot state. If the state is SUBMITTED, it takes the job.

All these operations fall in one total order. Either the worker's clear
precedes the client's exchange, which then reads 0 and sends, or the
client's store precedes the worker's load, which then sees SUBMITTED. In
both cases the worker takes the job, so it can wait with `portMAX_DELAY`
when idle. It already does so whenever interrupt delivery is configured
(`drivers/gpu/pvr_gpu_service.c:1034`).

The request queue holds `PVR_GPU_SERVICE_CLIENTS` messages today
(`drivers/gpu/pvr_gpu_service.c:1188`), one per client, since each client has
one pending request (`include/pvr_gpu_service.h:234`). The interrupt wake is
coalesced by `irq_wake_pending` (`drivers/gpu/pvr_gpu_service.c:150-153`).
The queue grows to `PVR_GPU_SERVICE_CLIENTS + 2`, one more for the interrupt
wake and one for the queue's wake. A wake send therefore never finds the
queue full. Today a failed mimus wake send takes the job back and refuses it
(`drivers/gpu/pvr_mimus_service.c:380-386`); that path disappears.

### The performance hold no longer names mimus

Today a HOLD action waits until the mimus slot is idle and swaps it to HELD,
and a client that finds HELD retries every tick
(`drivers/gpu/pvr_gpu_service.c:492-501`, `apps/os/gpu.c:482-487`). With the
queue:

1. HOLD calls `pvr_compute_queue_hold` on each worker step. The queue stops
   kicking and returns 1 once nothing is in flight. A job submitted during
   the hold stays in the slot.
2. When the queue returned 1, the worker starts `pvr_gpu_clock_hold`, as
   now. RATE and VOLTAGE run only while held; they name no client.
3. RELEASE resumes the clock and then calls `pvr_compute_queue_release`,
   which kicks the waiting job.

The client sees a later record instead of a HELD result, so the tick retry
loop goes away. A job whose `not_after_us` passes during the hold gets a
REFUSED record at that time, because the worker's wait includes that
deadline.

### Device OPEN and CLOSE belong to the OS GPU runtime

Applications never send OPEN or CLOSE. The OS GPU runtime, the successor of
the lifecycle half of `apps/os/gpu.c`, owns the device session on its own
client slot:

- the context creation runs the OPEN sequence before its reply;
- thermal throttling calls `pvr_compute_queue_close` and then CLOSE; jobs
  submitted while closed return PARKED;
- return to normal reopens the device. `pvr_compute_queue_reopen` restores
  the firmware memory-context record and the firmware context, as
  `a733_gpu_mimus_reopen` does (`platform/a733/gpu_mimus.c:190-197`). The
  timeline restarts at the last sequence, as `pvr_mimus_service_prepare`
  does (`drivers/gpu/pvr_mimus_service.c:158-161`). The GPU tables stay
  published and unchanged;
- the image's return path closes and parks the device, as
  `a733_gpu_mimus_return` does today (`apps/os/gpu.c:438-469`).

The healthy-reopen rule (`drivers/gpu/pvr_gpu_service.c:800-812`) calls the
queue in place of the mimus session.

## Memory and MMU

### Leases adopt manifest regions

The region table belongs to the [application interface](app-abi.md). Its
commit 7, "Generated tables", gives each image a table with the address,
size, kind and access of every region in the application's manifest
(`struct ss_region`, `include/sunstone_app.h`). Virtual address equals
physical address, and unpinned regions are placed on 2 MiB boundaries. The
platform's CPU tables, the EL3 publication of INPUT regions and the read-only
CPU mapping of GPU-written regions come from that design. The queue adds no
region table and no CPU mapping rule.

A lease request names a region index and a byte count. The count is a
multiple of 2 MiB, at most the region's size rounded up to 2 MiB; zero takes
the whole rounded region. The rounded tail lies inside the region's 2 MiB
placement, so it belongs to the same application. mimus's four leases:

| Lease | Region | Address | Region bytes | Lease bytes | Access |
| --- | --- | --- | --- | --- | --- |
| 0, output | `gpu_output` | `0x46c00000` | 4 MiB | 2 MiB | read, gpu-write |
| 1, base model | `base_model` | `0x44000000` | 7,501,968 | 8 MiB | read, gpu-read |
| 2, overlay model | `overlay_model` | `0x44800000` | 6,560,592 | 8 MiB | read, gpu-read |
| 3, arena | `arena` | `0x46000000` | 12 MiB | 12 MiB | read, write, gpu-read |

The region values come from `apps/mimus-board/manifest.json` on the
application-interface branch. The output lease maps 2 MiB of the 4 MiB
region, as the session maps `PVR_MIMUS_OUTPUT_BYTES` today
(`include/pvr_compute_mimus.h:27`). The device-memory pools back no lease.

### GPU virtual address equals physical address

Every lease maps at its physical address. The arena and output region are
mapped this way today (`include/a733_gpu_mimus.h:24-25`). The two model
files move from GPU address `0x43000000`, where the session places them
(`include/pvr_compute_mimus.h:21`), to their own addresses `0x44000000` and
`0x44800000`. Physical addresses lie below 2^36, and the firmware PDS, USC
and general heaps start at `0xda00000000`, `0xe000000000` and
`0x8000000000` (`platform/a733/gpu_mimus.c:19`), so data addresses never
collide with internal mappings. A program's pointers equal the application's
CPU pointers, and no GPU address allocator is needed.

Every lease maps with 256 KiB GPU pages. BRN 71317 excludes the hardware
encodings for 1 MiB and 2 MiB pages. One 2 MiB GPU region uses one page size
(`include/pvr_gpu_vm.h:12-17`).

### The map is built once

`pvr_gpu_vm_build` zeroes the firmware memory-context record and writes the
catalogue marker `UINT32_MAX` into it (`drivers/gpu/pvr_gpu_vm.c:133-135`).
It also requires the table extent to be the planned size
(`drivers/gpu/pvr_gpu_vm.c:85-87`). A rebuild while the firmware holds the
context would therefore reset a record the firmware owns. The queue builds
the map once, at creation, while the device is COLD:

1. The mapping list is the four leases plus the 4 KiB internal pages (job
   page, program pages and context-switch programs) at their fixed heap
   addresses, sorted by address.
2. `pvr_gpu_vm_require` plans the tables. The worker acquires the planned
   table bytes from the GPU write-back pool, at most 32 pages, as
   `a733_gpu_mimus_prepare` does (`platform/a733/gpu_mimus.c:133-136`). An
   exhausted pool refuses the creation before any device access.
3. `pvr_gpu_vm_build` writes the tables and the memory-context record;
   `a733_gpu_vm_memory_publish_begin` and `_poll` clean the table pages.

The map then never changes. Reopen after a thermal close restores the
memory-context record from the saved root page without rebuilding the tables,
as `a733_gpu_mimus_reopen` does (`platform/a733/gpu_mimus.c:190-195`). No
MMU cache flush is needed, because no mapping changes after the first kick.

The mimus mappings keep today's table size. The session maps two 8 MiB
model spans, a 12 MiB arena and a 2 MiB output region with 256 KiB pages, all
inside one 1 GiB catalogue entry (`platform/a733/gpu_mimus.c:105-111`). The
queue maps the same spans at different addresses in the same catalogue entry,
so it needs the same 15 data leaves and the planner's 24 table pages
(`include/a733_gpu_mimus.h:17-20`). The host check of the queue commit
confirms the count.

### Pool pages acquired through write-back mappings

`acquire` in `platform/a733/gpu_mimus.c:40-47` clears each new extent with
`memset` through the EL1 write-back mapping. The queue's table, payload and
firmware pages come from the same pools in the same way, and the existing
publication steps clean them before the GPU reads them. No lease comes from
a pool, so no application buffer is cleared through a write-back alias.

### Resource budget equals today's

| Resource | mimus session | Queue, mimus context |
| --- | --- | --- |
| MIPS firmware objects | 6 | 6 |
| Uncached firmware pages | 6 | 6 |
| Write-back payload pages | 21 (`include/a733_gpu_mimus.h:11-13`) | 21 |
| GPU table pages | 24 | 24, at most 32 for another application |
| CPU-side state | Session, slot and record queue | `struct pvr_compute_queue`, 2,240 bytes on AArch64, including a record queue of one entry |

Today the GPU holds 79 write-back pages (34 firmware, 21 payload, 24 table)
and 51 uncached pages for one session
(`docs/designs/gpu-mimus-compute-measurements.md`, "Pool pages for the
GPU"). The queue keeps those numbers for mimus.

## The interrupt route follows the application's main core

The application-interface design generates the interrupt routes: GPU 95
goes to the main core of the application that the session grants the GPU
(the generated interrupt routes of the [application interface](app-abi.md)).
The queue needs
one thing in addition. The worker task's affinity mask must name the same
core, because the interrupt table's dispatcher refuses delivery on any
other core (`platform/a733-smp-cached/irq_table.c:31`). Today both use
`OS_RECOGNITION_CORE`: the route `{95, OS_RECOGNITION_CORE, 0xd8,
a733_gpu_mimus_irq}` (`apps/os/irq.c:28`) and the worker affinity
(`apps/os/gpu.c:153,584`). With the queue, the OS GPU runtime creates the
worker with the generated route's core, and the image link check compares
the two, as `apps/os/dispatch.c:189` compares the route with
`os_recognition_core` today.

## mimus becomes an ordinary client

### Where the mimus code goes

The application-interface design moves the GPU glue into mimus
([application interface](app-abi.md), commit 5, "GPU glue into mimus"):
`dispatch.c`, `gpu_hook.cc` and `placement.py`. After that commit the queue
replaces the session calls in that code:

| Today | With the queue |
| --- | --- |
| `shaders/pvr_mimus_linear.cl`, `drivers/gpu/pvr_compute_mimus_usc.c` | `apps/mimus/gpu_linear.cl` and the program descriptor that `compile.py` generates; binary unchanged |
| Tensor admission and job rows (`drivers/gpu/pvr_compute_mimus.c:17-28,106-120`) | `apps/mimus/gpu_linear.c`; checks against the model leases in place of `PVR_MIMUS_MODEL_GPU` |
| Tag geometry and tag check (`include/pvr_compute_mimus.h:124-139`) | `apps/mimus/gpu_linear_job.h`; same arithmetic |
| Engine hook, including the CPU fallback and the sum check (`apps/os/gpu_hook.cc`) | Calls `gpu_queue_submit` and `gpu_queue_wait` |
| Tensor table, session configuration and recognizer lifecycle in `apps/os/gpu.c` | Context creation in `apps/mimus/gpu_linear.c`; device lifecycle in the OS GPU runtime |

### Setup at the start of recognition

1. `mimus_gpu_context` fills the configuration: `job_us` = 1,000,000, the
   current `OS_GPU_MAX_JOB_US` (`apps/os/gpu.c:34`), the linear-layer
   program and the four leases in the table above.
2. `gpu_queue_context_create` opens the device and returns the lease
   addresses.
3. Tensor admission over the placement table, as `admit_tensors` does now
   (`apps/os/gpu.c:108-122`), with each weight extent checked against its
   model lease.

### The job record mimus passes

`mimus_gpu_linear_job` in `apps/mimus/gpu_linear_job.h` fills this record
for one slice of `rows` activation rows:

| Field | Value |
| --- | --- |
| `sequence` | The recognizer's next sequence, rising through the boot |
| `groups` | `{⌈n_gpu / 16⌉, ⌈rows / 32⌉, 1}` |
| Binding 0, output | Lease 0, address 0; maintenance `[65536 - 4·tags, 65536 + sums)` |
| Binding 1, weights | Lease 1 or 2, the tensor's model file; address = weight row 0 of the tensor; no maintenance |
| Binding 2, codes | Lease 3; address = `x_offset`; maintenance = the code extent rounded to 64-byte lines |
| Arguments, 16 bytes | K in bits 0-15 and n_gpu in bits 16-31; `rows`; weight stride in bits 0-15 and code stride in bits 16-31; `sequence` |

Here `tags` = ⌈n_gpu / 16⌉ × ⌈rows / 32⌉ × 32 words and `sums` =
`rows × n_gpu × (K > 512 ? 8 : 4)` bytes. The kernel sees its current
32-byte record: the weight address, the code address, then the four shape
words.

### What mimus keeps

mimus keeps every policy that the OS carries for it today:

- one job in flight;
- the sequence and tag check of each OK record;
- the comparison of every sum with a CPU sum;
- CPU computation of refused jobs;
- the end of the boot on FAULT;
- the CPU path while the device is closed for thermal throttling, which the
  queue reports as PARKED.

The queue's REFUSED record takes the place of the session's REFUSED record
(`include/pvr_mimus_job.h:15-20`), and FAULT takes the place of LOST.

### The application registers startup initialization

An application may place `ss_region_initializer` records in the immutable
`sunstone_region_initializers` section. A record names a manifest region and a
prefix length. EL3 validates every record before the first store, clears each
prefix with aligned 64-bit stores and publishes it before CPU read-only
mappings take effect. Prefixes must fit CPU-readable, GPU-written WORK regions
that the CPU cannot write. An absent registration clears nothing.

Mimus registers its GPU output region and 64 KiB tag prefix in
`sunstoneos/apps/mimus/gpu_linear.c`. The OS contains neither that output
address nor its tag geometry. The image check verifies that registration data
comes from application objects and remains immutable.

## Performance

### Hot paths and their rates

- **Submit and complete,** once per job: 204 jobs per bench recording for
  mimus (`docs/designs/gpu-mimus-compute-measurements.md`).
- **Worker wait,** once per idle period.
- **Context creation,** once per boot.

### Per-job cost compared with the mimus session

The host model counts these operations per job of the mimus session
(`docs/designs/gpu-mimus-compute-measurements.md`, results table and "Other
host results"): 1 worker step to the kick, 57 transport and 6 maintenance
`DSB SY`, 6 queue operations, 25 critical sections and 27 translations in
the backend's register callbacks. The queue keeps the transport, the kernel
command queue, the backend and the interrupt path, so those counts stay.

The first migration commit replaces the session's 184 single-byte ring
stores per job (`drivers/gpu/pvr_mimus_service.c:45-47`) with 23 aligned
8-byte stores, so the queue starts from that baseline. Its differences from
the session after that commit:

| Item | mimus session | Queue | Change per job |
| --- | --- | --- | --- |
| Job description copies | 24-byte request, twice | 160-byte job, twice | About 70 more 8-byte loads and stores |
| Admission | At most 13 compares | About 25 compares | About 12 more compares |
| PDS data | Written at session open | Written at creation; 2 compares per job | 2 compares |
| Argument block and CDM stream | 32 and 60 bytes | 32 and 60 bytes | None |
| Job-page clean | 2 lines, one call | 2 lines, one call | None |
| Data maintenance | Code extent; tags and sums | The same ranges, declared by mimus | None |
| Client-ring writes | 23 aligned 8-byte stores | 23 aligned 8-byte stores | None |
| Wake messages | One send per job; a failed send refuses the job | One send per job; the send cannot fail | None |
| Completion | One timeline load, one record | One timeline load, one record | None |

The estimate uses one cycle per load, store or compare, and no cache lines
are added. The additions come to about 85 cycles per job. Allowing four times
that for cache misses on the job copy and mispredicted branches gives at
most 400 cycles: 0.2 µs on an A76 at 2,002 MHz and 0.8 µs on an A55 at
494 MHz.

The board measured these kick-to-timeline times per job
(`docs/designs/gpu-job-session.md:212-223`): 80 to 83 µs, 87 µs for the
first job after the client ring wrapped, and 113 µs for the first job after
an MMU cache flush. That time covers firmware scheduling, the work and the
timeline write, and none of the submit path. The queue's addition is at most
0.25% of the smallest measured job time on an A76 and 1% on an A55. The
first commit's 161 removed uncached stores reduce the submit time by an
amount this design does not estimate; its board request measures it.

Creation has two one-time costs. The table build costs what it costs at
session prepare today. The clean and invalidate of the 2 MiB output lease
covers 32,768 lines. At 20 cycles per line, the time model's assumption
(`docs/designs/gpu-mimus-compute.md:289`), that is 0.33 ms on an A76 at
2,002 MHz.

### Budgets

| Metric ([performance architecture](../performance-architecture.md), GPU) | Baseline (mimus session) | Budget |
| --- | --- | --- |
| Submit to kick, worker idle | 1 worker step, no tick wait | 1 worker step, no tick wait |
| Client wake-ups per job | 1 record | 1 record |
| Worker wake-ups per job beyond interrupts | 1 submit wake | 1 |
| Queue round trips to deliver a result | 0 (in place) | 0 (in place) |
| Copies per input or result byte | 0 | 0 |
| `DSB SY` per job, transport and maintenance | 57 and 6 | At most 57 and 6 |
| Translations per job | 27, all in the backend | At most 27; none added |
| Uncached ring stores per job | 184; 23 after the first commit | 23 |
| Added CPU time per job, A76 at 2,002 MHz | Not applicable | At most 0.5 µs |
| Hang detection | 1 s plus at most the 4 ms backup poll | `job_us` plus at most the 4 ms backup poll |

### Heavier forms used

- **Per-job admission compares** (performance architecture section 1.1):
  every compared field changes per job, because the application writes the
  job.
- **A copy of the job description** (section 1.6): the worker must not read
  fields that the client can still change after the checks. Buffer data is
  never copied.

### Polling that remains

The worker caps its wait at 4 ms while a job is in flight
(`drivers/gpu/pvr_gpu_service.c:1034-1037`). The cap stays until the GPU
interrupt is qualified on a board, since physical GPU interrupt behaviour
remains unqualified (`docs/hardware.md:1684`). With the cap, a lost
interrupt costs at most 4 ms on that job instead of a FAULT at `job_us`.
The worker keeps its one-tick wait while a masked event batch remains,
because that batch is already known to be pending
(`drivers/gpu/pvr_gpu_service.c:1016`). An idle worker waits with
`portMAX_DELAY`.

### Failure classes

| Failure | Class (performance architecture 1.8) | Retained |
| --- | --- | --- |
| Malformed or inadmissible job | Refused before device access | Nothing; context usable |
| Kick deadline passed in the slot | Refused before device access | Nothing |
| Creation refused (region, program, pool) | Refused before device access | Pages acquired so far, retained for the boot |
| Device CLOSE with acknowledged stop | Known terminal state | Context pages and tables, for reopen |
| Hang, reset event, timeline mismatch, failed cache operation, transport fault | State unknown | Every resource; service FAILED |

### Measurement

- **Host:** a new `checks/gpu-queue.py` replays the bench recording's 204
  jobs through the queue in the existing GPU service host model. It counts,
  per job, worker steps, `DSB SY`, queue operations, critical sections,
  translations, cache lines and uncached stores. The same replay through the
  mimus session, as `checks/pvr-mimus-service.py` runs it, gives the
  baseline. The engine model must produce bit-identical outputs through
  both.
- **Board, as a coordinator request:** the eight-job firmware session
  through the queue, recording kick-to-timeline per job on the 24 MHz
  counter, and submit-to-kick and timeline-to-record on the same counter.
  The prediction is 80 to 83 µs kick-to-timeline per job, 113 µs for the
  first, and at most 0.5 µs of added CPU time per job on an A76.

## Migration: ordered commits

The commits that edit `drivers/gpu`, `include/pvr_*` or the `apps/os` GPU
files start after the GPU bisection branch merges. Each commit leaves every
image building and its host checks passing.

1. **8-byte ring stores in the mimus session.** `ring_write` in
   `drivers/gpu/pvr_mimus_service.c` stores aligned 64-bit words when the
   offset and length are multiples of 8, and bytes otherwise. The host
   model's uncached ring stores per job fall from 184 to 23; every other
   count of `checks/pvr-mimus-service.py` stays the same.
2. **Queue in the worker.** `drivers/gpu/pvr_compute_queue.c`,
   `include/gpu_queue.h` and `include/pvr_compute_queue.h`. The worker calls
   the queue at each mimus site, and an image configures one of the two.
   The performance hold, reopen and the wake handshake become the queue's;
   the request queue grows by two messages. `checks/gpu-queue.py` replays
   the bench jobs with a region table that matches the mimus manifest, and
   checks the 24 table pages.
3. **Device ownership in the OS GPU runtime.** The context creation opens
   the device; thermal close returns PARKED and reopen restores the context;
   the return path closes the device. Host cases cover sequence continuity
   across close and reopen.
4. **Program descriptors from `compile.py`.** A `compile --profile 3
   --queue-program` output writes `struct gpu_queue_program`. The mimus
   kernel's descriptor holds the same code as
   `drivers/gpu/pvr_compute_mimus_usc.c`.
5. **mimus on the queue.** After application-interface commits 5 ("GPU glue
   into mimus") and 7 ("Generated tables"). Add `apps/mimus/gpu_linear.c`; the
   hook calls `gpu_queue_*`; the leases adopt manifest regions; the worker's
   affinity uses the generated GPU route's core. The engine model and the
   QEMU session cases pass with bit-identical outputs.
6. **Board request** through the coordinator: the eight-job session and the
   exact-sum caller through the queue.
7. **Remove the mimus session:** `pvr_mimus_service.c/.h`,
   `pvr_mimus_job.h`, `pvr_compute_mimus.c/.h`, `platform/a733/gpu_mimus.c`,
   `include/a733_gpu_mimus.h`, the `mimus` and `mimus_owner` configuration
   fields, `PVR_GPU_SERVICE_MIMUS_WAKE` and the build option
   `SUNSTONE_A733_OS_GPU_LINEAR`, which becomes `SUNSTONE_A733_OS_GPU`.

## Risks

- **Completion interrupts are unqualified on the board**
  (`docs/hardware.md:1684`). The 4 ms backup poll bounds a lost interrupt.
- **The map is built after the scheduler starts.** Today the session's map is
  built at prepare, before the scheduler. The queue builds it in the worker
  at creation and acquires its table pages from a pool that other tasks can
  also use. Pool calls serialize every core in a critical section
  (`include/device_memory_pool.h:33-35`). An exhausted pool refuses the
  creation.
- **The model files move in the GPU address space.** The kernel reads its
  addresses from the record, so its binary does not change. The board has
  run only today's layout, with the models at `0x43000000`.
- **GPU read-only mappings are not shown to be enforced.** The board has not
  shown that the GPU MMU refuses a store through a read-only mapping
  (`apps/os/dispatch.c:228-230`). The queue does not rely on it.
- **One fault stops GPU use for the rest of the boot.** The service has no
  per-context recovery.
- **Programs are trusted.** A program can reach every lease of its context;
  the queue checks descriptors only.
- **The design depends on another design's commits.** Commit 5 needs the
  application-interface region table and interrupt routes. If that design
  reorders its commits, commit 5 here moves with them.

## Settled decisions

- One address space per context.
- A region with both CPU write and GPU write is refused.
- No priority field.
- A fault fails the whole service, as today.
- The device opens at the first context creation.
- Binding 0 goes in the second pointer register.
- No zero fill of leases.
- One context, one job in flight, one program, four leases, four bindings
  and 64 argument bytes.
- The worker calls the queue directly; no component table.
- The 4 ms backup poll stays until the GPU interrupt is qualified on a board.

## Open decisions

1. **Removing the backup poll.** Which board measurement qualifies the GPU
   interrupt, and therefore removes the 4 ms cap.
2. **The second application.** Which capacity it needs first: a second
   context, a deeper queue, or a second program.
