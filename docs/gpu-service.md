# FreeRTOS GPU lifecycle ownership

The GPU lifecycle service is **implemented** and passes a two-core FreeRTOS
emulator check. One privileged task prepares firmware resources, starts the
firmware protocol, checks compatibility and health, services events, and shuts
down the device. The check uses a software model of firmware responses and GPU
registers. GPU firmware instructions and graphics/compute workloads do not run.
The [native memory adapter](gpu-memory.md) adds emulator evidence for translation
and cache operations. Physical power, cache synchronization, interrupts, isolation, and recovery
remain unqualified.
The optional [interrupt integration](gpu-interrupts.md) masks delivery, queues a
worker wakeup, and rearms only after event service drains its backlog. The
polling fallback remains available when interrupt callbacks are omitted.
The [event dispatcher](gpu-events.md) supplies typed decoding and resource
resolution for the platform event callback. It still requires implemented
resource handlers and deferred firmware response submission.

The [interface](../include/pvr_gpu_service.h) and
[implementation](../drivers/gpu/pvr_gpu_service.c) use static storage and copied
FreeRTOS messages. This document assumes familiarity with driver ownership,
task scheduling, and device memory. The service is excluded from default builds
and is not linked into a physical board image.

## Reserved resources belong to one worker

Before starting the scheduler, the platform supplies an immutable firmware
image, code/data/table buffers, nineteen firmware resources, and backend
callbacks. Initialization copies the configuration and checks allocation sizes,
CPU overlaps, physical page duplication, and reserved physical ranges. It makes
no backend calls and writes no firmware buffers. The platform must establish
CPU/physical correspondence, reserved RAM ownership, firmware identity, and
stopped-device ownership separately.

The platform creates and registers up to eight client tasks. Their handles must
remain live and cannot be deleted or reused. Each caller has one pending request
ticket and one reply slot. Request and reply messages contain values rather
than caller-owned pointers. A collect timeout preserves the ticket and output
argument; the caller must collect that reply before sending another request.

Only the internal worker receives EL1 privilege during task construction. It
runs preemptibly using its task stack at EL1t; ordinary clients remain at EL0.
The emulator uses a separate generated port copy with the existing privileged
task extension. Current shared mappings enforce cooperative API ownership but
do not isolate malicious tasks or memory corruption.

## Lifecycle transitions retain ownership until isolation

| Operation | Required state and result |
| --- | --- |
| STATUS | Any registered task obtains state, epoch, ownership, and component errors. |
| OPEN | Claims an unowned cold or successfully stopped device and starts preparation. Acceptance does not mean firmware readiness. |
| CLOSE | Only the owner of a ready device may begin bounded shutdown. Acceptance does not mean isolation has completed. |

OPEN assigns a fresh queue epoch and prepares initialization records, MIPS
firmware mappings, code/data, boot parameters, and command queues. The worker
then establishes platform power before starting the existing firmware startup
controller. One absolute deadline covers preparation, power enable, startup,
compatibility, and the health command. The platform enable operation must establish stopped
power/reset ownership and the configured GPU clock, including after a reopen.

Successful startup proceeds to compatibility and health checking. The service
reports ready only after that controller completes its protocol. It continues
polling with bounded event batches and retains the platform's event handler.
There is no application job-submission API yet. GPU job admission, completion
fences, and compiler/runtime integration remain required.

CLOSE enters shutdown immediately. The worker performs both firmware power
handshakes, retires normal queue/control access, checks idle/reset conditions,
and calls platform isolation. Only successful completion releases the session
owner. A later OPEN rebuilds the same reserved buffers with a new epoch.
The isolation callback must block new GPU memory transactions and drain those
already outstanding; requesting reset alone does not satisfy its contract.

Any preparation, enable, startup, control, clock, or shutdown failure retains
the owner, buffers, and diagnostics. The service then answers requests without
calling the backend again. It cannot automatically reopen a failed instance.
Independent recovery must prove device quiescence before external reclamation.
Task abandonment also retains resources.

The worker advances one bounded controller step before receiving each request.
While enabling power, starting, checking, ready, or stopping, it waits at most one tick for the
next request. Completion and deadlines therefore advance without client status
polling. Scheduling delays and callback duration can extend the actual interval.
Cold, stopped, and failed instances sleep until a request arrives.

## Power enable can advance without blocking service requests

The platform selects either one bounded synchronous `enable` callback or the
paired asynchronous `power.begin` and `power.poll` callbacks. Initialization
refuses missing pairs or both interfaces together. Both use the common platform
context and monotonic clock.

After firmware-buffer preparation, asynchronous enable enters `ENABLING`.
The begin callback receives the absolute OPEN deadline. Each worker advance
then performs one power poll. A pending result leaves the worker available for
STATUS requests; CLOSE remains busy until the device is ready. Interrupt
delivery remains masked during power sequencing.

The worker checks time before and after each poll. Firmware startup begins only
after successful power completion within the deadline. A power callback failure
or deadline violation retains session ownership and every partial resource
acquisition. The service performs no later power callback. Independent recovery
must reconcile the backend's retained state before it can be discarded.

