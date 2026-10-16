# The GPU lifecycle worker completes free-list work before shutdown

The privileged FreeRTOS [GPU service](../drivers/gpu/pvr_gpu_service.c) integrates
the native [free-list manager](gpu-freelist.md). With free lists enabled, OPEN
constructs backing before firmware starts. Firmware grow and reconstruction
requests become retained worker actions. CLOSE drains those actions before
retiring the firmware queue and isolating the device.

Two-core QEMU checks cover this integration with modeled firmware and memory.
Physical GPU execution remains unqualified. Board use still requires protected
GPU table mappings, page allocation and publication, linked render-target
recovery, firmware execution, and independently qualified device isolation.
This document assumes familiarity with the GPU lifecycle and firmware queues.

## Configuration pins resources for one firmware epoch

The optional `free_lists` member of
[`pvr_gpu_service_config`](../include/pvr_gpu_service.h) supplies immutable list
configurations, a feature-derived minimum page count, an operation timeout, and
concrete free-list callbacks. A zero count disables the integration. Enabled
callbacks use the service's existing platform context and execute only in its
privileged worker.

Service initialization checks configuration and metadata spans against the
service, firmware image, firmware allocations, and physical-page arrays. It
rejects aliases before preparation writes or platform callbacks. The free-list
manager performs its configuration and ownership validation during OPEN.
The platform must keep every referenced configuration and mapping pinned.
Only the manager modifies its page metadata.

Each resource belongs to the service's firmware epoch. Initial construction
retains allocation leases as it progresses. The service does not clear those
leases on failure or successful shutdown. Reopening an enabled service after
STOPPED currently returns BUSY without advancing the epoch. Explicit disposal
following verified isolation remains required for complete reopen support.
Services with free lists disabled retain their existing reopen behavior.

## Initial construction remains bounded and observable

OPEN prepares firmware buffers and the command queue, initializes the free-list
manager and event catalogue, then enters BUILDING. Each worker advance performs
one manager step. STATUS remains available during construction; CLOSE returns
BUSY until the service reaches READY.

The OPEN deadline bounds every construction step and the subsequent power,
startup, compatibility, and health operations. The free-list operation timeout
can impose an earlier construction deadline. Power sequencing and firmware
startup begin only after table and firmware-record publication completes.
A construction failure retains the owner and every admitted allocation.
The worker checks an active operation's deadline after each successful or pending
manager step, retaining the deadline captured before completion. A final callback
that finishes late therefore cannot escape timeout handling by leaving the
manager idle.

The enabled startup, control, and shutdown controllers share a clock adapter.
Every clock observation must be at least as recent as observations held by the
service, command queue, controllers, and free-list manager. A backward clock
fails before the controller proceeds to another device operation.

New service phases and errors are appended to their enums. Replies report the
manager's error and number of retained requests, alongside existing controller
diagnostics.

## Firmware events become deferred manager actions

The service routes grow and reconstruction events through the typed event
dispatcher. Its resource catalogue contains the configured free lists. Admission
copies the complete action into the manager before the event callback returns.
Other firmware event types retain the platform's existing handler.

During CHECKING and READY, the worker polls the control controller first and
then advances the manager once. This order reuses recorded command returns and
keeps allocation, page clearing, publication, and response handling outside
interrupt context. Manager failure stops the lifecycle service and command queue.
Later STATUS requests report retained diagnostics without calling the backend.

## Shutdown drains responses before queue retirement

CLOSE immediately enters DRAINING. Application admission remains closed while
control polling continues to receive firmware events and the manager completes
its retained work. Shutdown begins after the observed event backlog and action
queue drain.

The shutdown controller can receive additional firmware events while waiting for
power acknowledgements. If those events retain free-list work, the lifecycle
worker pauses shutdown progression and services control plus the manager.
Before queue retirement, it performs another control poll and drains any newly
admitted action. A pending response therefore retains queue access until it
completes or the service fails.

The CLOSE deadline applies during DRAINING and throughout STOPPING, including
periods when shutdown polling is paused for a free-list response. Expiry retains
the session owner, pending action, response state, and backing leases. Successful
shutdown releases session ownership only after the existing isolation operation
completes; the free-list backing remains owned by its manager.

Firmware free-list admission continues until control retirement because power-off
may require those responses. This internal admission does not reopen application
access during shutdown.

## Focused checks cover lifecycle ordering and retained failures

The dedicated [fixture](../checks/pvr-gpu-freelist-service.c) runs the actual
FreeRTOS worker, event dispatcher, free-list manager, command queue, and lifecycle
controllers on two emulated CPUs. EL0 clients use copied service requests.
Callbacks verify that they execute in the privileged EL1 worker, including after
worker migration. The fixture models GPU registers, firmware replies, and page
publication; it does not execute GPU firmware or test physical cache coherence.

The focused cases cover:

- Successful construction before power/start, growth, reconstruction, forwarding
  another event type, and CLOSE while a response is outstanding. A reconstruction
  event arrives after the shutdown controller's final internal control poll;
  its response completes before retirement and isolation.
- A failed growth publication that retains every admitted page and the pending
  action, reports the manager error, and prevents later backend callbacks.
- An outstanding response whose CLOSE deadline expires in DRAINING. The worker
  fails autonomously while preserving the pending action and session owner.
- Final record publication completes after the construction operation deadline.
  The manager becomes idle, but the service retains its deadline, detects the
  late completion, and fails before power or firmware startup.

One existing lifecycle check with free lists disabled covers event handling,
shutdown, reopening, and retained startup failure. Prior component checks remain
valid for the unchanged free-list, event, queue, startup, control, and shutdown
implementations.

Build `gpu-freelist-service-model` with the pinned firmware, then run selected
cases using the [runner](../checks/pvr-gpu-freelist-service.py):

```sh
python3 checks/pvr-gpu-freelist-service.py \
  --elf /path/to/gpu-freelist-service-model.elf \
  --qemu /path/to/qemu-system-aarch64 --nm /path/to/llvm-nm \
  --output /path/to/private/evidence --cases 1 2 3 4
```

The runner records each executable hash, selected fixture value, QEMU command,
output, and result. The fixture is an emulator executable; it is not a board-test
submission. Concrete board bindings and physical qualification remain outstanding.
