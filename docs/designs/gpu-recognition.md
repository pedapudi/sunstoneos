# Design: running mimus linear layers on the GPU from the integrated OS image

Status: **design**; nothing below is implemented. It follows
[performance-architecture.md](../performance-architecture.md) and completes the
GPU option of [recognition-dispatch.md](../recognition-dispatch.md), whose
placement table, dispatcher and supervision it keeps. The GPU session code of
[gpu-mimus.md](../gpu-mimus.md) (admission, job words, cache maintenance, the
completion tags) is used unchanged.

The image built with `SUNSTONE_A733_OS_GPU_LINEAR` does not build today. The
recognizer stops at its assertion that mimus's arena is at most 12 MiB
(`kMimusArenaBytes` in `apps/mimus/recorded.h` is 16 MiB). The functions that
open the GPU session, start the GPU worker and handle INTID 95 do not exist,
and no image links the GPU service with the session code. This package makes
the option configure, link and pass its host checks. Placing a call on the
GPU also needs a hook inside the mimus engine; "Changes needed in the mimus
engine" lists it. The recognizer opens the GPU session only when the engine's
header declares that hook, so until the hook exists the GPU stays unpowered
and every layer runs on the CPU.

With the measured GPU program, no channel split meets the recognition goal of
0.05 times the audio duration: the linear stage alone models at 0.535 s
against the 0.4435 s allowed for all of recognition. "The recognition goal is
not met" gives the figures and the levers that would close the gap.

## Data flow and ownership

The loader places the base and overlay models at `0x44000000` and
`0x44800000`. The boot's identity step (`apps/os/identity.c`) publishes them
to the point of coherency at EL3, and the recognizer hashes them before it
opens the session. Activation codes live in mimus's arena, the 12 MiB at
`0x46000000`. The GPU writes one job's tags and sums into the 2 MiB output
region at `0x46C00000`, which the CPU maps read-only at EL0 and EL1. No weight,
code or sum byte is copied.

| Range | GPU address → physical | GPU page, access | CPU maintenance |
| --- | --- | --- | --- |
| Base and overlay models | `0x43000000` → `0x44000000`, `0x43800000` → `0x44800000`, 8 MiB each | 2 MiB, read-only | None; published at boot |
| mimus arena (codes) | `0x46000000`, identity, 12 MiB | 2 MiB, read-only | Clean the job's code extent per job |
| Output region: 64 KiB of tags, then sums | `0x46C00000`, identity, 2 MiB | 2 MiB, read-write | Tags zeroed and published at boot; `DC CIVAC` over the job's tags and sums per job |
| Job page, program pages, context-switch programs | Fixed heap addresses of `gpu_compute_reserved.c` | 4 KiB, read-only | Cleaned once at open |
| Firmware objects: context, register state, client ring control and commands, memory context, timeline | Firmware addresses | Not in the job's address space | Uncached; barriers only |

The recognizer task (EL0) owns the codes and, through the dispatcher, the
session state. The GPU service worker (EL1) is the only task that touches GPU
registers, firmware queues, the client ring, the job page and cache lines of
the session. Ownership of the fixed ranges follows from the image layout,
which `checks/os-image-link.py` already proves disjoint. No pool lease covers
them, and nothing else in the image reuses them, so a retained failure retains
them for the boot without a lease record.

In GPU addresses the arena ends where the output region begins. The kernel
clamps every load to the job's last row and channel, so only a kernel defect
reads past the arena. Such a read reaches tags and sums that the GPU may
already write, so the design accepts these over-reads and leaves no unmapped
gap.

Per job:

1. The hook writes the job into a one-entry slot (tensor ordinal, code offset,
   rows, row stride) and stores the slot state SUBMITTED with release order.
   It sends the worker a wake message with a wait of zero ticks: client
   `PVR_GPU_SERVICE_CLIENTS` and ticket `PVR_GPU_SERVICE_MIMUS_WAKE` (1).
   Ticket 0 stays the interrupt handler's wake, and only that message clears
   `irq_wake_pending`. If the send fails, the hook moves the slot from
   SUBMITTED to IDLE by compare-and-swap. When the swap succeeds the job is
   REFUSED and the hook computes its rows on the CPU; when the worker has
   already taken the slot, the hook waits for the completion as usual. The
   hook then computes its CPU channels.
