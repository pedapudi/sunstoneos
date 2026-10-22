# FreeRTOS NPU ownership and privileged execution

The NPU service is **implemented** and passes a two-core FreeRTOS emulator check.
Application tasks at EL0, the unprivileged execution level, send copied requests
to one preemptible driver task at EL1. That task exclusively calls the
[controller core](npu-controller.md) and its [native CPU adapter](npu-native.md).
Physical NPU execution, board resource setup, model loading, and numerical
inference remain unimplemented or untested.

The [interface](../include/npu_service.h) uses static storage and FreeRTOS queues.
The [implementation](../drivers/npu/npu_service.c) owns the controller state,
session owner, command catalogue, request tickets, and reply queues. It performs
no dynamic allocation. The service is excluded from default builds and is not
linked into a physical board image.

## Startup reserves one worker and a fixed command catalogue

The platform initializes the native adapter before starting the scheduler.
It then configures the service with the adapter callbacks, private MMU image,
and trusted command descriptors. Command zero is the bootstrap stream; up to
31 additional entries identify application commands. The service copies these
descriptors, while their memory and tokens remain pinned for its lifetime.

The platform creates application tasks and binds up to eight task handles to
client slots. Each slot has a command-permission mask. Handles must be unique,
remain live, and never be deleted or reused. The worker cannot register as a
client. Initialization and client binding refuse after the scheduler starts.

Commands must already have validated semantics, register permissions, relocations,
and termination. Registration does not validate arbitrary compiled-model commands.
Native model loading and a controlled way to replace the startup command
catalogue remain required. Platform power, clocks, reset ownership, masked NPU
interrupts, reserved RAM, and independent watchdog recovery must precede OPEN.

## Requests preserve task and session ownership

`npu_service_send` identifies its caller through FreeRTOS and copies the request
into the service queue. A successful send means that the request is queued;
the eventual reply reports whether the controller accepted it. No application
pointer is passed to the worker.

Each client has one outstanding request and one private reply slot. Request
tickets increase without reuse; ticket exhaustion refuses further requests.
`npu_service_collect` accepts only the caller's pending ticket. A timeout leaves
that ticket pending and preserves the caller's output argument. The caller must
collect the reply before issuing another request.

Both APIs accept finite wait durations and refuse `portMAX_DELAY`. Queues own
the copied message data, so a caller that times out cannot leave a retained
pointer into its stack. These are task APIs and must not run from an interrupt.

| Operation | Ownership and resulting behavior |
| --- | --- |
| STATUS | Any registered client can inspect state and fault. The reply says whether that client owns the session. |
| OPEN | Claims an unowned session and starts bootstrap. Its timeout begins when the worker executes the request. |
| SUBMIT | Requires session ownership, READY state, and permission for the selected command. The request ticket identifies the resulting job. |
| CANCEL | Requires the owner and an active bootstrap or job. It starts the controller's bounded cancellation sequence. |
| REAP | Requires the owner and a completed or canceled job. It consumes one result and returns the job ticket and cancellation status. |
| CLOSE | Requires the owner and READY or DRAINED state. Only successful controller shutdown releases the session. |
| WAIT | Requires the owner and a zero timeout. The reply has the same fields as STATUS. While a bootstrap, job or cancellation is active, the worker holds the reply and sends it after the first poll or interrupt pass that finds the controller in another state, such as READY, DONE, CANCELED or a FAULT from the controller deadline. |

A held WAIT occupies a separate owner slot. Requests from other clients,
including STATUS, are still answered while it is held. Without WAIT, a client
learns of completion only by sending STATUS after its own delay, so each job
costs at least one extra tick and a STATUS round trip. With WAIT the reply is
sent within one worker poll period in the polled profile, or when the worker
handles the completion interrupt in the interrupt profile.

A bootstrap reaches READY automatically. Successful job completion remains DONE
until the owner reaps it. Cancellation requires acknowledgment and full bus idle;
reaping it enters DRAINED, which requires close before further submission.
Another task cannot cancel, reap, submit into, or close an owned session.

