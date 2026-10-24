# Design: running mimus linear layers on the GPU from the integrated OS image

Status: **design**; nothing below is implemented. It follows
[performance-architecture.md](../performance-architecture.md) and completes the
GPU option of [recognition-dispatch.md](../recognition-dispatch.md), whose
placement table, dispatcher, failure policy and supervision it keeps. The GPU
session code of [gpu-mimus.md](../gpu-mimus.md) (admission, job words, cache
maintenance, the completion tags) is used unchanged.

The image built with `SUNSTONE_A733_OS_GPU_LINEAR` does not build today. The
recognizer stops at its assertion that mimus's arena is at most 12 MiB
(`kMimusArenaBytes` in `apps/mimus/recorded.h` is 16 MiB). The functions that
open the GPU session, start the GPU worker and handle INTID 95 do not exist,
and no image links the GPU service with the session code. This package makes
the option configure, link and pass its host checks. Placing a call on the
GPU also needs a hook inside the mimus engine; "Changes needed in the mimus
engine" lists it. Until that hook exists the image opens the session and
computes every layer on the CPU.

## Data flow and ownership

The loader places the base and overlay models at `0x44000000` and
`0x44800000`. The boot's identity step publishes them to the point of
coherency at EL3 (`apps/os/identity.c`), and the recognizer hashes them before
it opens the session. Activation codes live in mimus's arena, the 12 MiB at
`0x46000000`. The GPU writes one job's tags and sums into the 2 MiB output
region at `0x46C00000`, which the CPU maps read-only at EL0 and EL1. No weight,
code or sum byte is copied.

| Range | GPU address → physical | GPU page, access | CPU maintenance |
| --- | --- | --- | --- |
| Base and overlay models | `0x43000000` → `0x44000000`, `0x43800000` → `0x44800000`, 8 MiB each | 2 MiB, read-only | None; published at boot |
| mimus arena (codes) | `0x46000000`, identity, 12 MiB | 2 MiB, read-only | Clean the job's code extent per job |
| Output region: 64 KiB of tags, then sums | `0x46C00000`, identity, 2 MiB | 2 MiB, read-write | `DC CIVAC` over the job's tags and sums per job |
| Job page, program pages, context-switch programs | Fixed heap addresses of `gpu_compute_reserved.c` | 4 KiB, read-only | Cleaned once at open |
| Firmware objects: context, register state, client ring control and commands, memory context, timeline | Firmware addresses | Not in the job's address space | Uncached; barriers only |

The recognizer task (EL0) owns the codes and, through the dispatcher, the
session state. The GPU service worker (EL1) is the only task that touches GPU
registers, firmware queues, the client ring, the job page and cache lines of
the session. Ownership of the fixed ranges follows from the image layout,
which `checks/os-image-link.py` already proves disjoint. No pool lease covers
them, and nothing else in the image reuses them, so a retained failure retains
them for the boot without a lease record.

Per job:

1. The hook writes the job into a one-entry slot (tensor ordinal, code offset,
   rows, row stride), stores the slot state SUBMITTED with release order and
   sends the worker the existing internal wake message
   (`pvr_gpu_service_message` with client `PVR_GPU_SERVICE_CLIENTS` and
   ticket 0). It then computes its CPU channels.
2. The worker, at a priority above the recognizer on the same core, loads the
   slot state with acquire order. It looks up the tensor admitted at open
   (one bounds compare), assigns the next sequence and calls
   `pvr_mimus_submit`. It writes two client-ring commands: the 96-byte compute
   command, which `pvr_mimus_session_command` wrote once at open into a zeroed
   buffer, and an 8-byte timeline UPDATE carrying the sequence. It kicks the
   firmware and sets the job deadline to the kick time plus `max_job_us`.
3. On INTID 95 the worker drains firmware events. When the timeline equals
   the sequence it calls `pvr_mimus_complete`, then posts an 8-byte record
   (sequence, result) to a static one-entry completion queue.
4. The hook blocks on that queue, checks every tag with
   `pvr_mimus_tags_complete` and runs the read-out.

## Runtime functions and where they live