2. The worker, at a priority above the recognizer on the same core, takes
   the slot by compare-and-swap from SUBMITTED to TAKEN with acquire order.
   In one step, under one interrupt mask and re-enable, it looks up the tensor
   admitted at OPEN (one bounds compare), assigns the next sequence, checks
   that the timeline equals the sequence minus one, as `pvr_compute_service`
   does, and calls `pvr_mimus_submit`. It then writes two client-ring
   commands, publishes the ring and submits the kick through the kernel ring.
   The commands are the 96-byte compute command, which
   `pvr_mimus_session_command` wrote once at OPEN into a zeroed buffer, and an
   8-byte timeline UPDATE carrying the sequence. The job deadline is the kick
   time plus `max_job_us`. OPEN writes timeline 0, and sequences start at 1.
3. On INTID 95 the worker drains firmware events. When the timeline equals
   the sequence it calls `pvr_mimus_complete`, sets the slot to IDLE and posts
   an 8-byte record (sequence, result) to a static one-entry completion queue.
4. The hook receives that record and checks every tag with
   `pvr_mimus_tags_complete`. It returns to the engine the job's rows and the
   address, row stride and width (4 or 8 bytes) of its sums. The engine reads
   the sums out with its own code, then calls the hook for the call's next
   job, which may reuse the output region.

## Runtime functions and where they live

- `apps/os/gpu.c` (new) composes the runtime from the board's providers:
  `a733_gpu_mimus_start(core)`, `a733_gpu_mimus_open()`,
  `a733_gpu_mimus_begin(job)`, `a733_gpu_mimus_finish(&completion)` and
  `a733_gpu_mimus_irq()`. `start` runs in the `gpu-linear` start entry before
  the scheduler. It binds the GPU platform to `providers.shared` (GPU
  resource manager, PMIC, locks), prepares the firmware and session memory and
  calls `pvr_gpu_service_init` with affinity `1 << core`. It performs no
  device access. It also reads the previous-boot report and makes OPEN refuse
  for this boot when that report holds a GPU loss ("Failure classes").
- `a733_gpu_mimus_worker` is removed from `dispatch.h`. The service creates
  its own worker, and `os_gpu_linear_task_privileged` grants it EL1 through
  `pvr_gpu_service_task_privileged`, as the accelerator image does.
- `drivers/gpu/pvr_mimus_service.c` (new) is the session component of the GPU
  service: firmware objects, the client ring, the slot, the completion queue
  and the tensor table. `pvr_gpu_service.c` reaches it through a
  `config.mimus` pointer, as it reaches the compute and render components
  through `config.compute` and `config.render`, at seven points: object list,
  publication, READY poll, event forwarding, wait bound, submit window and
  failure. Images without the component pass a null pointer. An eighth change
  in the worker loop takes ticket `PVR_GPU_SERVICE_MIMUS_WAKE` as a wake that
  leaves `irq_wake_pending` alone.
- `platform/a733/gpu_mimus.c` (new) acquires the pool pages and builds the GPU
  address space with `pvr_gpu_vm`, the compute context with
  `pvr_compute_context.c` and the context-switch programs with
  `pvr_compute_programs.c`, as `gpu_compute_reserved.c` does for packages.
- `apps/os/placement.py` adds, per GPU row, the model region, weight offset,
  weight stride and K. OPEN admits each such tensor once with
  `pvr_mimus_tensor_open` and refuses when the table's `job_rows` exceeds the
  worker's own bound. Its rate seeds become the measured ones ("Budgets").
- `apps/os/identity.c`, under the option, zeroes the 64 KiB tag area and
  publishes it with `a733_cpu_coherence_publish`, as it publishes the models.
  The step runs once per boot at EL3 with translation off, before
  `a733_cached_prepare` builds the read-only map.

