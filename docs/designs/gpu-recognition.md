# Design: running mimus linear layers on the GPU from the integrated OS image

Status: **implemented** and host-modeled; no board has run it. It follows
[performance-architecture.md](../performance-architecture.md) and completes the
GPU option of [recognition-dispatch.md](../recognition-dispatch.md), whose
placement table, dispatcher and supervision it keeps. The GPU session code of
[gpu-mimus.md](../gpu-mimus.md) (admission, job words, cache maintenance, the
completion tags) is used unchanged; [gpu-mimus-service.md](../gpu-mimus-service.md)
describes the component that runs it inside the GPU service, and
[the measurements](gpu-recognition-measurements.md) compare the result with
the budgets below.

The image built with `SUNSTONE_A733_OS_GPU_LINEAR` configures, links and
passes its link check and host checks: `apps/mimus/recorded.h` declares a
12 MiB arena under the option, and the image links the GPU service, the
session component and the runtime functions below. Placing a call on the
GPU also needs a hook inside the mimus engine; "Changes needed in the mimus
engine" lists it and the engine commit that provides it. The recognizer
opens the GPU session only when the engine's header declares that hook, so
with an engine that lacks it the GPU stays unpowered and every layer runs on
the CPU.

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

Per job, the hook writes one job into a slot that the GPU service worker
takes and submits in one step; after the job's timeline update the worker
maintains the output region and posts an 8-byte record, and the hook checks
the tags and every sum before the engine reads the sums out.
[gpu-mimus-service.md](../gpu-mimus-service.md#one-job) gives the sequence and
its ordering, and is the one description of it.

## Runtime functions and where they live

- `apps/os/gpu.c` (new) composes the runtime from the board's providers:
  `a733_gpu_mimus_start(core)`, `a733_gpu_mimus_open()`,
  `a733_gpu_mimus_begin(job)`, `a733_gpu_mimus_finish(&completion)` and
  `a733_gpu_mimus_irq()`. `start` runs in the `gpu-linear` start entry before
  the scheduler. It admits each placed tensor once with
  `pvr_mimus_tensor_open` and refuses when the table's `job_rows` exceeds the
  worker's own bound. It binds the GPU platform to `providers.shared` (GPU
  resource manager client 7, PMIC, locks) and the power-domain owner's GPU_TOP
  row, prepares the firmware and session memory and calls
  `pvr_gpu_service_init` with priority 4 and affinity `1 << core`, binding the
  recognizer task as client 0. It performs no device access. It also reads
  the previous-boot report and makes OPEN refuse for this boot when that
  report holds a GPU loss ("Failure classes"). Every refusal returns 1 with
  OPEN refused, so the boot continues on the CPU.
- `apps/os/gpu_hook.cc` (new) is the hook the engine calls. `os_gpu_open`,
  which the recognizer calls after the model hashes, opens the session and
  installs the hook only when the engine's header exists. Without the header
  the `gpu-linear` start entry prepares nothing: no pool page, firmware check
  or worker exists for a session that cannot open.
- `a733_gpu_mimus_worker` is removed from `dispatch.h`. The service creates
  its own worker, and `os_gpu_linear_task_privileged` grants it EL1 through
  `pvr_gpu_service_task_privileged`, as the accelerator image does.
- `drivers/gpu/pvr_mimus_service.c` (new) is the session component of the GPU
  service: firmware objects, the client ring, the slot, the completion queue
  and the tensor table. `pvr_gpu_service.c` reaches it through a
  `config.mimus` pointer, as it reaches the compute and render components
  through `config.compute` and `config.render`. The changes are twelve: the
  object list, publication at OPEN, the core-mode admission before firmware
  start, the READY poll, event forwarding, the wait bound, the submit window,
  the progress identity, the end of a burst once the session has nothing to
  submit and the interrupt is re-enabled, failure, the configuration checks
  at init, and in the worker loop ticket `PVR_GPU_SERVICE_MIMUS_WAKE` as a
  wake that leaves `irq_wake_pending` alone. Images without the component pass a null
  pointer and do not link `pvr_mimus_service.c`; its functions are weak
  references in `pvr_gpu_service.c`, and no other image's source list
  changes.
- `platform/a733/gpu_mimus.c` (new) acquires the pool pages and builds the GPU
  address space with `pvr_gpu_vm`, the compute context with
  `pvr_compute_context.c` and the context-switch programs with
  `pvr_compute_programs.c`, as `gpu_compute_reserved.c` does for packages.