- `apps/os/gpu.c` (new) composes the runtime from the board's providers:
  `a733_gpu_mimus_start(core)`, `a733_gpu_mimus_open()`,
  `a733_gpu_mimus_begin(job)`, `a733_gpu_mimus_finish(&completion)`,
  `a733_gpu_mimus_stop()` and `a733_gpu_mimus_irq()`. `start` runs in the
  `gpu-linear` start entry before the scheduler. It binds the GPU platform to
  `providers.shared` (GPU resource manager, PMIC, locks), prepares the
  firmware and session memory and calls `pvr_gpu_service_init` with affinity
  `1 << core`. It performs no device access.
- `a733_gpu_mimus_worker` is removed from `dispatch.h`. The service creates
  its own worker, and `os_gpu_linear_task_privileged` grants it EL1 through
  `pvr_gpu_service_task_privileged`, as the accelerator image does.
- `drivers/gpu/pvr_mimus_service.c` (new) is the session component of the GPU
  service: firmware objects, the client ring, the slot, the completion queue,
  the tensor table and the tag epochs below. `pvr_gpu_service.c` calls it at
  eight points (object list, publication, READY poll, event forwarding, wait
  bound, submit window, CLOSE cleanup, failure), each compiled only under the
  target definition `PVR_GPU_SERVICE_MIMUS`.
- `platform/a733/gpu_mimus.c` (new) acquires the pool pages and builds the GPU
  address space with `pvr_gpu_vm`, the compute context with
  `pvr_compute_context.c` and the context-switch programs with
  `pvr_compute_programs.c`, as `gpu_compute_reserved.c` does for packages.
- `apps/os/placement.py` adds, per GPU row, the model region, weight offset,
  weight stride and K. OPEN admits each such tensor once with
  `pvr_mimus_tensor_open` and refuses when the table's `job_rows` exceeds the
  worker's own bound.

## Interrupts and polling

INTID 95 is routed at boot to the recognition core and stays disabled until
the service's first rearm. The handler masks it and posts one wake. Recognizer,
worker and interrupt share that core, so no completion needs an
inter-processor interrupt. Until SPI 63 delivery is qualified, the worker's
existing 4 ms bound while a job is in flight finds lost completions.
The recognizer never polls: OPEN and stop also finish with a record on the
completion queue, which replaces the accelerator client's tick-paced STATUS
requests. Its waits are finite receives that record no service progress, so
a worker that never answers expires the 3 s mimus service window and the
watchdog resets the board. The start sequence keeps its existing one-tick
waits for hardware steps longer than 50 µs, which section 1.3 permits.

## Memory placement and cache policy

- **Pools.** The WB pool (2 MiB) gives 34 firmware pages, the job page, two
  program pages, the context-switch programs and the GPU table pages. The
  uncached pool (2 MiB, of which USB2 takes 128 KiB) gives 45 firmware pages
  and the six session objects.
- **Open.** OPEN cleans the job, program and context pages once, and cleans
  and invalidates the 64 KiB tag area. It does not clean the models, which
  boot publication covers, or the full output region, because every job
  cleans and invalidates the bytes it reads.
- **Tag epochs.** The CPU cannot write the read-only tag area, and DRAM keeps
  the previous boot's tags, which use the same small sequences. At OPEN the
  worker reads all 16,384 tag words and marks each word's top 15 bits in a
  4 KiB bitmap. It chooses the lowest unmarked value `e`, which exists because
  32,768 values exceed 16,384 words, and numbers jobs `e·2^17 + 1` upward. A
  matching tag therefore comes from this boot's job with that sequence. After
  131,071 jobs (642 bench recordings) it rescans and takes the lowest unmarked
  epoch above `e`; if none exists, every later call runs on the CPU. This
  replaces the start-code zeroing that the kernel design requested.
- **Per job.** Barriers and maintenance are the kernel design's: three
  `aarch64_device_cache` calls with six `DSB SY`. The client-ring write
  operation copies without a barrier, because `pvr_cccb` issues its release
  barrier before it publishes the write offset. The timeline is a plain
  load from uncached memory; the leading `DSB SY` of the completion's
  `DC CIVAC` orders the reads after it.

## Synchronization across cores

All runtime work runs on the recognition core. The slot has one writer per
state: the recognizer in IDLE, the worker in SUBMITTED. The worker reads the
slot after an acquire load and posts the completion after the trailing
`DSB SY` of its maintenance; the queue's critical section orders the post
before the recognizer's tag reads. Interrupt mask and rearm keep the service's
critical sections. A second inference worker on the pair's other core, once
installed, writes codes before the hook's join; the kernel design's leading
barrier on the code clean covers that ordering.

## Mapping check at application start