## Interrupts and polling

INTID 95 is routed at boot to the recognition core and stays disabled until
the service's first rearm. The handler masks it and posts one wake. Recognizer,
worker and interrupt share that core, so no completion needs an
inter-processor interrupt. Until SPI 63 delivery is qualified, the worker
bounds each wait while a job is in flight by the existing 4 ms
`PVR_GPU_SERVICE_JOB_POLL_US`, so a job wakes the worker
2 + ⌈job time / 4 ms⌉ times. Defining `PVR_GPU_SERVICE_INTERRUPT_QUALIFIED`
(new; set once the board shows INTID 95 for every job) bounds that wait by
the job deadline alone, which leaves two wake-ups per job.

The recognizer never polls: OPEN also finishes with a record on the
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
- **Boot.** The identity step zeroes the tag area and publishes it, 1,024
  lines once per boot. DRAM therefore holds no tag of an earlier boot at OPEN,
  and a tag that matches a sequence was written by this boot's job with that
  sequence.
- **Open.** OPEN cleans the job, program and context pages once. It does not
  clean the models, which boot publication covers, or the output region,
  because boot publication covers the tags and every job cleans and
  invalidates the bytes it reads.
- **Per job.** Maintenance is the kernel design's: three
  `aarch64_device_cache` calls with six `DSB SY`. The session component's
  client-ring write operation copies without a barrier, because `pvr_cccb`
  issues its release barrier before it publishes the write offset. The
  timeline is a plain load from uncached memory; the leading `DSB SY` of the
  completion's `DC CIVAC` orders the tag and sum reads after it. The kernel
  ring and firmware queue keep the transport of `pvr_ccb.c` and
  `gpu_memory.c` unchanged.

**Barriers per job.** Each `gpu_memory.c` access and each GPU register access
issues two `DSB SY`; the counts below follow from the code at the baseline
commit and assume an empty firmware queue on each check.

| Step | Source | `DSB SY` |
| --- | --- | --- |
| Slot to kick | Interrupt mask at step start (`gicv3_spi_mask`) | 1 |
| | Firmware-queue check: two interrupt-status reads (4), empty receive (5), kernel-ring return poll (6) | 15 |
| | Client-ring space check: acquire, two control reads | 3 |
| | Client-ring command writes | 0 |
| | Client-ring publish | 1 |
| | `pvr_ccb_submit`: control check (5), command and return writes (4), two releases, write offset (2), kick (2), completion barrier | 16 |
| | Total to the kick; then re-enable (`gicv3_spi_rearm`, 2) | 36 + 2 |
| Interrupt to post | Handler mask | 1 |
| | Firmware-queue check with the interrupt pending: as above, plus the interrupt clear (2) and the job's kernel-ring return (1) | 18 |
| | Total to the post; then re-enable | 19 + 2 |
| Per firmware event in a batch | `pvr_ccb_receive` | 11 |
| Per timed wake while the job runs | Mask, firmware-queue check, re-enable | 19 |

A job therefore costs 59 transport and 6 maintenance barriers, plus 19 per
timed wake and 11 per firmware event. The GPU runtime design
([gpu-runtime.md](gpu-runtime.md)) owns the reduction to lighter barriers in
`gpu_memory.c` and `gicv3_spi.c`, which needs the board qualification of
[shared-helpers.md](../shared-helpers.md); "Left out" gives its cost here.

## Synchronization across cores

All runtime work runs on the recognition core. The slot moves IDLE →
SUBMITTED (hook, release store) → TAKEN (worker, compare-and-swap with acquire)
→ IDLE (worker, before the post). The hook's failed send is the only other
transition, SUBMITTED → IDLE by compare-and-swap, so the hook and the worker
never both run a job. The worker posts the completion after the trailing
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
RAM with level-2 block descriptors only. With the 2 MiB stride the check
translates the first and the last 4 KiB page of each block: 12 translations
for the arena and 8 for the view (an EL0 read and an EL0 write probe per
page), 20 in all. The QEMU golden comparison of
`checks/device-memory-capacity.py`, including its read-only-view fixture,
fails if any entry becomes a page table. The asset checks and the default
build's 16 MiB check are unchanged.