- `apps/os/placement.py` adds, per GPU row, the model region, weight offset,
  weight stride, K and N (179 rows for the bench models). Its rate seeds become
  the measured ones ("Budgets").
- `apps/os/identity.c`, under the option, zeroes the 64 KiB tag area and
  publishes it with `a733_cpu_coherence_publish`, as it publishes the models.
  The step runs once per boot at EL3 with translation off, before
  `a733_cached_prepare` builds the read-only map. There every access is a
  Device access, so it makes aligned 64-bit volatile stores only, as
  `a733_fault_boot` does: no `memset` and no `DC ZVA`.

## Interrupts and polling

INTID 95 is routed at boot to the recognition core and stays disabled until
the service's first rearm. The handler masks it and posts one wake. Recognizer,
worker and interrupt share that core, so no completion needs an
inter-processor interrupt. Until SPI 63 delivery is qualified, the worker
bounds each wait while a job is in flight by the existing 4 ms
`PVR_GPU_SERVICE_JOB_POLL_US`, so a job wakes the worker
2 + ⌈job time / 4 ms⌉ times. Once the board shows INTID 95 for every job, a
switch added with that evidence can bound the wait by the job deadline alone,
which leaves two wake-ups per job; it does not exist yet.

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
- **Boot.** The identity step zeroes the tag area with Device stores that
  reach DRAM directly, then publishes it, which cleans 1,024 lines to the
  point of coherency and invalidates none. No cached copy can hold an earlier
  tag, because the loader runs with caches off and no CPU writes the area, and
  each job's completion cleans and invalidates its tags before the recognizer
  reads them. A tag that matches a sequence was therefore written by this
  boot's job with that sequence.
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

**Barriers per job.** Each `gpu_memory.c` write and each GPU register access
issues two `DSB SY`, and each `gpu_memory.c` read one. The counts below were
read from the code at the baseline commit and assume an empty firmware queue
on each check. `burst()` would run further steps after the kick and after the
post, because the kernel ring's serial and the session's state change; the
service therefore ends a burst once a ready session has nothing to submit and
the interrupt is re-enabled, so a job takes two worker steps, the kick step
and the step that posts its record. The QEMU host check counts 57 per job in
transport, 18 per timed wake and 11 per firmware event
([the measurements](gpu-recognition-measurements.md)).

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
critical sections. The second inference worker on the pair's other core
writes codes before the hook's join; the kernel design's leading barrier on
the code clean covers that ordering. That worker also reads half of each
job's sums. The completion's `DC CIVAC` to the point of coherency acts on
every core's cache in the inner-shareable domain before the record is
posted, and the engine's hand-off of the rows orders the worker's reads
after the record.

## Mapping check at application start

With the option, `mimus_board_pins` checks the 12 MiB arena as EL1-writable
with `a733_mapping_check_stride`, and the 4 MiB view at `0x46C00000` as
EL0-readable and writable at neither EL0 nor EL1, at the link-time stride
`a733_local_pool_stride` (2 MiB in images that link
`platform/a733-smp-cached/memory.c`, 4 KiB elsewhere). That builder maps image
RAM with level-2 block descriptors only. With the 2 MiB stride the arena
check translates one page per block, 6 translations, and the view check the
first and the last 4 KiB page of each block with an EL0 read, an EL0 write
and an EL1 write probe, 12 translations: 18 in all. The QEMU golden comparison of
`checks/device-memory-capacity.py`, including its read-only-view fixture,
fails if any entry becomes a page table. The asset checks and the default
build's 16 MiB check are unchanged.

## Build gate

`cmake/a733-os.cmake` sets `A733_GPU_OUTPUT_EL0_READ_ONLY` itself in the GPU
branch and refuses a user-supplied value. `A733_GPU_OUTPUT_CLEAN_INVALIDATE`
is deleted with its refusal and its cases in `checks/os-image-dispatch.py`,
because no source reads it. `checks/pvr-mimus-service.py` asserts that every
cache operation on the output region is clean-and-invalidate, because
`DC IVAC` is checked as a write and faults on the read-only region. The
branch adds the GPU service, the session code, the
firmware (`SUNSTONE_GPU_FIRMWARE`, SHA-256 pinned as in the accelerator image)
and `SUNSTONE_A733_OS_GPU_LINEAR` as a target definition. `recorded.h`
declares 12 MiB when `SUNSTONE_A733_OS_GPU_LINEAR` is defined; the arena peak
is at most 2,784,048 bytes.

