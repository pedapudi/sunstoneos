# GPU compute queue

Status: implemented and qualified on the board (ten open/job/close cycles with a peer job, full recorded recognition, injected-timeout recovery); host-tested. Recovery from a shader that never becomes idle, separate application address spaces and the submit-path timing through the queue are not qualified.

The GPU compute queue is the OS service through which an application runs a compute program on the A733 GPU. The application creates one context with one program from `apps/gpu-program/compile.py` and up to four leases of its own manifest regions, submits jobs, and waits for each job's completion record. The application interface is [`include/gpu_queue.h`](../../include/gpu_queue.h). The service worker uses [`include/pvr_compute_queue.h`](../../include/pvr_compute_queue.h) (`drivers/gpu/pvr_compute_queue.c`), and the A733 constructor is [`platform/a733/gpu_queue.c`](../../platform/a733/gpu_queue.c). `SUNSTONE_A733_OS_GPU` selects the service. The external mimus application and [`apps/queue-add`](../../apps/queue-add/README.md) use the same application kit.

Finding 6 of [the layering audit](../layering-audit.md) classes a per-application GPU session as a defect that blocks a second application. An image classifier with a GPU preprocessing kernel must use the queue by adding files under its own `apps/<name>/` directory only. The qualification service (`drivers/gpu/pvr_compute_service.c`), with its 4,096-byte input and 3,968-byte output limits (`include/pvr_compute_package.h:7-8`), is separate.

## Retained close, shared clients and fault recovery

One GPU context retains one program, up to four leases and one published memory map. Two explicitly admitted application tasks may share it. Each has one submission slot and one completion queue; only one job executes at a time. The worker selects submitted slots in round-robin order. Application sequence numbers are independent. A separate increasing firmware serial prevents equal sequence numbers from satisfying the wrong job's timeline.

The primary controls device-wide lifecycle. `gpu_queue_close` seals admission, drains its owed record and returns `GPU_QUEUE_BUSY` while the peer still owns work. It preserves the peer's queued job. Successful close includes acknowledged firmware stop, idle checks and platform OFF. `gpu_queue_open` reconstructs firmware context state and repeats startup, health and MMU-cache completion. Program bytes, leases and GPU page tables remain retained. Explicit close stays closed until explicit open; thermal parking reopens automatically.

A READY job timeout or decoded reset event starts automatic recovery when the native backend's clock, power ownership and session remain valid. The worker masks interrupts and stops kernel traffic, then runs the unacknowledged stop plan (`pvr_gpu_stop_init_unacknowledged`, derived from the pinned open driver's `pvr_power_reset` and `pvr_fw_stop`): mandatory Sidekick, BIF and SLC idle polls, full soft reset, the completed reset read and platform OFF. The [reset completion contract](gpu-recovery-reset-readback.md) defines that read. Only completed isolation and firmware-memory reclaim permit the failed job's FAULT record. The failed job is never replayed.

Recovery reconstructs retained firmware state, increments the service epoch and requires fresh health and MMU-cache completion before another kick. The peer's submitted job, buffers and original kick deadline survive reconstruction; an expired queued deadline produces REFUSED. `gpu_queue_recover` waits for this bounded recovery and does not consume a caller's owed record.

Stop is bounded by 300 ms and READY by one second from first fault detection, both clipped to the admitted session and the returning-image workload cutoff. A failed idle check, reset write or read, OFF, restart, clock or session check leaves the service failed and its backing retained. No fault record returns buffer ownership after unproven isolation. Recovery does not force reset through an idle failure, so arbitrary stuck hardware requires external recovery.

Final application completion drains work and seals the context before the CPU return or resident task suspension. All caller slots, records, service replies, IRQ wakes and recovery work must be quiescent. A successful reversible close provides the same GPU quiescence prerequisite for a future system sleep path; complete system suspend and wake are not qualified. Returning images produce an untouched receipt when GPU construction was skipped (an application without a GPU grant or one that never uses it); it requires an admitted application, its current task, entirely zero GPU state and an unexpired return deadline.

The fixed sharing composition is defined in [the application interface](app-abi.md). The second application ([`apps/gpu-queue-peer`](../../apps/gpu-queue-peer/README.md)) uses independent mutable ranges. Separate application address spaces and GPU programs are outside the shared-context implementation. The vendor power, voltage, operating point and firmware-start sequence are unchanged.