## Build gate

`cmake/a733-os.cmake` sets `A733_GPU_OUTPUT_EL0_READ_ONLY` itself in the GPU
branch and refuses a user-supplied value. `A733_GPU_OUTPUT_CLEAN_INVALIDATE`
is deleted with its refusal and its cases in `checks/os-image-dispatch.py`,
because no source reads it. `os-image-link.py` checks in the built image
that `pvr_mimus_complete` passes operation 1 (clean and invalidate) to
`aarch64_device_cache`. The branch adds the GPU service, the session code, the
firmware (`SUNSTONE_GPU_FIRMWARE`, SHA-256 pinned as in the accelerator image)
and `SUNSTONE_A733_OS_GPU_LINEAR` as a target definition. `recorded.h`
declares 12 MiB when `SUNSTONE_A733_OS_GPU_LINEAR` is defined; the arena peak
is at most 2,784,048 bytes.

Until the board items on GPU cache invalidation at job start, cross-core code
writes and GPU read-only enforcement pass, the CMake branch refuses the option
without `SUNSTONE_A733_OS_GPU_VALIDATE`. In that validation build the hook
compares every GPU sum with an int64 sum that a scalar CPU loop computes from
the same codes and weights, before it returns the job to the engine. A
mismatch is handled as a missing tag. The check of the largest job
(62,447,616 MACs) takes an estimated 60 ms at 2,002 MHz and two cycles per
MAC.

## Failure classes

| Failure | Class (section 1.8) | Result | Retained |
| --- | --- | --- | --- |
| Preparation refused in `start` (pool, firmware hash, owner collision, GPU loss in the previous-boot report) | 1 | OPEN refused; CPU for the boot | Acquired pool pages |
| OPEN fails after power or firmware access | 3 | CPU for the boot | Everything the service holds; the GPU can write only the output region and firmware memory |
| Job refused before the first client-ring write, including a failed wake send | 1 | Completion REFUSED; slot IDLE; the hook computes the rows | Nothing |
| Tag missing, or in the validation build a sum that differs from the CPU's | 2 | INCOMPLETE record; `os_dispatch_incomplete` closes placement at once; the hook computes the job's rows | The session and the output region for the boot; the GPU stays powered and idle |
| After the client ring is published: deadline missed, CONTEXT_RESET event, service FAILED | 3 | Completion LOST; `os_dispatch_failed`, `board_fail`, watchdog reset | Everything |
| No completion | Unknown | Service window expires; watchdog reset | Everything |

`max_job_us` is a hang bound of 1 s. The largest bench job, with every
channel on the GPU, models at 26 ms with 200-clock loads and about 50 ms with
400-clock loads, and near 100 ms if fewer than 8 tasks stay resident. One
second leaves a tenfold margin inside the 3 s service window; board timing per
layer shape sets the tighter bound. No reset recovery exists, so the runtime
reports LOST and never RESET.

A GPU loss must not repeat after the watchdog reset. On LOST,
`board_resident_stop_detail` (`apps/os/services.c`) returns a GPU-loss detail,
which the supervising core's deliberate exit stores in its retained fault slot
([fault-capture.md](../fault-capture.md)). On the next boot `a733_fault_boot`
copies the slot into `a733_fault_previous` before any task runs, and `start`
refuses OPEN for that boot. That boot clears the slots and runs on the CPU, so
a later boot opens the GPU again. The RTC general-purpose registers are not
used, because [reset.md](../reset.md) records that their boot0 owners are
unknown.

## Budgets

Baselines are commit `0fd1b51`. "Host check" is `checks/pvr-mimus-service.c`
(new), which runs the service worker and the session component over the
bench recording's 204 jobs against the software firmware model of
`checks/pvr-compute-service.c`. Its firmware model completes each job after
the duration that `apps/os/placement.py` models for it, and wrap counters
count `aarch64_device_cache`, `aarch64_device_translate`, the barrier
operations, distributor accesses, critical sections, queue calls and timed
waits.

