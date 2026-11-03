# Design: running mimus linear layers on the GPU from the integrated OS image

Status: **host-tested**. The hook, dispatcher and placement table are implemented in the mimus checkout and pass their host checks and QEMU runs. No board has run them (**open**: board qualification), and the modeled recognition time misses the goal.

The mimus linear layers run on the GPU through the [generic compute queue](../gpu-mimus-service.md). The OS image links the queue, the GPU service and the thermal and CPU-clock owners. The hook, the dispatcher, the placement generator and the recognizer patch belong to the mimus checkout recorded in [apps/mimus/source.json](../../apps/mimus/source.json) (paths beginning with `sunstoneos/` below). The design follows [performance-architecture.md](../performance-architecture.md) and completes the GPU option of [recognition-dispatch.md](recognition-dispatch.md), whose placement table, dispatcher and supervision it uses. The kernel, job words, cache maintenance and completion tags are in [gpu-mimus.md](../gpu-mimus.md); [gpu-mimus-service.md](../gpu-mimus-service.md) gives the sequence of one job and is its only description.

The recognizer opens the GPU session only when the engine's header declares the hook (`os_gpu_open` in `sunstoneos/apps/os/gpu_hook.cc`). With an engine that lacks the header the GPU stays unpowered and every layer runs on the CPU.

With the measured GPU program no channel split meets the recognition goal of 0.05 times the audio duration (0.4435 s for the bench recording). The modeled linear stage takes 0.535 s on the A76 pair, and recognition takes 0.818 s, 0.092 times the audio duration. Sections "Limitations" and "Open work" give the levers.

## Data flow and ownership

The loader places the base and overlay models at `0x44000000` and `0x44800000`. The boot's identity step publishes them to the point of coherency at EL3, and the recognizer hashes them before it opens the session. Activation codes live in mimus's arena, the 12 MiB at `0x46000000`. The GPU writes one job's tags and sums into the 2 MiB output region at `0x46C00000`, which the CPU maps read-only at EL0 and EL1. The 2 MiB above it, up to `0x47000000`, is not leased. No weight, code or sum byte is copied.

| Range | GPU view | GPU page, access | CPU maintenance |
| --- | --- | --- | --- |
| Base and overlay models, 8 MiB each | `0x43000000` and `0x43800000` map to `0x44000000` and `0x44800000` | 2 MiB, read-only | None; published at boot |
| mimus arena (codes), 12 MiB | `0x46000000`, identity | 2 MiB, read-only | Clean the job's code extent per job |
| Output region: 64 KiB of tags, then sums | `0x46C00000`, identity | 2 MiB, read-write | Tags zeroed and published at boot; `DC CIVAC` over the job's tags and sums per job |

The recognizer task (EL0) owns the codes. The GPU service worker (EL1) is the only task that touches GPU registers, firmware queues and the cache lines of the session. The image layout keeps these ranges disjoint, which `checks/os-image-link.py` proves, and no pool lease covers them. A retained failure therefore retains them for the boot without a lease record.

In GPU addresses the arena ends where the output region begins. The kernel clamps every load to the job's last row and channel, so only a kernel defect reads past the arena; such a read reaches tags and sums that the GPU may already write. The design accepts these over-reads and leaves no unmapped gap.

## Components

- `sunstoneos/apps/os/gpu_hook.cc` is the hook the engine calls: `begin` writes one job into the queue and `finish` returns each job's first row, row count and sum address. `os_gpu_open` runs after the model hashes.
- `sunstoneos/apps/os/dispatch.c` and `dispatch.h` hold the placement lookup, the dispatch record (placed calls and refusals, written only by the recognizer) and the session state.
- `sunstoneos/apps/os/placement.py` generates, per GPU row, the model region, weight offset, weight stride, K and N (179 rows for the bench models) and the table of `n_gpu` and `job_rows` ([recognition-dispatch.md](recognition-dispatch.md)).
- `apps/os/gpu.c` constructs the queue, binds the GPU platform to `providers.shared` (resource manager client 7, PMIC, locks), creates the service worker at priority 4 on the recognition core, and bounds every OPEN and CLOSE ([gpu-recognition-wait.md](gpu-recognition-wait.md)). It performs no device access before the scheduler starts, and any refusal leaves the boot on the CPU.
- `apps/os/identity.c`, under the option, zeroes the 64 KiB tag area and publishes it with `a733_cpu_coherence_publish`. The step runs once per boot at EL3 with translation off, where every access is a Device access, so it makes aligned 64-bit volatile stores only.