A controller fault retains the owner and all borrowed resources. The service
continues answering status requests but makes no further backend calls.
Reap and close cannot clear the fault. Task abandonment also retains resources.
External recovery must prove device quiescence before releasing faulted memory;
automatic fault reset and task-deletion cleanup remain unimplemented.

## Shared mode gives each client its own session

`npu_service_share`, called after initialization and before scheduling, lets
several clients use the controller at once. The [session runtime](npu-session.md)
uses it to run several models. Each bound client opens its own session: the
first OPEN boots the controller and later OPENs join it, even while the
bootstrap is still running. Sharing refuses a command table in which any
command overlaps the internal-SRAM window, because the controller would refuse
such a command only when the worker later started it.

Each member has at most one job. SUBMIT queues the job and replies at once.
After each poll or interrupt pass and after each request, the worker reaps a
finished job, records the result for its member and starts the next queued
job. It searches the client slots in round-robin order, starting after the
client it served last. While every member keeps a job queued, each member
therefore runs once before any member runs twice. A completed result stays
with its member until that member's REAP, so a member that has not yet reaped
never delays the others. The completion's cache maintenance has already run
for that job's [session group](npu-memory.md).

Replies report the member's own view of the controller:

| Member state | Reply state |
| --- | --- |
| No job; bootstrap running | BOOTING |
| No job; controller ready or running another member's job | READY |
| Job queued or running | RUNNING |
| Job finished, not yet reaped | DONE |
| Queued job canceled, not yet reaped | CANCELED |
| Session left by this CLOSE | CLOSED |
| Any member's controller fault | FAULT |

WAIT is held separately for each member, until that member's own view leaves
BOOTING or RUNNING. CANCEL removes only a job that has not started; REAP then
reports it as canceled. Canceling a started job would drain the controller for
every member, so the request is refused and the job's deadline bounds it.
CLOSE requires a member without a job and leaves the session. The last
member's CLOSE closes the controller exactly as in exclusive mode, including
the interrupt stop. A controller fault, such as an expired deadline, retains
the controller and every member's buffers; every member then receives FAULT.
Exclusive mode, the default, is unchanged.

The [shared-mode check](../checks/npu-service-shared.c) runs three EL0 member
tasks on two QEMU cores against the actual controller, memory policy and
native cache operations, with a register model that completes one job per
credit granted by the check. It covers:

- Refusals for non-members, joining during the bootstrap and immediate replies
  to queued SUBMITs.
- The start order A, B, C, A when A resubmits while C is still queued.
- A completed result that stays unreaped while other members' jobs run.
- Refusal of CANCEL for a started job, and cancellation of a queued job that
  never reaches the device.
- Held WAITs of all three members at once.
- A member leaving and rejoining, the last member's CLOSE and a second
  bootstrap after it.
- A deadline fault reported to every member with no further register access.

Before scheduling, sharing refuses a command overlapping internal SRAM and a
second `npu_service_share` call.
[npu-session.py](../checks/npu-session.py) builds the check with the
npu-service-model flags and link line and runs it:

```sh
checks/npu-session.py BUILD_DIRECTORY NEW_OUTPUT_DIRECTORY --mode service-shared
```

It passed seven consecutive QEMU runs; results are under
`~/.cache/sunstoneos/build/audit-npu-runtime/evidence/service-shared/`. The
exclusive-mode and interrupt checks below still pass with the changed source.

## A preemptible EL1 task performs device operations

The [SMP port extension](../patches/freertos-smp-privileged.patch) changes the
initial execution level only for the service's internal worker during its
startup construction. That worker uses EL1t: privileged execution with its task
stack in `SP_EL0`. Exceptions use the separate per-core EL1 exception stacks.
The existing context-switch path preserves the execution level with the task's
other state.

Application tasks continue at EL0. The service uses normal blocking queues,
timer preemption, and core affinity. A long device operation does not hold an
entire supervisor-call handler with interrupts masked. The emulator check moves
the active worker between cores while its register patterns are live.

