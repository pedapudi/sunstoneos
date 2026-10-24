# The mimus session in the GPU service

The integrated OS image's GPU option ([recognition-dispatch.md](recognition-dispatch.md))
runs the exact integer sums of mimus linear layers on the GPU. This
component gives the session code of [gpu-mimus.md](gpu-mimus.md) (job words,
cache maintenance, completion tags) a life inside the GPU service
([gpu-service.md](gpu-service.md)): firmware objects, a client ring, a
timeline, one job slot shared with the recognizer and a completion queue.
The [design](designs/gpu-recognition.md) states the budgets and
[the measurements](designs/gpu-recognition-measurements.md) compare the
result with them.

Status: **implemented** and host-modeled. The GPU option's image links and
passes its link check and host checks. No board has run it, and no call
reaches the GPU until the mimus engine has its exact-sum hook (the design's
"Changes needed in the mimus engine").

## Parts

| Part | Source | Role |
| --- | --- | --- |
| Session component | [pvr_mimus_service.c](../drivers/gpu/pvr_mimus_service.c), [pvr_mimus_service.h](../include/pvr_mimus_service.h) | Slot, client ring, timeline, completion queue, failure |
| Service points | [pvr_gpu_service.c](../drivers/gpu/pvr_gpu_service.c) | `config.mimus`: objects, publication, mode admission, READY poll, events, wait bound, submit window, progress, failure, job wake |
| Native construction | [gpu_mimus.c](../platform/a733/gpu_mimus.c), [a733_gpu_mimus.h](../include/a733_gpu_mimus.h) | Pool pages, GPU address space, context-switch programs, compute context |
| Runtime | [gpu.c](../apps/os/gpu.c) | Start, OPEN, per-job begin and finish, INTID 95, tensor admission |
| Engine hook | [gpu_hook.cc](../apps/os/gpu_hook.cc) | Splits a call into jobs, checks tags, hands sums to the engine |

## Address space and memory

| GPU range | Physical | Page, access | Owner of its bytes |
| --- | --- | --- | --- |
| `0x43000000`, `0x43800000`, 8 MiB each | Base and overlay models at `0x44000000`, `0x44800000` | 2 MiB, read-only | Loader; published at boot |
| `0x46000000`, 12 MiB | Same: mimus's arena with the codes | 2 MiB, read-only | Recognizer |
| `0x46C00000`, 2 MiB | Same: 64 KiB of tags, then sums | 2 MiB, read-write | GPU; read-only at EL0 and EL1 |
| PDS heap `+0x1e0000`; USC heap `+0x100000`, `+0x101000` | Job page; two program pages | 4 KiB, read-only | Worker |
| Context-switch programs at the compute heap addresses | 18 pages | 4 KiB; shared and resume pages read-write | Worker |

The job and program pages share the first 2 MiB of their heaps with the
context-switch programs, because every 2 MiB region the session maps costs one
4 KiB table page: the address space needs 24 table pages. The session takes
21 write-back pages and the tables from the write-back pool, and six uncached
pages (compute context, register state, client-ring control, client ring,
memory context, timeline) from the uncached pool, after USB2 and audio.
With the firmware's 34 and 45 pages the GPU holds 79 write-back and 51
uncached pages. Nothing is released; a failure retains every page.

## One job

1. The hook ([gpu_hook.cc](../apps/os/gpu_hook.cc)) runs in the recognizer
   task at EL0. It accepts a call only when the engine's tensor ordinal,
   weight address, weight stride, K and N all match the tensor admitted at
   start under that ordinal, and the codes lie in the arena span. It writes
   the job (ordinal, code offset, rows, row stride) into the slot, stores
   SUBMITTED with release order and sends the worker a wake message with a
   wait of zero ticks: client `PVR_GPU_SERVICE_CLIENTS` and ticket
   `PVR_GPU_SERVICE_MIMUS_WAKE` (1). Ticket 0 stays the interrupt handler's
   wake, and only that message clears `irq_wake_pending`. If the send fails,
   the hook moves the slot from SUBMITTED to IDLE by compare-and-swap. When
   the swap succeeds the job is refused and the engine computes its rows;
   when the worker has already taken the slot, the hook waits for the record
   as usual. The engine then computes its own channels.