## Interrupts and polling

INTID 95 (SPI 63) is routed at boot to the recognition core and stays disabled until the service's first rearm. The handler masks it and posts one wake. Recognizer, worker and interrupt share that core, so no completion needs an inter-processor interrupt. Until SPI 63 delivery is qualified, the worker bounds each wait while a job is in flight by `PVR_GPU_SERVICE_JOB_POLL_US` (4 ms), so a job wakes the worker 2 + ⌈job time / 4 ms⌉ times.

The recognizer never polls. Its waits are finite receives that record no service progress, so a worker that never answers expires the 3 s mimus service window and the watchdog resets the board.

## Cache policy

- **Boot.** The identity step zeroes the tag area with Device stores that reach DRAM directly and publishes it (1,024 lines cleaned, none invalidated). No cached copy can hold an earlier tag, because the loader runs with caches off, no CPU writes the area, and each job's completion cleans and invalidates its tags before the recognizer reads them. A matching tag was therefore written by this boot's job with that sequence.
- **Open.** OPEN cleans the job, program and context pages once. Boot publication covers the models and tags, and every job cleans and invalidates the bytes it reads.
- **Per job.** Three `aarch64_device_cache` calls with six `DSB SY`. The client ring write copies without a barrier because `pvr_cccb` issues its release barrier before it publishes the write offset. The timeline is a plain load from uncached memory; the leading `DSB SY` of the completion's `DC CIVAC` orders the tag and sum reads after it.

A job costs 59 transport and 6 maintenance barriers by the code reading, plus 19 per timed wake and 11 per firmware event; the QEMU host check counts 57, 6, 18 and 11 ([barrier record](../evidence/gpu-recognition.jsonl), id `gpu-recognition-barriers`). Lighter barriers in `gpu_memory.c` and `gicv3_spi.c` belong to [the GPU runtime design](gpu-runtime.md) and return here when the board-measured cost exceeds 2% of the linear stage (10.7 ms).

## Synchronization across cores

All runtime work runs on the recognition core. The slot moves IDLE to SUBMITTED (hook, release store), TAKEN (worker, compare-and-swap with acquire) and IDLE (worker, before the post). The hook's failed send is the only other transition, SUBMITTED to IDLE by compare-and-swap, so the hook and the worker never both run a job. The worker posts the completion after the trailing `DSB SY` of its maintenance, and the queue's critical section orders the post before the recognizer's tag reads. The second inference worker on the pair's other core writes codes before the hook's join, which the leading barrier of the code clean covers. It reads half of each job's sums after the completion's `DC CIVAC` has acted on every core's cache in the inner-shareable domain.

## Mapping check and build gate

`mimus_board_pins` checks the 12 MiB arena as EL1-writable and the 4 MiB view at `0x46C00000` as EL0-readable and writable at neither EL0 nor EL1. The check uses `a733_mapping_check_stride` at the link-time stride `a733_local_pool_stride` (2 MiB in images that link `platform/a733-smp-cached/memory.c`). With that stride it makes 6 translations for the arena and 12 for the view (an EL0 read, an EL0 write and an EL1 write probe on the first and last 4 KiB page of each block): 18 in all. The builder maps image RAM with level-2 block descriptors only, and the QEMU golden comparison of `checks/device-memory-capacity.py`, including its read-only-view fixture, fails if any entry becomes a page table.