With the option, `mimus_board_pins` checks the 12 MiB arena as EL1-writable
and the 4 MiB view at `0x46C00000` as EL0-readable and not EL0-writable, at
the link-time stride `a733_local_pool_stride` (2 MiB in images that link
`platform/a733-smp-cached/memory.c`, 4 KiB elsewhere). That builder maps image
RAM with level-2 block descriptors only, so one translation describes a whole
block. The QEMU golden comparison of `checks/device-memory-capacity.py`,
including its read-only-view fixture, fails if any entry becomes a page
table; it is the check that covers the per-page translations removed here.
The asset checks and the default build's 16 MiB check are unchanged.

## Build gate

`cmake/a733-os.cmake` sets both `A733_GPU_OUTPUT_EL0_READ_ONLY` and
`A733_GPU_OUTPUT_CLEAN_INVALIDATE` itself in the GPU branch, now that
`memory.c` maps the view and `pvr_mimus_complete` uses `DC CIVAC`. It refuses
a user-supplied value of either. `os-image-link.py` checks in the built image
that `pvr_mimus_complete` passes operation 1 (clean and invalidate) to
`aarch64_device_cache`. The branch adds the GPU service, the session code, the
firmware (`SUNSTONE_GPU_FIRMWARE`, SHA-256 pinned as in the accelerator image),
`PVR_GPU_SERVICE_MIMUS` and `SUNSTONE_A733_OS_GPU_LINEAR` as target
definitions. `recorded.h` declares 12 MiB when `SUNSTONE_A733_OS_GPU_LINEAR`
is defined; the arena peak is at most 2,784,048 bytes.

## Failure classes

| Failure | Class (section 1.8) | Result | Retained |
| --- | --- | --- | --- |
| Preparation refused in `start` (pool, firmware hash, owner collision) | 1 | OPEN refused; CPU for the boot | Acquired pool pages |
| OPEN fails after power or firmware access | 3 | CPU for the boot | Everything the service holds; the GPU can write only the output region and firmware memory |
| Job refused before the kick | 1 | Completion REFUSED; the hook computes the rows | Nothing |
| Tag missing | 2 | `a733_gpu_mimus_stop` runs CLOSE: context cleanup, then the acknowledged `pvr_gpu_stop`; then `os_dispatch_incomplete` | Output region for the boot |
| Deadline missed, CONTEXT_RESET event, service FAILED, stop not acknowledged | 3 | Completion LOST; `os_dispatch_failed`, `board_fail`, watchdog reset | Everything |
| No completion | Unknown | Service window expires; watchdog reset | Everything |

`max_job_us` is 100 ms: the largest bench job (62,447,616 MACs) models at
26 ms with 200-clock loads. No reset recovery exists, so the runtime reports
LOST and never RESET.

## Budgets

Baselines are commit `0fd1b51`. "Host check" is `checks/pvr-mimus-service.c`
(new), which runs the service worker and the session component over the
bench recording's 204 jobs against the software firmware model of
`checks/pvr-compute-service.c`, with wrap counters on `aarch64_device_cache`,
`aarch64_device_translate`, the barrier operations, queue calls and timed
waits.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Default OS images (supervised, resident, resident with microphone, resident with headset) | — | Loadable bytes identical apart from the 32-byte build identity | `llvm-objcopy -O binary` at both commits |
| Other images that link `drivers/gpu/` | — | Loadable bytes identical | Same, for the accelerator, GPU application and GPU display images |
| GPU linear image | Stops at the arena assertion | Links; link check passes; `.text` + `.rodata` at most 1.75 MiB of the 2 MiB read-only block (base 888 KiB) | Build, `llvm-size -A` |
| Translations in `mimus_board_pins`, arena and view | 5,120 | 10 | Counter in `checks/os-image-dispatch.c` |
| Per job, slot write to kick: translations, hashes, allocations, tick waits in the session component | No path | 0, 0, 0, 0; at most 13 compares | Host check |
| `DSB SY` per job | Compute path: 16 in transport, 2 per cache call | At most 12: 6 in maintenance, at most 6 in transport | Host check |
| Bytes through queues per job | No path | One 64-byte wake message, one 8-byte completion record | Host check |
| Wake-ups per job | No path | Recognizer 1; worker 2 (wake, interrupt); 0 inter-processor interrupts | Host check |
| Cache lines at OPEN | Dispatcher design: 294,912 | 1,024 for the tag area, plus the job, program and context pages | Host check |
| Tag words read | None | 16,384 at OPEN, again after each 131,071 jobs | Host check |
| Hang detection | None | 104 ms after the kick | Host check, timeline never updated |
| Pool pages for the GPU | 0 | At most 80 WB and 52 uncached pages, allocated after USB2 and audio in the image's admission order | Replay case in `checks/os-image-dispatch.c` |