The port extension applies to a separate generated source copy for the service
target. Existing SMP kernel, C++ runtime, and mimus targets retain their generated
port files. Source license notices remain intact.

The current SMP mappings expose kernel data and peripherals to all tasks.
Task identity and command permissions enforce cooperative API ownership; they
do not isolate malicious tasks or memory corruption. Memory protection remains
a separate platform requirement.

## Polling advances without application requests

The worker processes at most one request between controller polls. During an
active job or cancellation, it waits at most one tick for a request before
polling again. Request traffic therefore cannot suppress completion or deadline
checks. Scheduling latency and callback execution can extend the actual interval;
the platform must assign suitable priority and CPU time.

Healthy idle sessions wake after a one-second queue wait and sample the native
counter. This maintains the counter extension during long periods without NPU
requests. Faulted sessions stop this sampling and all other backend callbacks.

The controller requires completion or cancellation acknowledgment and full idle
before output synchronization. Software polling cannot recover a synchronous bus
stall. Independently qualified watchdog recovery remains a physical prerequisite.

## Focused integration evidence

The [emulator check](../checks/npu-service.c) connects the real service, controller,
MMU builder, memory policy, and native CPU operations to a software NPU register
model. A fixture-only supervisor call reports the caller's saved execution level.
No NPU command stream runs on physical or emulated accelerator hardware.

The final check passed:

- Two EL0 clients, one EL1t worker, and refusal of an unregistered task.
- A worker yield and cross-core migration preserving `x0`–`x30`, stack position,
  `q0`–`q31`, condition flags, floating-point control, and floating-point status.
- Copied requests, deterministic reply timeout, preserved pending tickets,
  rejection of another request while a reply is pending, and single consumption.
- Session ownership, per-client command permissions, close/reopen, and transfer
  of session ownership between clients.
- Completion after event and bus idle without a client poll, cancellation
  acknowledgment before idle, output guards, and required close after cancellation.
- Idle counter maintenance, autonomous job expiry, retained owner and memory,
  and no register or clock callbacks after the retained fault.
- WAIT: refusal for a non-owner and a nonzero timeout, a READY reply after
  bootstrap, one held DONE reply carrying the job ticket while another client
  still receives STATUS replies, and a FAULT reply when the job deadline
  expires. The interrupt check also uses WAIT for bootstrap and both jobs with
  the worker on core 1, and for the missing-notification deadline case.

The initial integration run passed. Review added idle counter maintenance and
an explicit one-second idle case. That case exposed a fixture startup deadline
that expired before the deliberate idle interval ended. The corrected fixture
allows two seconds for those startup waits; driver deadlines are unchanged.
One final integration run passed within the ten-second host limit.

Unchanged controller, memory-policy, native-adapter, DMA, and mimus checks were
not repeated. No physical device request occurred. QEMU does not establish
physical cache coherence, interconnect behavior, interrupt latency, performance,
or NPU numerical correctness. Broader interrupt-nesting and physical multicore
qualification remain outstanding.

Local results, the failed fixture, final artifacts, and input identities are under
`~/.cache/sunstoneos/evidence/npu-service-20261016/`.
The final ELF has SHA-256
`55323616f8c91cb26b77b8b105df3a37de9ce5a01774f35f2eaf1b22a2bc8575`.
It contains 52,488 bytes of code/constants and 1,834,032 bytes of BSS and reserved
stacks, with no initialized data. That ELF predates shared mode. The service
object now occupies 36,912 bytes, including its 32 KiB task stack and eight
64-byte shared-mode member records. The native adapter and fixture command/table
buffers are separate. Physical stack usage and latency remain unmeasured.

Reproduce after relevant inputs change, using the [configured toolchain](build.md):