The build sets `A733_GPU_OUTPUT_EL0_READ_ONLY` itself and refuses a user-supplied value. Every cache operation on the output region is clean-and-invalidate, because `DC IVAC` is checked as a write and faults on the read-only region; `checks/pvr-mimus-service.py` asserts this. `recorded.h` declares the 12 MiB arena, and the arena peak is at most 2,784,048 bytes. The recognizer patch asserts that the arena base is `0x46000000` and its size at most 12 MiB.

Until the board items on GPU cache invalidation at job start, cross-core code writes, GPU read-only enforcement and fault-slot retention across a WDT0 reset pass, the hook compares every GPU sum with an int64 sum that a scalar CPU loop computes from the same codes and weights (`SumsEqual`). The comparison is unconditional, and a mismatch is handled as a missing tag. It costs about 60 ms for the largest job (62,447,616 MACs) at 2,002 MHz, about 1.9 s per bench recording on the A76 pair and 15 s on the A55 pair at 494 MHz ([estimate](../evidence/gpu-recognition.jsonl), id `gpu-recognition-sum-validation`).

## Failure classes

[gpu-mimus-service.md](../gpu-mimus-service.md#failure-classes) is the one table of failures, their classes, results and retained state. `max_job_us` is a hang bound of 1 s, which is also the most the session's bind accepts, and a tenfold margin inside the 3 s service window for the largest bench job. No reset recovery exists, so the runtime reports LOST and never RESET.

A GPU loss must not repeat after the watchdog reset. On LOST, or when the recognizer's service window expires while it waits for OPEN or for a job's record, `board_resident_stop_detail` (`apps/os/services.c`) returns `OS_STOP_DETAIL_GPU_LOSS`, a value outside the service numbers 1 to `OS_SERVICES`. It reads the dispatcher's atomic session state, which the recognizer stores before its `board_fail` release store, so the value does not depend on `board_resident_renew`. The supervising core stores the detail in its retained fault slot ([fault-capture.md](../fault-capture.md)). On the next boot `a733_fault_boot` copies the slot into `a733_fault_previous` before any task runs, and `start` refuses OPEN for that boot. That boot clears the slots and runs on the CPU, so a later boot opens the GPU again. The RTC general-purpose registers are not used, because [reset.md](../reset.md) records that their boot0 owners are unknown.

## Engine requirements

The mimus engine (commit `9c57286b7e1e50cd2181438a2d9ab717102d288e`, which [apps/mimus/source.json](../../apps/mimus/source.json) pins and configuration requires of `MIMUS_DIR`) provides:

1. `TensorView::ordinal`, set by `Model::Load`: base tensors in file order with overlay replacements in place, then the overlay's new tensors.
2. An installable exact-sum hook on AArch64, declared in `mimus/asr/exact_sum_hook.h`, called from `Linear`, `LinearAccumulate`, `LinearProduced`, `LinearProducedParallel` and `LinearProducedAccumulate` for int8 weights and int16 codes. `begin` runs after every row's codes and scales for the call are final in the arena, after the `ParallelFor` join, and the streamed SRAM-ring path is off for hooked calls. It receives the ordinal, weight codes and stride, rows, K, N, and code pointer and stride, and returns `n_gpu`. The hook returns 0 unless the weights, stride, K and N equal the tensor admitted under that ordinal, because the four attention projections share K and N. The engine computes channels `[n_gpu, N)`. `finish` returns one job's first row, row count, and the address, row stride and width of its sums; the engine converts each sum to float and calls `kernels.cc` `Combine()` with its residual and sink handling, so no third copy of the arithmetic exists. It computes channels `[0, n_gpu)` of the rows no job returned.
3. Under an active `ScopedBlockSink`, an `elementwise` sink (the feed-forward SiLU) is applied to each GPU column as it is stored, after the CPU columns' `done()` calls. Otherwise (the GLU sink of pointwise convolution 1, which reads column c − d for column c ≥ d) the engine holds `done()` and calls `done(0, N)` once on worker 0 after every column of every row is final. A host test runs both sinks with `n_gpu < N` and `n_gpu = N` and compares every output bit with the CPU path.
4. Activation codes of every hooked call allocated from the arena, with a row stride that is a multiple of 4 bytes and at least 2K.
5. Subsampling rows produced in blocks of 224 (the last of 184) into one 709,632-byte code buffer.
6. `SetExactSumOrdinals`, which limits the hook to the placed tensors' calls so that every other call keeps the streamed row ring. `os_gpu_open` passes the bitmap of placed ordinals, and the hook removes itself with `SetExactSumHook(nullptr)` when placement closes for the boot.

## Qualified figures

All figures come from host builds, QEMU and the time model; none comes from a board.

| Figure | Value | Record |
| --- | --- | --- |
| Mapping-check translations at application start | 18 | `gpu-recognition-host-budgets` |
| Worker steps per job; queue operations per job | 2; 6 | `gpu-recognition-host-budgets` |
| Worker wake-ups per bench recording | 429 | `gpu-recognition-host-budgets` |
| Hang detection after the kick | 1,000,751 µs | `gpu-recognition-host-budgets` |
| Pool pages | 79 write-back, 51 uncached | `gpu-recognition-host-budgets` |
| Exact sums equal to int64 CPU sums | 5,870,398 | `gpu-recognition-host-other` |
| Linear stage, bench recording (model) | 0.535 s A76 pair, 1.117 s A55 pair | `gpu-recognition-host-budgets` |
| Recognition, bench recording (model) | 0.818 s A76 pair, 3.408 s A55 pair; goal 0.4435 s | `gpu-recognition-goal-levers` |
| Two-worker CPU-only instructions, hook engine against pinned engine | 17.64 G against 26.11 G | `gpu-recognition-engine-hook` |

The records are in [gpu-recognition.jsonl](../evidence/gpu-recognition.jsonl).

## Limitations

- The recognition goal is not met. The linear stage may take at most 0.161 s on the A76 pair; the best split takes 0.535 s. Only a GPU at 800 MHz, 1.75 GPU instructions per MAC and NEON int32-lane accumulation at four times the CPU rate together reach 0.440 s, and only if the other stages scale perfectly. The A55 pair cannot meet the goal because its other stages alone take 2.291 s, so A76 admission is the first requirement.
- With the sum comparison in place the GPU linear stage takes about 2.4 s, slower than the 0.920 s of CPU only.
- After a missing tag the GPU stays powered with no jobs for the rest of the boot, and no reset recovery or power-off when idle exists.
- The image links the compute, render and free-list objects that `pvr_gpu_service.c` references; the `.text` budget absorbs them.
- Backend per-access translations and checks cost about 6.6 µs per job and 1.3 ms per recording at assumed unit costs, below the 2% threshold.

## Open work

1. Run the board items. OPEN reaches READY on a cold and a warm boot with the tag area zero. The bench recording's 204 jobs pass the sum comparison with text and CRCs equal to the host reference while the second worker rewrites codes at the same addresses. INTID 95 arrives on the recognition core for every job. GPU time per layer shape replaces the model's rates.
2. Show that a GPU store to a read-only mapping faults, that the jobs run with the compute command's border-colour table at address zero without a page fault or context reset, and that a fault slot written before a WDT0 reset reads back after it.
3. Inject a missing tag (placement closes and recognition runs on the CPU) and a hang (LOST, watchdog reset and a CPU-only next boot; AGENTS.md defers intentional watchdog expiry until [watchdog-expiry.md](../watchdog-expiry.md) qualifies the reset destination).
4. Qualify 2 MiB GPU pages, `DC CIVAC` at EL1 on a read-only page, per-job device cache flushing before the timeline UPDATE, invalidation of GPU and system-level cache lines at job start, and enforcement of read-only access by the GPU MMU. If stale lines appear, map the arena and job page with the GPU's system-level-cache bypass; no source here gives that encoding.
5. Measure whether the firmware or TPU reads the border-colour table for a compute job that does not sample; if so, map a zeroed read-only table as the compute path does.

The board sources and the full prediction are in `gpu-recognition-board-confirmation`.