Until the board items on GPU cache invalidation at job start, cross-core code
writes, GPU read-only enforcement and fault-slot retention across a WDT0
reset pass, the hook compares every GPU sum with an int64 sum that a scalar
CPU loop computes from the same codes and weights, before it returns the job
to the engine; the comparison is unconditional and has no build switch. A
mismatch is handled as a missing tag. At two cycles per MAC the check costs about 60 ms
for the largest job (62,447,616 MACs) at 2,002 MHz, about 1.9 s per bench
recording on the A76 pair (1.89 G GPU MACs) and about 15 s on the A55 pair at
494 MHz (3.76 G GPU MACs).

## Failure classes

[gpu-mimus-service.md](../gpu-mimus-service.md#failure-classes) lists each
failure with its class, result and what stays retained, and is the one table
of them.

`max_job_us` is a hang bound of 1 s, which is also the most the session's
bind accepts. The largest bench job, with every
channel on the GPU, models at 26 ms with 200-clock loads and about 50 ms with
400-clock loads, and near 100 ms if fewer than 8 tasks stay resident. One
second leaves a tenfold margin inside the 3 s service window; board timing per
layer shape sets the tighter bound. No reset recovery exists, so the runtime
reports LOST and never RESET.

A GPU loss must not repeat after the watchdog reset. On LOST, or when the
recognizer's service window expires while it waits for OPEN or for a job's
record, `board_resident_stop_detail` (`apps/os/services.c`) returns the
GPU-loss detail `OS_STOP_DETAIL_GPU_LOSS`, a value outside the service numbers
1 to `OS_SERVICES`. It reads the dispatcher's atomic session state, which the
recognizer stores before its `board_fail` release store and the supervisor
loads after `board_live`'s acquire of the failure, so the value does not
depend on `board_resident_renew`, which `supervise()` never reaches after a
failure. The supervising core's deliberate exit stores the detail in its
retained fault slot ([fault-capture.md](../fault-capture.md)). On the next boot `a733_fault_boot`
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
instructions per MAC (217 groups per 64 MACs) in place of 1.75, and 3.0
read-out instructions per GPU output, the engine's compiled read-out counted
on the placed shapes ([the measurements](gpu-recognition-measurements.md#with-the-engines-hook)),
in place of 12. Both inference workers read each job out, half of its rows
each. The regenerated tables, as n_gpu and job_rows, are:

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
| Other images that link `drivers/gpu/` | — | Existing host checks pass; code changes only at the twelve service points | Their host checks |
| GPU linear image | Stops at the arena assertion | Links; link check passes; `.text` + `.rodata` at most 1.75 MiB of the 2 MiB read-only block (base 888 KiB) | Build, `llvm-size -A` |
| Translations in `mimus_board_pins`, arena and view | 5,120 | 18, with an EL1 write probe on each view page checked | Counter in `checks/os-image-dispatch.c` |
| Per job, slot write to kick: translations, hashes, allocations, tick waits in the session component | No path | 0, 0, 0, 0; at most 13 compares | Host check |
| Worker steps from slot write to kick | No path | 1, with one mask and re-enable and one firmware-queue check; barriers, translations, distributor accesses and critical sections reported | Host check |
| `DSB SY` per job | Compute path: 16 per submit, 11 per event + 5 per drain, 2 per cache call | 6 in maintenance; 59 in transport, plus 19 per timed wake and 11 per firmware event; any increase fails | Host check, per source |
| Queue traffic per job | No path | Two 56-byte wake messages (hook, interrupt) and one 8-byte completion record: six queue operations, each one kernel-lock critical section | Host check |
| Wake-ups per job | No path | Recognizer 1; worker 2 + ⌈job time / 4 ms⌉, 633 per recording on the A76 table; 2 once a switch added with SPI 63 board evidence bounds the wait by the job deadline; 0 inter-processor interrupts | Host check |
| Cache lines at boot and OPEN | Dispatcher design: 294,912 at OPEN | 1,024 per boot in the identity step; at OPEN, 64 per job, program and context page | Host check; `checks/os-image-dispatch.c` |
| Hang detection | None | `max_job_us` (1 s) plus at most one 1 ms tick after the kick | Host check, timeline never updated |
| Pool pages for the GPU | 0 | At most 80 WB and 52 uncached pages, allocated after USB2 and audio in the image's admission order | Replay case in `checks/os-image-dispatch.c` |
| Linear stage, bench recording | 14.894 s on one A55, measured. Two-core QEMU image, CPU only: 14.73 G instructions retired on both cores with the hook engine, 23.23 G with the pinned engine | A76 pair: 0.535 s, 45.4% of MACs on the GPU, regression bound 0.54 s. A55 pair: 1.117 s, 90.2%, bound 1.12 s. These are issue-bound model figures without the hook's validation of every sum: they bound `placement.py --summary`, and the board needs its own measurement; with 200-clock and 400-clock GPU loads the A76 pair models at 0.666 s and 0.818 s | `placement.py --summary` (model); board |
| Recognition, PCM to text, bench recording (8.87 s of audio) | 19.476 s on one A55, measured. Two-core QEMU image, CPU only: 17.64 G instructions retired on both cores with the hook engine, 26.11 G with the pinned engine | A76 pair: 0.818 s, 0.092 times the audio duration. A55 pair: 3.408 s, 0.384 times. Goal 0.4435 s, 0.05 times: **not met** | Linear-stage model plus the other stages scaled from the A55 measurement; board |

The recognition row adds the linear stage to attention and the remaining
stages, which took 4.582 s on one A55 at 494 MHz ([mimus.md](../mimus.md)).
Two A76 cores at 2,002 MHz, each doing twice an A55's work per clock and
scaling perfectly, would take 0.283 s for them; the A55 pair would take
2.291 s.

The QEMU figures are instructions retired on both cores, not elapsed time;
[the measurements](gpu-recognition-measurements.md#with-the-engines-hook)
give their conditions and why the modeled rows stay derived from the
single-A55 board baseline.

The host check also reports, without a target, the translations and
`external()` scans in `gpu_memory.c`, the translations in `gpu_backend.c`
and the counter-clock lock acquisitions per job. Read from the code, a job
makes about 47 translations, 26 scans and 12 lock acquisitions.

### The recognition goal is not met

The goal allows 0.4435 s from PCM to text for the bench recording. With the
other stages at 0.283 s on the A76 pair, the linear stage may take at most
0.161 s; the best split takes 0.535 s, and recognition at 0.818 s is 1.84
times the goal (0.092 against 0.05 times the audio duration). The A55 pair
cannot meet the goal at all, because its other stages alone take 2.291 s, so
admission of the A76 cores is the first requirement. The same model on the
A76 pair gives:

| Lever (owner) | Linear stage | Recognition |
| --- | --- | --- |
| CPU only, no GPU (the CPU baseline of the model) | 0.920 s | 1.203 s, 0.136× |
| None | 0.535 s | 0.818 s, 0.092× |
| GPU at 800 MHz (GPU power and thermal owner) | 0.385 s | 0.668 s |
| 1.75 GPU instructions per MAC (kernel design) | 0.394 s | 0.677 s |
| NEON int32-lane accumulation at 4× the CPU rate (mimus engine) | 0.220 s | 0.503 s |
| 800 MHz and 4× | 0.187 s | 0.470 s, 0.053× |
| 800 MHz, 1.75 instructions per MAC and 4× | 0.157 s | 0.440 s, 0.0496× |

Only all three levers together reach the goal, and only if the other stages
scale perfectly. No GPU operating point above 400 MHz is recorded, the
gpu-mimus measurements record no program below 3.39 instructions per MAC, and
the CPU factor is unmeasured.

The GPU rows exclude the hook's validation of every GPU sum (`SumsEqual` in
`apps/os/gpu_hook.cc`), a scalar loop on the recognizer core alone that
costs about 1.9 s per recording on the A76 pair. While it stays, the GPU
linear stage takes about 2.4 s, slower than the 0.920 s of CPU only;
splitting it across both workers or vectorizing it would shorten that until
the board items let it go. The read-out's memory traffic, about 13 GB/s from
just-invalidated lines on the A76 pair, is unmeasured until the board run.

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
  [fault-capture.md](../fault-capture.md), which the option's gate includes;
- a compute command without a border-colour table (address zero): the
  program samples nothing, and no source states that the firmware or the
  TPU reads the table of a compute job that does not sample. If either reads
  it, the read reaches an unmapped GPU address, the GPU raises a page fault,
  the firmware resets the context and the job ends LOST; the fix is a zeroed
  read-only table mapped as the compute path maps its own.

If the board shows stale lines at job start, the arena and the job page are
mapped with the GPU's system-level-cache bypass instead; no source here gives
that encoding.

## Board evidence that would confirm the design

- OPEN reaches READY on a cold and a warm boot, and the tag area reads zero
  at OPEN.
- With the engine hook, the bench recording's 204 jobs pass the hook's
  comparison of every GPU sum with the CPU's, and text and CRCs equal to the host
  reference, while the second inference worker rewrites codes at the same
  addresses. INTID 95 arrives on the recognition core for every job, with
  submit-to-kick and completion-to-wake times. GPU time per layer shape
  replaces the model's rates.
- A GPU store probe to a read-only mapping faults.
- The bench jobs run with the compute command's border-colour table at
  address zero and raise no GPU page fault or context reset.
- A fault slot written before a WDT0 reset reads back after it.
- An injected missing tag closes placement and the recognition runs on the
  CPU.
- An injected hang ends in LOST, a watchdog reset and a CPU-only next boot.
  This case is an intentional watchdog expiry, which AGENTS.md defers until
  the reset destination and recovery are qualified
  ([watchdog-expiry.md](../watchdog-expiry.md)).

## Changes outside this package

The interrupt table already leaves enabling to the device owner, the
read-only view and EL0 translation exist, and `identity.c`, `services.c` and
`dispatch.h` lie under `apps/os/`. The one edit outside this package is
`checks/index.json`, regenerated by `checks/check-index.py --write` for the
two new checks, and a comment in `platform/a733-smp-cached/memory.c` that the
identity step's stores to the tag area make outdated. Other owners: the GPU
runtime owner reviews the service points in `pvr_gpu_service.c`, and may
merge the session's client-ring callbacks in `pvr_mimus_service.c` with the
near-identical ones of `pvr_compute_service.c`.

## Changes needed in the mimus engine

mimus commit `9c57286b7` implements all six; the image build's pinned engine
must move to it before any call reaches the GPU.

1. `TensorView::ordinal`, set by `Model::Load`: base tensors in file order
   with overlay replacements in place, then the overlay's new tensors.
2. An installable exact-sum hook on AArch64, declared in a header the
   recognizer can test (`mimus/asr/exact_sum_hook.h`, as
   `checks/gpu-recognition-hook.h` gives it), and called from `Linear`,
   `LinearAccumulate`, `LinearProduced`, `LinearProducedParallel` and
   `LinearProducedAccumulate` for int8 weights and int16 codes. `begin` is
   called only after every row's codes and scales for the call are final in
   the arena, after the `ParallelFor` join; the streamed SRAM-ring path is
   off for hooked calls. It receives the ordinal, the weight codes and their
   row stride, rows, K, N, code pointer and row stride, and returns `n_gpu`;
   the hook returns 0 unless the weights, their stride, K and N equal the
   tensor admitted under that ordinal, because the four attention
   projections share K and N and an ordinal that names another of them would
   otherwise use the wrong weights unnoticed; the engine computes channels
   `[n_gpu, N)`. `finish`, called until it returns false, returns one job's
   first row, row count and the address, row stride and width of its sums.
   The engine converts each sum to float and calls `kernels.cc` `Combine()`
   with its existing residual and sink handling, as `CombinedSums` does, so no
   third copy of the arithmetic exists. It computes channels `[0, n_gpu)` of
   the rows no job returned. `MimusGemmS16Raw` is portable and writes 40-bit
   tiles, and only `MimusRawReadoutF32C` and `MimusRawReadoutF40C` are S3
   assembly over those tiles (`gemm_epilogue.h`), so neither applies to the
   GPU's int32 and int64 sums.
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
6. `SetExactSumOrdinals` (`kernels.h`), which limits the hook to the placed
   tensors' calls, so that every other call keeps the streamed row ring and
   internal-arena codes. `os_gpu_open` passes the generated bitmap of placed
   ordinals, and the hook removes itself with `SetExactSumHook(nullptr)` when
   placement closes for the boot.