```sh
cmake -S . -B ~/.cache/sunstoneos/build/qemu-virt
cmake --build ~/.cache/sunstoneos/build/qemu-virt --target npu-service-model
timeout 10s ~/.cache/sunstoneos/host/usr/bin/qemu-system-aarch64 \
  -machine virt,gic-version=3,secure=off,virtualization=off \
  -cpu cortex-a55 -smp 2 -m 128M -nographic -monitor none \
  -semihosting-config enable=on,target=native \
  -kernel ~/.cache/sunstoneos/build/qemu-virt/npu-service-model.elf
```

The emulator must exit with status zero and print `PASS NPU service:`.

## Optional interrupt service preserves one event owner

The finite single-core interrupt profile binds copied private callbacks before
scheduling through `npu_service_bind_irq`. The existing unbound service remains
polled. The binding and worker remain pinned; successful terminal CLOSE cannot
reopen an interrupt binding. Requests and replies retain their existing layout.

The callbacks run in the worker task and must act on the CPU that receives
interrupt 97. The caller therefore passes the affinity mask of that one CPU,
and binding is refused unless the worker was created pinned to exactly that
CPU. The board binding passes CPU0, where the EL3 installer routes the
interrupt and the only CPU on which its callbacks run. An all-core profile
that pins the worker to core 1 therefore cannot bind the interrupt and keeps
polled completion. Routing the interrupt to another core would also require
the EL3 installer, its restore path and the callbacks to target that core.

The platform ISR masks interrupt 97 and calls
`npu_service_irq_notify_from_isr`. This latches an event notification or a sticky
mask failure and wakes the worker. The ISR performs no NPU register read, output
copy or cache operation. Pre-binding delivery is a retained platform failure.

The worker alone reads the destructive event register. Ordinary deadline and
idle checks do not consume an unnotified event. A successful completion still
requires event zero, full idle, a second error check and output synchronization.
The service counts only successful notified bootstrap and job completions; the
finite client requires three, covering bootstrap and both inference jobs. Missing
notifications expire with all borrowed memory retained. Cancellation progresses
through its existing bounded request/acknowledgment/idle protocol without requiring
an interrupt.

The callback contract is specific to this source and consumer:

| Callback | Invocation and completion condition |
| --- | --- |
| `mask` | Before OPEN, SUBMIT, CANCEL, CLOSE or notified event service when not already masked. Success proves disable propagation and no active handler. |
| `rearm` | Only for an active bootstrap/job with no latched completion. The worker checks pending notifications and invokes rearm inside one kernel critical section. Pending state is never cleared by rearm. |
| `stop` | Only after successful controller CLOSE has disabled device events and proved full idle. Success proves the owned source disabled, inactive and pending clear. |

Each callback returns 1 for completion, 0 for deferred progress or -1 for a
retained fault. Deferred work resumes through the worker's bounded waits, without
a spin loop. A retained callback is never retried. Notifications arriving before
rearm remain latched for service. Client queue insertion also wakes the worker,
and request traffic cannot suppress deadline checks.

CLOSE remains a worker-owned pending request while mask or stop is deferred. The
service publishes its stopped receipt and releases the session owner only after
stop succeeds. A failure keeps the owner and buffers retained. Controller faults
allow status replies while deferred architecture masking continues; they permit
no further NPU backend access. A late interrupt revokes the stopped receipt and
latches failure. The finite application requires that receipt before suspending
workers and again before normal return.

Focused checks use the real FreeRTOS service, controller, native memory policy
and cache adapter with modeled register effects and IRQ notifications. They
cover pending notification during rearm, three completions and output acquisition,
deferred mask/stop, missing IRQ deadlines, mask/rearm/device/stop failures,
diagnostic replies after a deferred-mask timeout and cancellation without an
event IRQ. They also refuse binding with a route of no CPU, two CPUs or a CPU
other than the worker's, and complete bootstrap plus two jobs with the worker
and the modeled route both on core 1. Changed controller cases also pass address/undefined-behavior
sanitizers. Results and exact inputs reside under
`~/.cache/sunstoneos/evidence/npu-interrupt-service-20261019/`. Physical delivery,
cache visibility, numerical execution and recovery remain unqualified.