`placement.py` takes its rates from
[the kernel measurements](gpu-mimus-compute-measurements.md): 3.39 GPU
instructions per MAC (217 groups per 64 MACs) in place of 1.75, and 2.9
read-out instructions per output (37.6 M for 12,923,134 outputs) in place of
12. The current A76 table, with 62.9% of MACs on the GPU, models at 0.732 s
with these rates. The regenerated tables, as n_gpu and job_rows, are:

| Layer, K → N | A55 pair, 494 MHz | A76 pair, 2,002 MHz |
| --- | --- | --- |
| Subsampling convolution, 1,584 → 176 | 160, 256 | 80, 512 |
| Subsampling output, 3,520 → 176 | 160, 96 | 80, 224 |
| Feed-forward first linear, 176 → 704 | 624, 416 | 320, 800 |
| Feed-forward second linear, 704 → 176 | 160, 576 | 80, 1,184 |
| Attention projections and pointwise convolution 2, 176 → 176 | 160, 1,632 | 80, 3,264 |
| Pointwise convolution 1, 176 → 352 | 304, 832 | 160, 1,632 |
| Output head, 176 → 1,025 | 897, 256 | 449, 544 |

Every bench call stays one job, 204 in all. On the A76 table the jobs take
1.7 ms at the median and 7.5 ms at most, 0.510 s of GPU time in all.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Default OS images (supervised, resident, resident with microphone, resident with headset) | — | Loadable bytes identical apart from the 32-byte build identity | `llvm-objcopy -O binary` at both commits |
| Other images that link `drivers/gpu/` | — | Existing host checks pass; code changes only at the eight service points | Their host checks |
| GPU linear image | Stops at the arena assertion | Links; link check passes; `.text` + `.rodata` at most 1.75 MiB of the 2 MiB read-only block (base 888 KiB) | Build, `llvm-size -A` |
| Translations in `mimus_board_pins`, arena and view | 5,120 | 20 | Counter in `checks/os-image-dispatch.c` |
| Per job, slot write to kick: translations, hashes, allocations, tick waits in the session component | No path | 0, 0, 0, 0; at most 13 compares | Host check |
| Worker steps from slot write to kick | No path | 1, with one mask and re-enable and one firmware-queue check; barriers, translations, distributor accesses and critical sections reported | Host check |
| `DSB SY` per job | Compute path: 16 per submit, 11 per event + 5 per drain, 2 per cache call | 6 in maintenance; 59 in transport, plus 19 per timed wake and 11 per firmware event; any increase fails | Host check, per source |
| Queue traffic per job | No path | Two 56-byte wake messages (hook, interrupt) and one 8-byte completion record: six queue operations, each one kernel-lock critical section | Host check |
| Wake-ups per job | No path | Recognizer 1; worker 2 + ⌈job time / 4 ms⌉, 633 per recording on the A76 table; 2 with `PVR_GPU_SERVICE_INTERRUPT_QUALIFIED`; 0 inter-processor interrupts | Host check |
| Cache lines at boot and OPEN | Dispatcher design: 294,912 at OPEN | 1,024 per boot in the identity step; at OPEN, 64 per job, program and context page | Host check; `checks/os-image-dispatch.c` |
| Hang detection | None | `max_job_us` (1 s) plus at most one 1 ms tick after the kick | Host check, timeline never updated |
| Pool pages for the GPU | 0 | At most 80 WB and 52 uncached pages, allocated after USB2 and audio in the image's admission order | Replay case in `checks/os-image-dispatch.c` |
| Linear stage, bench recording | 14.894 s on one A55, measured | A76 pair: 0.535 s, 45.4% of MACs on the GPU, regression bound 0.54 s. A55 pair: 1.147 s, 90.2%, bound 1.15 s | `placement.py --summary` (model); board |
| Recognition, PCM to text, bench recording (8.87 s of audio) | 19.476 s on one A55, measured | A76 pair: 0.818 s, 0.092 times the audio duration. A55 pair: 3.438 s, 0.388 times. Goal 0.4435 s, 0.05 times: **not met** | Linear-stage model plus the other stages scaled from the A55 measurement; board |

