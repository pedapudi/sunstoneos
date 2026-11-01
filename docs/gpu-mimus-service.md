# Mimus on the generic GPU compute queue

**Status: implemented and host-modeled.** Complete recognition through the
current GPU queue remains physically unqualified. [GPU status](status/gpu.md)
identifies the scoped standalone results and remaining lifecycle work.

SunstoneOS provides a generic compute queue through the
[application kit](designs/app-abi.md). Mimus owns its exact-sum shader, tensor
admission, job sizing, output tags and numerical readout in its own repository.
The [application source record](../apps/mimus/source.json) identifies the
checkout. Source paths beginning with `sunstoneos/` below belong to that checkout.

## Components and ownership

| Component | Source | Responsibility |
| --- | --- | --- |
| Public queue | [gpu_queue.h](../include/gpu_queue.h), [gpu_queue.c](../drivers/gpu/gpu_queue.c) | Context handles, leases, submission and completion records |
| Worker component | [pvr_compute_queue.c](../drivers/gpu/pvr_compute_queue.c), [pvr_compute_queue.h](../include/pvr_compute_queue.h) | Client slots, ring, timeline and bounded job progress |
| GPU service | [pvr_gpu_service.c](../drivers/gpu/pvr_gpu_service.c) | Firmware lifecycle, interrupts, scheduling and recovery |
| Native construction | [gpu_queue.c](../platform/a733/gpu_queue.c), [a733_gpu_queue.h](../include/a733_gpu_queue.h) | Pool pages, mappings, programs and compute context |
| OS runtime | [gpu.c](../apps/os/gpu.c) | Queue construction, IRQ95 binding, thermal transitions and final retirement |
| Mimus context and program | `sunstoneos/apps/mimus/gpu_linear.c`, `gpu_linear_program.c` | Model/arena/output leases, tensor admission and compiled shader |
| Mimus job adapter | `sunstoneos/apps/mimus/gpu_linear_job.h` | Job bounds, argument words, bindings, cache extents and tag validation |
| Mimus engine hook | `sunstoneos/apps/os/gpu_hook.cc` | Split a linear call into jobs, compare sums and supply the engine readout |

The queue retains one program and memory map, up to four buffer leases and two
trusted caller slots. One job executes at a time. The native mapping planner
uses workload pages no larger than 256 KiB and bypasses the GPU system cache
for shared mutable data. The [queue design](designs/gpu-compute-queue.md)
defines admission, memory ownership and the public lifecycle.

## One job

1. Mimus opens its queue context after model hashes match. Its adapter admits
   tensor extents against the returned leases and bounds each job by output
   capacity, workgroup count and the arithmetic budget.
2. The engine hook builds a generic job with a sequence, buffer bindings and
   kernel arguments. The worker copies the submitted slot before validating
   and using it. The OS performs the declared cache maintenance and encodes
   COMPUTE 205 followed by timeline UPDATE 213 and KICK 101.
3. The worker observes firmware events on IRQ95 with a 4 ms backup poll. Queue
   completion requires both client-ring completion and the matching timeline;
   deadlines bound progress. Output acquisition precedes publication of the
   completion record.
4. Mimus requires the matching sequence and every output tag. Qualification
   builds compare every sum with CPU int64 arithmetic before engine readout.
   The next job reuses the output only after all readers finish.

The [exact-sum description](gpu-mimus.md) explains the integer arithmetic.
SunstoneOS accepts application-owned programs through the generic interface;
[queue-add](../apps/queue-add/README.md) supplies an independent vector consumer.

## Failure classes

The public queue distinguishes a refused job, thermal parking, timeout and a
latched device fault. A refusal before submission permits the application to
choose CPU execution. Completion requires the submitted sequence; a stale
record cannot authorize output reuse. A timeout does not prove the device
stopped. Fault recovery and buffer release require the queue's documented
quiescence and isolation conditions.

## Lifecycle and power limits

The shared controller selects the vendor's 1008 MHz/960 mV GPU operating point.
Healthy close powers off while retaining memory for reopen. Timeout recovery
requires idle, reset completion, OFF and firmware-memory reclamation before the
queue resumes; failed isolation retains buffers and owed records. Final return
requires both callers to quiesce. Physical thermal response, repeated queue
startup and fault recovery remain unqualified.

[checks/gpu-queue.py](../checks/gpu-queue.py) exercises the queue and service
model; [checks/gpu-peer-native.py](../checks/gpu-peer-native.py) covers native
second-caller construction. The mimus repository owns shader, readout and
recognition checks. Host evidence includes 204 exact-sum jobs and ten modeled
open/close cycles, with queued-peer recovery and final quiescence. These results
establish no physical queue execution. [GPU status](status/gpu.md) records the
evidence paths and current qualification blockers.