The backend's checks in `platform/a733/gpu_backend.c` translate on every
register access and clock sample. The host check counts these translations
per job and reports them without a target; see "Left out".

## Reused and removed

Reused: the GPU service and its pacing, start, stop, client ring, firmware
queue and interrupt path; the platform binding, backend, firmware reservation
and VM publication of the accelerator image; the mimus session code; the
placement table and dispatcher. Removed: `a733_gpu_mimus_worker`; the two
user-supplied CMake constants; per-page translations in the GPU branch of
`mimus_board_pins`; OPEN's model and output-region cleans; the request for
start-code zeroing of the tag area.

## Left out

- **Backend per-access translations** (section 1.1). They are an
  all-GPU-image change, estimated below 1 ms per recording, under the 2% rule
  of the kernel design. They return when the host check's count, at a
  board-measured cost per `AT`, exceeds 16 ms per recording.
- **Compiling out unused service components.** The image links the compute,
  render and free-list objects that `pvr_gpu_service.c` references; the
  `.text` budget absorbs them.
- **The second inference worker.** `apps/mimus/parallel_runtime.cc` installs
  `ParallelFor`; it is SunstoneOS code in this repository and needs no engine
  change. It lands with the engine hook, which is its first GPU consumer.
- **Reset recovery and power-off when idle**, as in the dispatcher design.

## Hardware sequences and their sources

This package adds no register sequence. Power, clock, start, stop, the kick
at `0xb00` and SPI 63 come from existing code that cites the board driver
`linux-orangepi` at `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` and its device
tree. The compute command, timeline UPDATE and command order follow that
driver's `pvr_queue.c`; per-job GPU cache flushing follows its `pvr_drm.h`.
Mesa `86c7176d2dd0065555521bf8c61f208bf53c75d6` supplies the CDM and PDS
encodings. These sources do not establish on this board:

- 2 MiB GPU pages (format checked by `checks/pvr-gpu-vm-reference.py` only);
- `DC CIVAC` at EL1 on a read-only page without a fault, which the Arm
  architecture specifies and no vendor source states;
- per-job device cache flushing before the timeline UPDATE;
- execution of the mimus program and SPI 63 delivery.

## Board evidence that would confirm the design

- OPEN reaches READY; the tag scan's epoch and the tag area's contents are
  recorded on a cold and a warm boot.
- With the engine hook, the bench recording's 204 jobs give text and CRCs
  equal to the host reference; INTID 95 arrives on the recognition core for
  every job, with submit-to-kick and completion-to-wake times.
- An injected missing tag ends in an acknowledged stop and CPU-only
  recognition; an injected hang ends in LOST and a watchdog reset.

## Changes outside this package

None needs `primary.patch`: the interrupt table already leaves enabling to the
device owner, and the read-only view and EL0 translation exist. Other owners:
the GPU runtime owner reviews the eight `PVR_GPU_SERVICE_MIMUS` points in
`pvr_gpu_service.c`.

## Changes needed in the mimus engine

1. `TensorView::ordinal`, set by `Model::Load`: base tensors in file order
   with overlay replacements in place, then the overlay's new tensors.
2. An installable two-part exact-sum hook on AArch64, called from `Linear`,
   `LinearAccumulate`, `LinearProduced`, `LinearProducedParallel` and
   `LinearProducedAccumulate` for int8 weights and int16 codes. `begin`
   receives the ordinal, rows, K, N, code pointer and row stride, scales,
   bias, alpha, residual flag, output pointer and stride and any row
   post-function, and returns `n_gpu`. The engine computes channels
   `[n_gpu, N)`. `finish` returns the rows completed on the GPU, after which
   the engine computes channels `[0, n_gpu)` of the remaining rows.
3. Activation codes of every call that reaches the hook allocated from the
   arena, with a row stride that is a multiple of 4 bytes and at least 2K.
4. Subsampling rows produced in blocks of 224 (the last of 184) into one
   709,632-byte code buffer in place of 160-row calls.