The recognition row adds the linear stage to attention and the remaining
stages, which took 4.582 s on one A55 at 494 MHz ([mimus.md](../mimus.md)).
Two A76 cores at 2,002 MHz, each doing twice an A55's work per clock and
scaling perfectly, would take 0.283 s for them; the A55 pair would take
2.291 s.

The host check also reports, without a target, the translations and
`external()` scans in `gpu_memory.c`, the translations in `gpu_backend.c`
and the counter-clock lock acquisitions per job. Read from the code, a job
makes about 47 translations, 26 scans and 12 lock acquisitions.

### The recognition goal is not met

The goal allows 0.4435 s from PCM to text for the bench recording. With the
other stages at 0.283 s on the A76 pair, the linear stage may take at most
0.16 s; the best split takes 0.535 s. The A55 pair cannot meet the goal at
all, because its other stages alone take 2.291 s, so admission of the A76
cores is the first requirement. The same model on the A76 pair gives:

| Lever (owner) | Linear stage | Recognition |
| --- | --- | --- |
| None | 0.535 s | 0.818 s, 0.092× |
| GPU at 800 MHz (GPU power and thermal owner) | 0.386 s | 0.669 s |
| 1.75 GPU instructions per MAC (kernel design) | 0.395 s | 0.678 s |
| NEON int32-lane accumulation at 4× the CPU rate (mimus engine) | 0.219 s | 0.502 s |
| 800 MHz and 4× | 0.187 s | 0.470 s, 0.053× |
| 800 MHz, 1.75 instructions per MAC and 4× | 0.157 s | 0.440 s, 0.0496× |

Only all three levers together reach the goal, and only if the other stages
scale perfectly. No GPU operating point above 400 MHz is recorded, the
gpu-mimus measurements record no program below 3.39 instructions per MAC, and
the CPU factor is unmeasured.

## Reused and removed

Reused: the GPU service and its pacing, start, stop, client ring, firmware
queue and interrupt path; the platform binding, backend, firmware reservation
and VM publication of the accelerator image; the mimus session code; the
placement table and dispatcher; the retained fault slots and previous-boot
report. Removed: `a733_gpu_mimus_worker`; `A733_GPU_OUTPUT_CLEAN_INVALIDATE`
and the user-supplied `A733_GPU_OUTPUT_EL0_READ_ONLY`; per-page translations
in the GPU branch of `mimus_board_pins`; OPEN's model and output-region
cleans; the dispatcher's condition that a missing tag waits for a stop
acknowledgement (`os_dispatch_incomplete` in `dispatch.h`).

## Left out

- **Lighter transport barriers** (section 1.2). 59 per job plus 19 per timed
  wake come to 16,311 per recording. At an estimated 100 to 500 cycles per
  `DSB SY` at 2,002 MHz, within the tens to hundreds of cycles of section 1.2,
  they take 0.8 to 4.1 ms, below 1% of the 0.535 s linear stage. The change
  belongs to the GPU runtime design, changes every GPU image and returns here
  when the board-measured cost exceeds 2% of the linear stage (10.7 ms).
- **Backend per-access translations and checks** (section 1.1). The counts
  above, at an assumed 0.1 µs per translation or lock and 25 ns per scan,
  give about 6.6 µs per job and 1.3 ms per recording. They return when the
  host check's counts, at a board-measured cost, exceed 2% of the linear stage
  (10.7 ms).
- **Compiling out unused service components.** The image links the compute,
  render and free-list objects that `pvr_gpu_service.c` references; the
  `.text` budget absorbs them.
- **The second inference worker.** `apps/mimus/parallel_runtime.cc` installs
  `ParallelFor`; it is SunstoneOS code in this repository and needs no engine
  change. It lands with the engine hook, which is its first GPU consumer.