The `gpu-power-service-model` target connects this interface to the actual
[bounded A733 power controller](gpu-power.md). Its clocks, power-domain registers,
and resource leases are modeled. The successful lifecycle case covers power-on,
firmware startup, shutdown, release, and reopen under a new owner. Separate
cases cover an unacknowledged power-domain request and failure after resource
admission. STATUS remains available while power is pending, and expiry occurs
without further client requests. Both failures retain the lease before any
firmware startup or publication.

Three asynchronous cases passed. One lifecycle case with the
[native memory adapter](gpu-memory.md) also passed through synchronous enable.
The original source comparisons and unchanged resource/interrupt checks were
reused. Evidence is in `~/.cache/sunstoneos/evidence/gpu-power-service-20261016/`.
These checks do not exercise the native PMIC backend, physical power, GPU
firmware instructions, or device isolation.

The shared [resource manager and native adapter](gpu-resources.md) still need
scheduling-aware platform locks and a binding to these service callbacks.
Resource contention must remain pending before the power controller's Boolean
acquire callback is invoked. Cold clock/regulator initialization and physical
qualification remain required.

## Emulator evidence covers integration and retained failure

The [fixture](../checks/pvr-gpu-service.c) connects the actual firmware builders,
queue transport, startup, control, shutdown, and service to a software device
model. It embeds the pinned firmware as an immutable build input but does not
execute its instructions. Its GPU clock value is synthetic.

The focused check verifies:

- EL0 clients, an EL1t worker, preemption during a backend callback, and worker
  migration between the two emulator cores.
- Copied requests, deterministic reply timeout, pending-ticket preservation,
  duplicate-collection refusal, and rejection of an unregistered caller.
- Session ownership, refusal of another client's close/open requests, successful
  shutdown, fresh epochs, and transfer of ownership on reopen.
- Preparation of all reserved firmware resources, health readiness, and one
  synthetic known event serviced without a client request.
- Autonomous startup and shutdown expiry, a control callback failure after
  readiness, retained owners, and no backend calls after each terminal failure.
- Refusal of cross-component CPU and physical allocation aliases, and intact
  guard pages around all twenty-two backing allocations.

One compiled executable supports three distinct failure cases. The first also
covers successful lifecycle transitions and ownership transfer; the other two
reach shutdown and control failure independently. Each has a ten-second host
limit. The [runner](../checks/pvr-gpu-service.py) changes only the initialized
fixture-selection word in derived ELF files and verifies the reported case.

The first link exposed a compiler-generated `memmove` dependency. A freestanding
implementation now supplies that operation; overlapping copies in both directions,
self-copy, and a zero-length copy pass in the integration fixture. An initial
QEMU loader selector was overwritten at startup and repeated the first case.
Those launches do not count as evidence for the other cases. Explicit derived
images subsequently passed both remaining cases.

Existing component reference checks, NPU checks, mimus, and physical board tests
were not repeated. No performance measurement occurred. The model does not
qualify physical coherency, interconnect behavior, interrupt delivery, firmware
publication, or isolation. The synthetic event handler does not implement
resource-specific firmware requests.

Evidence is retained under `~/.cache/sunstoneos/evidence/gpu-service-20261016/`,
including source identities, commands, logs, derived images, and stack reports.
The executable SHA-256 is
`85d1deb7d67d89ac7c1f6ac9a524ae93a1fa20251c63713512523c74cbf229d2`.
That recorded build's code/constants occupy 202,104 bytes, including the 131,072-byte firmware
input. It has four initialized data bytes and 684,496 bytes of BSS/reserved
stacks. The service object occupies 41,832 bytes, including its 32 KiB worker
stack. The worker's compiler-reported frame is 224 bytes; called functions add
their own frames. Physical stack use remains unmeasured.

Reproduce only after relevant inputs change, using the configured toolchain:

```sh
cmake -S . -B ~/.cache/sunstoneos/build/qemu-virt \
  -DSUNSTONE_GPU_FIRMWARE="$HOME/.cache/sunstoneos/sources/gpu/imagination-firmware/powervr/rogue_36.56.104.183_v1.fw"
cmake --build ~/.cache/sunstoneos/build/qemu-virt --target gpu-service-model -j 8
python3 checks/pvr-gpu-service.py \
  --elf ~/.cache/sunstoneos/build/qemu-virt/gpu-service-model.elf \
  --qemu ~/.cache/sunstoneos/host/usr/bin/qemu-system-aarch64 \
  --nm /usr/lib/llvm-21/bin/llvm-nm \
  --output ~/.cache/sunstoneos/evidence/gpu-service-local
```

Use `--cases` to select affected cases when existing evidence covers the others.
For asynchronous power integration, build `gpu-power-service-model` and select
cases `1 5 6` with that executable. Case 5 exercises power-domain timeout; case 6
exercises partial acquisition failure.
The next implementation work is the board backend: reserved allocations and
native memory integration, power/reset/isolation services, resource-specific events, and
physical interrupt delivery. Watchdog recovery remains a prerequisite for new
peripheral access.