## One context serves two caller slots with one active job

| Capacity | Value | Why |
| --- | --- | --- |
| Contexts | 1 | Two trusted applications explicitly share the program and map |
| Caller tasks | 2 | Each caller owns one copied job slot and completion queue |
| Jobs in flight per context | 1 | mimus jobs share the output region; the client ring allows one KICK outstanding (`include/pvr_cccb.h`, `pvr_cccb_begin`) |
| Programs per context | 1 | mimus has one kernel |
| Leases per context | 4 | mimus maps its output region, two model files and its arena |
| Bindings per job | 4 | mimus binds three ranges |
| Argument bytes per job | 64 | mimus passes 16 |
| Job record | 160 bytes | 32-byte header, 4 bindings of 16 bytes, 64 argument bytes |

Each limit is a constant in `include/gpu_queue.h`. A consumer that needs another capacity raises the constant and, for more contexts or a deeper queue, adds the per-context and per-slot storage these limits leave out.

## The application interface

The interface is plain C without FreeRTOS types, so host checks and applications share it. The calls run in the application's GPU client task, which the image binds to a GPU service client slot before the scheduler starts (`pvr_gpu_service_bind`). The caller makes an atomic store or load in shared memory and the EL1 worker does the device work ([application interface](app-abi.md), "Applications call the OS through plain functions that are safe at EL0").

### Creation opens the device, maps the leases and loads the program

`gpu_queue_context_create` takes one `struct gpu_queue_context_config`: the job bound `job_us`, the program descriptor and up to four lease requests. It travels through the GPU service ticket path (`pvr_gpu_service_send_epoch` and `pvr_gpu_service_collect`) and waits at most `wait_ticks`. A reply that arrives after the wait stays with the client's pending ticket, and the next call collects it.

The worker, with the device COLD:

1. checks the program descriptor and every lease request against the application's region table;
2. copies the program into the context's program pages;
3. plans the mappings, acquires the planned GPU table pages and builds the context's GPU address space once;
4. cleans and invalidates each GPU-written lease once;
5. builds the firmware compute context;
6. runs the OPEN sequence and replies when the device is READY.

A context is created once per boot. `gpu_queue_close` stops the device and retains every page; `gpu_queue_open` reuses the context for another powered lifetime in the same boot. Final application completion permanently seals it. The device opens at the first context creation.

### Submit and wait bypass the request queue

1. `gpu_queue_submit` checks the fields that need no worker state: the binding count, the binding lease indices, the argument length and nonzero group counts. It copies the 160-byte job into the context's slot and wakes the worker. A slot that still holds an unanswered job returns BUSY; a closed device returns PARKED. Neither queues anything.
2. The worker copies the slot into its own storage and checks only that copy, so it never reads fields the client can still change.
3. `gpu_queue_wait` blocks in `xQueueReceive` on the context's record queue of one entry. The worker posts the record after the completion interrupt, so the client wakes without polling.

The worker reaches the kick in one step with no tick wait.

### A job binds leases in place

A binding names a lease, the byte offset in it that the program receives as a GPU address, and a byte range for cache maintenance (`struct gpu_queue_binding`). The queue copies no buffer data. Maintenance follows the access bits of the lease's region and touches only the declared range, rounded out to 64-byte lines:

| Region access | At creation | Before the kick | After the timeline update |
| --- | --- | --- | --- |
| CPU read and write, GPU read | None | Clean the range to the point of coherency | None |
| CPU read, GPU write | Clean and invalidate the whole lease | None | Clean and invalidate the range (`DC CIVAC`) |
| CPU read, GPU read | None | None | None |