- **Reset recovery and power-off when idle**, as in the dispatcher design.
  After a missing tag the GPU stays powered with no jobs for the rest of the
  boot; that costs power and no recognition time.

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
- invalidation of GPU and system-level cache lines at job start. Codes and job
  words are rewritten at the same addresses for every job, so a stale line
  would give wrong sums behind matching tags;
- enforcement of read-only access by the GPU MMU on the model, arena and job
  page mappings;
- execution of the mimus program and SPI 63 delivery;
- retention of the fault slots across a WDT0 reset, a board item of
  [fault-capture.md](../fault-capture.md).

If the board shows stale lines at job start, the arena and the job page are
mapped with the GPU's system-level-cache bypass instead; no source here gives
that encoding.

## Board evidence that would confirm the design

- OPEN reaches READY on a cold and a warm boot, and the tag area reads zero
  at OPEN.
- In the validation build with the engine hook, the bench recording's 204
  jobs give GPU sums equal to the CPU's, and text and CRCs equal to the host
  reference, while the second inference worker rewrites codes at the same
  addresses. INTID 95 arrives on the recognition core for every job, with
  submit-to-kick and completion-to-wake times. GPU time per layer shape
  replaces the model's rates.
- A GPU store probe to a read-only mapping faults.
- An injected missing tag ends in an INCOMPLETE record and CPU-only
  recognition.
- An injected hang ends in LOST, a watchdog reset and a CPU-only next boot.
  This case is an intentional watchdog expiry, which AGENTS.md defers until
  the reset destination and recovery are qualified
  ([watchdog-expiry.md](../watchdog-expiry.md)).

## Changes outside this package

None needs `primary.patch`: the interrupt table already leaves enabling to the
device owner, the read-only view and EL0 translation exist, and
`identity.c`, `services.c` and `dispatch.h` lie under `apps/os/`. Other
owners: the GPU runtime owner reviews the eight service points in
`pvr_gpu_service.c`.

## Changes needed in the mimus engine

1. `TensorView::ordinal`, set by `Model::Load`: base tensors in file order
   with overlay replacements in place, then the overlay's new tensors.
2. An installable exact-sum hook on AArch64, declared in a header the
   recognizer can test, and called from `Linear`, `LinearAccumulate`,
   `LinearProduced`, `LinearProducedParallel` and `LinearProducedAccumulate`
   for int8 weights and int16 codes. `begin` receives the ordinal, rows, K,
   N, code pointer and row stride, and returns `n_gpu`; the engine computes
   channels `[n_gpu, N)`. `finish`, called until it returns no rows, returns
   one job's first row, row count and the address, row stride and width of
   its sums. The engine converts those sums with its own read-out order, that
   of `gemm_combine_portable.cc` (scales, bias, alpha, residual), and computes
   channels `[0, n_gpu)` of the rows no job covered. The S3 functions
   `MimusGemmS16Raw` and `MimusRawReadout*` do not apply, because they read
   40-bit accumulator tile images (`gemm_epilogue.h`).
3. Under an active `ScopedBlockSink` (`kernels.h`), the GPU's columns follow
   the sink contract. If the sink has `elementwise`, as the feed-forward SiLU
   sink does, the engine applies it to each GPU column as it stores it, after
   the CPU columns' `done()` calls have returned. Otherwise, as for the GLU
   sink of pointwise convolution 1, the engine holds `done()` for the call and
   calls `done(0, N)` once on worker 0 after every column of every row is
   final. That sink reads column c − d when it is given column c ≥ d, so it
   needs the GPU's low columns before any CPU column is reported.
   A host test runs both sinks with `n_gpu < N` and `n_gpu = N` and compares
   every output bit for bit with the CPU path.
4. Activation codes of every call that reaches the hook allocated from the
   arena, with a row stride that is a multiple of 4 bytes and at least 2K.
5. Subsampling rows produced in blocks of 224 (the last of 184) into one
   709,632-byte code buffer in place of 160-row calls.
