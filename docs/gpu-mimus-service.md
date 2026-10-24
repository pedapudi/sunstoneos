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

1. The recognizer task (EL0) writes the job (ordinal, code offset, rows, row
   stride) into the slot, stores SUBMITTED and sends the worker a wake
   message (client `PVR_GPU_SERVICE_CLIENTS`, ticket
   `PVR_GPU_SERVICE_MIMUS_WAKE`) without waiting. If the send fails it takes
   the slot back to IDLE by compare-and-swap; the job is then refused.
2. The worker, one priority above the recognizer on the same core, takes the
   slot by compare-and-swap and copies it once. In the same step it looks up
   the tensor admitted at start, checks that the timeline holds the previous
   sequence, calls `pvr_mimus_submit`, writes the compute command and the
   timeline UPDATE to the client ring and submits the kick. A refusal before
   the ring write posts REFUSED and leaves the slot IDLE.
3. On INTID 95, or at the 4 ms bound of each wait while the job runs, the
   worker drains firmware events. When the kick's return is collected and
   the timeline reaches the sequence, it calls `pvr_mimus_complete` (DC CIVAC
   over the job's tags and sums), sets the slot IDLE and posts an 8-byte
   record (sequence, OK).
4. The recognizer receives the record and checks every tag; the engine reads
   the sums out.

The hang bound is 1 s from the kick. A missed deadline, a context-reset
event, a timeline value other than the previous or current sequence, or any
service failure posts LOST for an owed record and sets the lost flag, which
makes later submits return -1.

## Failure classes

| Failure | Result |
| --- | --- |
| Start refused: previous-boot GPU loss, owner or client collision, tensor admission, pools, firmware | OPEN refused; CPU for the boot |
| OPEN fails after power or firmware access | CPU for the boot; the service keeps everything |
| Job refused before the client ring, including a failed wake send | REFUSED; the hook's rows run on the CPU |
| Tag missing, or a sum that differs in the validation build | The hook closes placement for the boot; the session stays open and idle |
| Job lost after the ring write | LOST; the recognizer ends the boot; its supervisor records a GPU loss, and the next boot keeps OPEN refused |

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

See the design's "Board evidence that would confirm the design". The
board items that gate the option without the validation build are GPU cache
invalidation at job start, GPU read-only enforcement, cross-core code writes
and retention of the fault slots across a WDT0 reset.