- **CPU read and write, GPU read** (mimus's arena): the CPU writes through a write-back mapping, so each job's range is cleaned before the kick.
- **CPU read, GPU write** (mimus's output region): the CPU mapping is read-only at EL1 and EL0, because AArch64 has no access encoding that is writable at EL1 and read-only at EL0 (`platform/a733-smp-cached/memory.c:109-117`). Reads leave clean lines. The clean and invalidate at creation removes lines from boot-time accesses and the one after each job removes lines the CPU read before the GPU wrote. `DC IVAC` would fault on the read-only mapping (`docs/designs/gpu-recognition.md:235`), so both use `DC CIVAC`, which needs read permission only.
- **CPU read, GPU read** (INPUT regions, such as mimus's model files): the EL3 publication step cleans them before the OS runs, from the generated region table of the [application interface](app-abi.md), and nothing writes them afterwards.

Creation refuses a region with both CPU write and GPU write, because a CPU store between the clean and the invalidate would be written back over GPU output. It also refuses a region without CPU read, because `DC CIVAC` needs it. A range of zero bytes skips maintenance. The range limits maintenance only: the GPU MMU confines a program to the leases of its context, and a program can read any byte of any lease and write any byte of a GPU-writable one.

### Programs keep the compiler's two-pointer register layout

Profile 3 of `compile.py` admits programs with two 64-bit pointer arguments in unified registers 0-3, the flat local index in unified register 4, the workgroup IDs in coefficients 0-2 and the group counts in coefficients 4-6 (`include/pvr_compute_package.h:20-25`, `apps/gpu-program/compile.py:161-167`). The queue assigns the two pointers:

- **Unified registers 0-1:** the job's argument block, holding the GPU addresses of bindings 1 to n-1 as 64-bit words, then the job's argument bytes.
- **Unified registers 2-3:** the GPU address of binding 0 (the second pointer register).

The mimus kernel reads this layout: a 32-byte record with the weight and code addresses first and four shape words after them, and the output region in the second pointer (`shaders/pvr_mimus_linear.cl`).

A program descriptor (`struct gpu_queue_program`) carries the code, its length (at most 8,128 bytes, the limit of `compile.py --session`), the compiler's temporary count, coefficient count and local size, and the three digests the compiler manifest records. The queue checks the descriptor's fields at creation. It cannot check the code's memory behavior: programs are trusted, and source admission is an offline review step (`include/pvr_compute_package.h:26-31`). The local-memory ABI is in [gpu-compute-local-memory.md](gpu-compute-local-memory.md).

### The worker repeats every check on its own copy

| Check | Where | Refusal |
| --- | --- | --- |
| Context open | Submit, worker | REFUSED, `CLOSED` |
| 1 to 4 bindings, each lease index below the lease count | Submit, worker | REFUSED, `BINDING` |
| Binding address and maintenance range inside the lease | Worker | REFUSED, `RANGE` |
| Sequence above the caller's last sequence and below 2^32 - 1 | Worker | REFUSED, `SEQUENCE` |
| Each group count nonzero | Submit, worker | REFUSED, `GRID` |
| Argument bytes at most 64, and the argument block at most 88 bytes | Submit, worker | REFUSED, `ARGUMENTS` |
| `not_after_us` not passed at the kick | Worker | REFUSED, `DEADLINE` |

Lease access needs no per-job check, because creation already refused the combinations that maintenance cannot serve. Every check compares against the context's lease table, which never changes after creation. No check translates an address, hashes a buffer or takes a lock. A refused job gets a REFUSED record and nothing reaches the client ring.

### REFUSED, PARKED and FAULT reach the application, which decides

The queue never computes a job on the CPU. Results follow section 1.8 of [the performance architecture](../performance-architecture.md):

- **REFUSED:** nothing reached the device and the context stays usable. The application may run the work on the CPU; mimus does so (`apps/os/gpu_hook.cc`).
- **PARKED:** explicit close, thermal parking, recovery or final retirement prevents submission. Nothing was queued by this call. Explicit owner OPEN ends manual close; thermal policy ends thermal parking.
- **FAULT:** an isolated failed job produces its own FAULT record. Successful recovery leaves the context usable and preserves the peer's queued work. Permanent isolation or restart failure retains every resource and any unpublished owed record. A scalar FAULT result alone does not prove device isolation or return buffer ownership. The application may end the session; mimus ends it on FAULT.

### Each context has a hang bound, and each job may have a kick deadline

- **Job bound:** `job_us`, 1 µs to 1 s, from the kick to the timeline update.
- **Kick deadline:** `not_after_us`, optional. A job still in the slot at that time, for example during a performance hold, gets a REFUSED record with `DEADLINE`. A recognizer with a fixed service window uses it to bound GPU work it would otherwise wait for.
- **Creation and close:** each takes `wait_ticks`.

### The objects follow the upstream PowerVR driver

| Upstream (`linux-orangepi` `2ac08e8c7`, `bsp/drivers/gpu/imagination`) | Queue |
| --- | --- |
| `DRM_IOCTL_PVR_CREATE_CONTEXT` with a VM context handle and priority (`include/pvr_drm.h:764`, `pvr_context.c:280-311`) | `gpu_queue_context_create`; one address space per context; no priority |
| GEM object mapped by `DRM_IOCTL_PVR_VM_MAP` (`include/pvr_drm.h:645`, `pvr_vm.c:735`), MMU flush on change (`pvr_mmu.c:124`) | Leases mapped once at creation; no per-job map flush, and the startup flush repeats after restart |
| `struct drm_pvr_job` with a caller-built command stream (`include/pvr_drm.h:1229`) | `struct gpu_queue_job`; the worker builds the CDM command from the program and grid, so applications never write firmware commands |
| Done fence signalled when the queue's timeline UFO reaches the job's seqno, signed compare (`pvr_queue.c:942-962`) | Record posted when the timeline reaches the worker's internal serial, retaining the caller's job sequence |

This design uses the upstream driver source only, and no information from the firmware binary.

## The queue runs inside the GPU service worker

The GPU service worker keeps the firmware, the kernel command queue, power, the clock, the interrupt, the lifecycle and the request queue. The queue owns the context's firmware objects, its client ring, its job page and its slot. The worker calls the queue directly (`pvr_compute_queue_prepare`, `_publish`, `_objects`, `_submitting`, `_progress`, `_hold`, `_release`, `_poll`, `_reopen`, `_close`, `_deadline`, `_abort`, `_event`); there is no component table.

The queue uses 6 firmware objects: compute context, register state, client-ring control, client ring (1 KiB), memory context and timeline. With the firmware's 19 resources they use 25 of the 64 entries of the MIPS table (`include/pvr_fw_state.h:13`, `include/pvr_mips.h:7`). They join the table at OPEN, because the firmware table has no live update.

### The job page is written once at creation

The context has one job page in the PDS heap:

| Bytes | Contents | Written |
| --- | --- | --- |
| 0-127 | PDS data: task word, group counts, the two pointer constants | At creation; the binding-0 address and the group counts again only when they change |
| 128-191 | PDS dispatch program | At creation |
| 192-255 | Fence PDS program | At creation |
| 256-315 | CDM stream: the grid kernel, the fence kernel, the terminate word | Per job |
| 320-407 | Argument block: binding addresses, then argument bytes | Per job |

The worker writes the PDS data with `pvr_compute_job_grid_pds`. A program that reads no group counts, such as mimus's, keeps zero counts there; the CDM stream carries each job's grid. Per job the worker writes the CDM stream with `pvr_compute_job_grid_cdm`, the argument block and the 8-byte timeline UPDATE. It rewrites the binding-0 pointer only when the address differs from the previous job's, and the group counts only for a program with seven coefficients and only when they change. The per-job bytes are contiguous from byte 256, so one clean covers them (at most 3 lines). The 96-byte firmware compute command points at the CDM stream and is built once with `pvr_compute_job_command`.

### Completion reads one timeline word

On each READY step after an interrupt, the queue reads the context's timeline word, one uncached load. When it equals the active job's internal firmware serial, the worker cleans and invalidates the job's GPU-written ranges, checks the job bound once more and posts the caller's sequence in the record. A value matching neither the previous nor the active firmware serial produces FAULT `TIMELINE`.

### The wake cannot be lost

The slot state, the lost flag and the wake flag are atomics, and every access on both sides is sequentially consistent (`include/pvr_compute_queue.h`, comment above `pvr_compute_queue_wake`):

1. The client writes the job into the slot, then stores SUBMITTED to the slot state.
2. The client exchanges the wake flag with 1. If the old value was 0, it sends one wake message to the worker's request queue.
3. The worker, on that message, stores 0 to the wake flag, then loads the slot state. If the state is SUBMITTED, it takes the job.

All these operations fall in one total order. Either the worker's clear precedes the client's exchange, which then reads 0 and sends, or the client's store precedes the worker's load, which then sees SUBMITTED. In both cases the worker takes the job, so it can wait with `portMAX_DELAY` when idle. The request queue holds `PVR_GPU_SERVICE_CLIENTS + 2` messages: one per client (each client has one pending request), one for the coalesced interrupt wake and one for the queue's wake, so a wake send never finds the queue full.

### The performance hold names no client

1. HOLD calls `pvr_compute_queue_hold` on each worker step. The queue stops kicking and returns 1 once nothing is in flight. A job submitted during the hold stays in the slot.
2. When the queue returned 1, the worker starts `pvr_gpu_clock_hold`. RATE and VOLTAGE run only while held.
3. RELEASE resumes the clock and then calls `pvr_compute_queue_release`, which kicks the waiting job.

The client sees a later record instead of a HELD result. A job whose `not_after_us` passes during the hold gets a REFUSED record at that time, because the worker's wait includes that deadline.

### Device OPEN and CLOSE belong to the OS GPU runtime

The primary application requests reversible device transitions through `gpu_queue_close` and `gpu_queue_open`. The OS GPU runtime performs privileged device sequencing and thermal transitions. The peer cannot request lifecycle changes:

- context creation runs the OPEN sequence before its reply;
- thermal throttling calls `pvr_compute_queue_close` and then CLOSE; jobs submitted while closed return PARKED;
- return to normal reopens the device. `pvr_compute_queue_reopen` restores the firmware memory-context record and the firmware context. The timeline restarts at the last internal firmware serial, each caller keeps its own application sequence, and the GPU tables stay published and unchanged;
- the image's return path closes and parks the device ([normal return](gpu-normal-return.md)).

## Memory and MMU

### Leases adopt manifest regions

The region table belongs to the [application interface](app-abi.md): each image has a table with the address, size, kind and access of every region in the application's manifest (`struct ss_region`, `include/sunstone_app.h`). Virtual address equals physical address, and unpinned regions are placed on 2 MiB boundaries. The platform's CPU tables, the EL3 publication of INPUT regions and the read-only CPU mapping of GPU-written regions come from that design. The queue adds no region table and no CPU mapping rule.

A lease request names a region index and a byte count. The count is a multiple of 2 MiB, at most the region's size rounded up to 2 MiB; zero takes the whole rounded region. The rounded tail lies inside the region's 2 MiB placement, so it belongs to the same application, and the manifest and runtime independently refuse lease tails that intersect another region, including an unleased one. mimus's four leases:

| Lease | Region | Address | Region bytes | Lease bytes | Access |
| --- | --- | --- | --- | --- | --- |
| 0, output | `gpu_output` | `0x46c00000` | 4 MiB | 2 MiB | read, gpu-write |
| 1, base model | `base_model` | `0x44000000` | 7,501,968 | 8 MiB | read, gpu-read |
| 2, overlay model | `overlay_model` | `0x44800000` | 6,560,592 | 8 MiB | read, gpu-read |
| 3, arena | `arena` | `0x46000000` | 12 MiB | 12 MiB | read, write, gpu-read |

The region values come from `apps/mimus-board/manifest.json`. The device-memory pools back no lease, so no application buffer is cleared through a write-back alias. Program copies are confined to the immutable `sunstone_gpu_program` section admitted from application objects.

### GPU virtual address equals physical address

Every lease maps at its physical address. Physical addresses lie below 2^36, and the firmware PDS, USC and general heaps start at `0xda00000000`, `0xe000000000` and `0x8000000000`, so data addresses never collide with internal mappings. A program's pointers equal the application's CPU pointers, and no GPU address allocator is needed.

Every lease maps with 256 KiB GPU pages. BRN 71317 makes the hardware encodings for 1 MiB and 2 MiB pages unsafe on this GPU, and the shared A733 planner refuses larger pages. One 2 MiB GPU region uses one page size (`include/pvr_gpu_vm.h:12-17`). Lease alignment remains 2 MiB.

### The map is built once

`pvr_gpu_vm_build` zeroes the firmware memory-context record and writes the catalogue marker `UINT32_MAX` into it, and requires the table extent to be the planned size. A rebuild while the firmware holds the context would reset a record the firmware owns, so the queue builds the map once, at creation, while the device is COLD:

1. The mapping list is the four leases plus the 4 KiB internal pages (job page, program pages and context-switch programs) at their fixed heap addresses, sorted by address.
2. `pvr_gpu_vm_require` plans the tables. The worker acquires the planned table bytes from the GPU write-back pool, at most 32 pages. An exhausted pool refuses the creation before any device access. Pool calls serialize every core in a critical section (`include/device_memory_pool.h:33-35`).
3. `pvr_gpu_vm_build` writes the tables and the memory-context record; `a733_gpu_vm_memory_publish_begin` and `_poll` clean the table pages.

The map then never changes. Reopen restores the memory-context record from the saved root page without rebuilding the tables, and startup still requires MMU-cache completion before READY after reopen or recovery. The mimus mappings need 15 data leaves in one 1 GiB catalogue entry and the planner's 24 table pages (confirmed by the host check). The queue's table, payload and firmware pages come from the pools through the EL1 write-back mapping and the existing publication steps clean them before the GPU reads them.

### Resources for the mimus context

| Resource | Queue, mimus context |
| --- | --- |
| MIPS firmware objects | 6 |
| Uncached firmware pages | 6 |
| Write-back payload pages | 21 |
| GPU table pages | 24, at most 32 for another application |
| CPU-side state | `struct pvr_compute_queue`, 2,632 bytes on AArch64, including two caller slots and completion queues; GPU images also reserve a fixed 16 KiB peer task stack |

The GPU holds 79 write-back pages (34 firmware, 21 payload, 24 table) and 51 uncached pages for the mimus layout.

## The interrupt route follows the application's main core

The application-interface design generates the interrupt routes: GPU 95 goes to the main core of the application that the session grants the GPU. The worker task's affinity mask must name the same core, because the interrupt table's dispatcher refuses delivery on any other core (`platform/a733-smp-cached/irq_table.c:31`). The OS GPU runtime creates the worker with the generated route's core, and the image link check compares the two.

## mimus is an ordinary client

The mimus GPU glue lives in mimus (`apps/mimus/gpu_linear.c`, `gpu_linear_job.h`, `gpu_linear.cl`); the OS holds no mimus tensor table or tag geometry.

### Setup at the start of recognition

1. `mimus_gpu_context` fills the configuration: `job_us` = 1,000,000 (`OS_GPU_MAX_JOB_US`, `apps/os/gpu.c`), the linear-layer program and the four leases above.
2. `gpu_queue_context_create` opens the device and returns the lease addresses.
3. Tensor admission over the placement table checks each weight extent against its model lease.

### The job record mimus passes

`mimus_gpu_linear_job` fills this record for one slice of `rows` activation rows:

| Field | Value |
| --- | --- |
| `sequence` | The recognizer's next sequence, rising through the boot |
| `groups` | `{⌈n_gpu / 16⌉, ⌈rows / 32⌉, 1}` |
| Binding 0, output | Lease 0, address 0; maintenance `[65536 - 4·tags, 65536 + sums)` |
| Binding 1, weights | Lease 1 or 2, the tensor's model file; address = weight row 0 of the tensor; no maintenance |
| Binding 2, codes | Lease 3; address = `x_offset`; maintenance = the code extent rounded to 64-byte lines |
| Arguments, 16 bytes | K in bits 0-15 and n_gpu in bits 16-31; `rows`; weight stride in bits 0-15 and code stride in bits 16-31; `sequence` |

Here `tags` = ⌈n_gpu / 16⌉ × ⌈rows / 32⌉ × 32 words and `sums` = `rows × n_gpu × (K > 512 ? 8 : 4)` bytes.

### What mimus keeps

mimus keeps one job in flight, the sequence and tag check of each OK record, the comparison of every sum with a CPU sum, CPU computation of refused jobs, the end of the boot on FAULT, and the CPU path while the device is closed for thermal throttling (the queue reports PARKED).

### The application registers startup initialization

An application may place `ss_region_initializer` records in the immutable `sunstone_region_initializers` section. A record names a manifest region and a prefix length. EL3 validates every record before the first store, clears each prefix with aligned 64-bit stores and publishes it before CPU read-only mappings take effect. Prefixes must fit CPU-readable, GPU-written WORK regions that the CPU cannot write. An absent registration clears nothing. Mimus registers its GPU output region and 64 KiB tag prefix in `apps/mimus/gpu_linear.c`; the image check verifies that registration data comes from application objects and remains immutable.

## Performance and failure classes

Budgets per job: submit to kick in one worker step with no tick wait; one client record; no queue round trip to deliver a result; no copy of input or result bytes; at most 57 transport and 6 maintenance `DSB SY`; at most 27 translations, all in the backend; 23 aligned uncached ring stores; hang detection at `job_us` plus at most the 4 ms backup poll. The per-job admission compares (every compared field changes per job) and the copy of the 160-byte job description (the worker must not read fields the client can still change) are the heavier forms used; buffer data is never copied. The estimated added CPU time is at most 0.5 µs per job on an A76, not yet measured on the board ([record](../evidence/gpu-compute-queue.jsonl) `gpu-compute-queue-submit-cost`).

The worker caps its wait at 4 ms while a job is in flight until the GPU interrupt is qualified on a board; a lost interrupt then costs at most 4 ms on that job instead of a FAULT at `job_us`. It keeps its one-tick wait while a masked event batch remains, and an idle worker waits with `portMAX_DELAY`.

| Failure | Class (performance architecture 1.8) | Retained |
| --- | --- | --- |
| Malformed or inadmissible job | Refused before device access | Nothing; context usable |
| Kick deadline passed in the slot | Refused before device access | Nothing |
| Creation refused (region, program, pool) | Refused before device access | Pages acquired so far, retained for the boot |
| Device CLOSE with acknowledged stop | Known terminal state | Context pages and tables, for reopen |
| Job timeout or reset with proved idle, reset and OFF | Isolated failed job | Context, leases and queued peer job survive restart |
| Failed isolation, timeline mismatch, cache or I/O fault, failed restart | State unknown | Every resource and any unpublished owed record; service FAILED |

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Ten open/job/close cycles with a peer job | returned; ten owner jobs, one peer job | [gpu.jsonl](../evidence/gpu.jsonl) `1793558732769026225-99c2d8da` |
| Injected-timeout recovery preserving the peer | returned, one recovered HANG fault, peer and next owner job completed | [gpu.jsonl](../evidence/gpu.jsonl) `1793560094069288734-8923c447` |
| Full recorded recognition through the queue | returned, 186 completed GPU jobs | [gpu.jsonl](../evidence/gpu.jsonl) `1793560622000131271-53954059` |
| Kick to timeline per job | 80 to 83 µs, 87 µs after a ring wrap, 113 µs after an MMU cache flush | [gpu-job-session.jsonl](../evidence/gpu-job-session.jsonl) `gpu-job-session-set30` |
| Host and native models | 14 queue cases, 38 service cases, 204-job replay with 5,870,398 checked sums, 71 constructor-refusal cases | [gpu-compute-queue.jsonl](../evidence/gpu-compute-queue.jsonl) `gpu-compute-queue-host` |

## Risks and limitations

- **Completion interrupts are unqualified on the board** (`docs/hardware.md`). The 4 ms backup poll bounds a lost interrupt.
- **The map is built after the scheduler starts**, from a pool that other tasks can also use. An exhausted pool refuses the creation.
- **GPU read-only mappings are not shown to be enforced.** The board has not shown that the GPU MMU refuses a store through a read-only mapping (`apps/os/dispatch.c:228-230`). The queue does not rely on it.
- **Recovery requires hardware idle.** A timeout or reset resumes the retained queue only after the reset and OFF proof; a failed isolation or restart keeps every resource retained. The timeout test does not cover a shader that never becomes idle.
- **Programs are trusted.** A program can reach every lease of its context; the queue checks descriptors only.
- **Compound transport, backup and restoration checks** belong to the external application launcher, not to the OS check of native retirement captures and the generic decoder.

## Settled decisions

One address space per context; a region with both CPU write and GPU write is refused; no priority field; a recoverable timeout or reset isolates the failed job and restarts the retained context while unproven isolation keeps the service failed; no zero fill of leases; one context, two admitted callers, one job in flight, one program, four leases, four bindings and 64 argument bytes; the worker calls the queue directly; the 4 ms backup poll stays until the GPU interrupt is qualified on a board.

## Open work

1. Choose the board measurement that qualifies the GPU interrupt and removes the 4 ms cap.
2. A consumer that needs independent programs or address spaces needs another reviewed capacity extension.
3. Measure the added submit-path CPU time and the eight-job kick-to-timeline times through the queue on a board.