2. The worker, one priority above the recognizer on the same core, takes the
   slot by compare-and-swap from SUBMITTED to TAKEN with acquire order and
   copies it once into its own memory; every later check and use reads that
   copy, because `pvr_mimus_submit` reads the code offset twice. In one step,
   under one interrupt mask and re-enable, it looks up the admitted tensor
   (one bounds compare), assigns the next sequence, checks that the timeline
   holds the previous sequence, as `pvr_compute_service` does, and calls
   `pvr_mimus_submit`. It then writes the 96-byte compute command, which
   `pvr_mimus_session_command` wrote once at OPEN into a zeroed buffer, and
   an 8-byte timeline UPDATE carrying the sequence to the client ring,
   publishes the ring and submits the kick through the kernel ring. A refusal
   before the ring write posts REFUSED and leaves the slot IDLE. OPEN writes
   timeline 0, and sequences start at 1. After a step that leaves the session
   with nothing to submit and the interrupt re-enabled, the worker runs no
   further step until the next wake.
3. On INTID 95, or at the 4 ms bound of each wait while the job runs, the
   worker drains firmware events. When the kick's return is collected and the
   timeline reaches the sequence, it calls `pvr_mimus_complete` (`DC CIVAC`
   over the job's tags and sums), sets the slot IDLE and posts an 8-byte
   record (sequence, OK) to the one-entry completion queue.
4. The hook receives the record, requires the sequence after the previous
   OK record's, checks every tag with `pvr_mimus_tags_complete` and compares
   every sum with an int64 sum that a scalar CPU loop computes from the same
   codes and weights. It hands the engine the job's rows and the address, row
   stride and width (4 or 8 bytes) of its sums; the engine reads them out with
   its own code and then asks for the call's next job, which reuses the
   output region.

The hang bound `max_job_us` is 1 s from the kick, the most that bind accepts.
A missed deadline, a context-reset event, a timeline value other than the
previous sequence before a submit or other than the previous or current one
while a job runs, a failed completion maintenance, or any service failure
posts LOST for an owed record and sets the lost flag, which makes later
submits return -1.

## Failure classes

Classes follow section 1.8 of
[performance-architecture.md](performance-architecture.md).

| Failure | Class | Result | Retained |
| --- | --- | --- | --- |
| Start refused: previous-boot GPU loss, owner or client collision, tensor admission, pools, firmware | 1 | OPEN refused; CPU for the boot | Acquired pool pages |
| OPEN fails after power or firmware access | 3 | CPU for the boot | Everything the service holds; the GPU can write only the output region and firmware memory |
| Job refused before the first client-ring write, including a failed wake send | 1 | REFUSED; slot IDLE; the engine computes the rows | Nothing |
| Tag missing, or a sum that differs from the CPU's | 2 | `os_dispatch_incomplete` closes placement at once; the hook returns no rows for that job and the rest of the call, and the engine computes channels `[0, n_gpu)` of every row no job returned | The session and the output region for the boot; the GPU stays powered and idle |
| After the client ring is published: deadline missed, context reset, timeline mismatch, failed maintenance, service failure | 3 | LOST; `os_dispatch_failed`, `board_fail`, watchdog reset; the supervisor records a GPU loss and the next boot keeps OPEN refused | Everything |
| No record | Unknown | The recognizer's 3 s service window expires; watchdog reset; a GPU loss when it was waiting for OPEN or a job's record | Everything |

## Checks

- `checks/pvr-mimus-service.py` runs the worker, this component, the native
  construction and the firmware memory transport under QEMU against a
  software firmware model, over the bench recording's jobs and the failure
  cases, and counts barriers, translations, queue operations and wake-ups.
- `checks/gpu-recognition-engine.py` runs the hook against a model of the
  mimus engine, with block sinks, on the host.
- `checks/os-image-dispatch.py` and `checks/os-image-link.py` cover the
  dispatcher, the mapping checks, the pool replay, the supervisor's GPU-loss
  detail and the linked image.

## Board evidence that would confirm it

See the design's "Board evidence that would confirm the design". The hook's
comparison of every GPU sum with a CPU sum stays until the board shows GPU
cache invalidation at job start, GPU read-only enforcement, cross-core code
writes and retention of the fault slots across a WDT0 reset. The compute
command names no border-colour table (address zero); if the firmware or the
GPU reads it, the read faults on an unmapped address and every job ends
LOST, which a board run of the bench jobs shows at once.
