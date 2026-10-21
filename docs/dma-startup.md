# DMA starts after authenticated platform preparation

The [DMA startup adapter](../platform/a733/dma_startup.c) connects native register
operations, the common clock registry, shared FreeRTOS register locks, and the
privileged DMA service. A designated EL1 bootstrap task prepares DMA0 after the
scheduler starts. Client requests remain disabled until that task confirms
platform readiness and activates the service.

The implementation passes a focused two-core FreeRTOS composition with modeled
MMIO. Physical DMA startup remains unqualified. A private handoff record carries
the boot authority's assertions; it cannot establish that remote firmware stopped
or that earlier memory transactions drained. Those facts require independent
board evidence before physical execution.

## One physical counter history covers admission and register locks

The [counter provider](../platform/a733/counter_clock.c) supplies absolute
microseconds from `CNTPCT_EL0`. Initialization checks `CNTFRQ_EL0` against the
established platform rate, which is 24 MHz for native A733 use. It does not
program the counter or reset its time origin.

One platform-lifetime object must supply `a733_counter_clock_now` to the clock
registry, shared register locks, and every startup callback. Adapter initialization
checks that the registry and lock service use the same function and object.
The counter's lower bound therefore includes samples taken inside authority
admission and mutex waits. Separate per-component histories cannot hide an
intermediate regression.

The provider masks interrupts around a short atomic critical section. It acquires
that section before sampling the counter, so samples from different cores cannot
be published in reverse order after preemption. The section contains no callbacks
or scheduler operations. A ticket lock admits waiting cores in arrival order, so
a slower Cortex-A55 core cannot lose every round to faster Cortex-A76 cores. The
wait is bounded by counter time: a caller that waits 100 microseconds for the
section permanently faults the provider. Live holders need far less, because
each holder masks interrupts for only a few instructions. Counter regression,
frequency change, or conversion overflow also permanently faults the provider.
Failure preserves the caller's output. All consumers must retain their
outstanding resources when time becomes uncertain.

At the native 24 MHz rate, conversion to microseconds divides by the constant
24 and cannot overflow; other admitted rates, such as the emulator's, keep the
checked general conversion. The history word that the holder writes is at least
64 bytes from the words that waiters poll, so the two never share a cache line.
The [host contention check](../checks/clock-counter-contention.c) runs the
provider with modeled counter reads on 16 threads, 200,000 calls each; the
thread count and the call count are command-line parameters. No caller faults
and each caller observes nondecreasing time. An injected regression, a changed
counter frequency and a holder stalled beyond 100 modeled microseconds each
latch the fault.

This provider is for privileged platform operations. It requires a stable,
synchronized architectural physical counter across participating CPUs. The
emulator checks do not qualify physical secondary-core counter behavior.

## The copied handoff authorizes an exact controller configuration

The [startup interface](../include/a733_dma_startup.h) copies a private handoff
record before scheduling begins. It records the controller address, complete
16-channel ownership, service channel mask, evidence identity, and exact allowed
platform configuration. That configuration includes the expected clocks, CPU/MCU
routes, permitted route changes, owner, cold/adopt mode, and absolute deadline.

The boot authority must establish these conditions before approving the record:

- All earlier CPU and remote-MCU controller users are quiescent.
- FIFO producers stopped and earlier DMA transactions drained.
- DMA interrupt delivery remains masked throughout startup.
- Independent watchdog recovery is qualified for the planned accesses.

The record must refer to the corresponding board evidence. Setting its flags or
assigning a matching identity does not prove those conditions. Every earlier
controller and route writer must remain stopped while the DMA0 reservation lives.

Channel security and controller access require independent admission. The
handoff's quiet/drain/interrupt flags do not establish nonsecure permissions.
The manual's security overview conflicts with its channel reset-value table,
and neither passage authorizes nonsecure reads of `DMA_SEC_REG`. Startup keeps
that register outside its allowlist. A trusted external record must establish
controller access, selected-channel nonsecure state and descriptor/data firewall
access, including validity after reset release and exclusion of security writers
through recovery. It must not infer these facts from reset defaults or idle
registers. Physical security and access remain unqualified.

The adapter registers one bootstrap task before scheduling starts. Acquisition
requires that task, EL1 execution, the copied configuration, and the derived
shared-clock mask. It then requests one combined registry token for stable
fabric/parent clocks and exclusive DMA0 ownership. An admitted token retains its
identity through later polls and failures. The record and authority callbacks
are privileged state; they are not an application-supplied authorization scheme.

Native register access uses the existing DMA platform allowlist. Each access
occurs while the bootstrap task owns the shared register mutex. Ownership
admission occurs outside that mutex. The underlying
[platform preparation](dma-platform.md) controls reset release, gate ordering,
route changes, deadlines, and retained faults.

All startup objects, clock and lock services, bootstrap task storage, activation
callbacks, and native memory-adapter state must remain reserved and excluded
from DMA allocations. Task deletion, handle reuse, and object reinitialization
are forbidden while the service exists. Current shared mappings do not isolate
malicious privileged writers.

## Deferred service activation closes the scheduler ordering gap

`dma_service_init_deferred` creates the service and copies its activation
configuration before the scheduler starts. Clients are also bound before
scheduling. The private configuration contains the bootstrap task, a bounded
prepared-state callback, and its lifetime context. The existing
`dma_service_init` API still creates an immediately active service whose hardware
prerequisites must already hold.

While deferred activation is pending, the worker waits on a task notification.
It performs no controller polling or other device callbacks. Client sends fail
without creating tickets. The bootstrap task can therefore use FreeRTOS register
mutexes while the service remains inactive.

`dma_service_activate` authenticates the designated task before checking EL1.
An atomic claim admits one activation check. Initialization snapshots activation
metadata before writing service storage, including when the caller supplied
metadata inside that storage. The startup adapter's
`a733_dma_startup_prepared` callback compares the actual service controller and
channel mask with its private handoff, verifies token identity, and requires
completed platform preparation. It also refuses a failed clock provider, retained
shared lock authority, or an unhealthy registry. A read-only registry operation
checks the live token under its existing lock without sampling time or changing
state. Registry contention keeps activation pending. No hardware operation occurs.

| Prepared callback result | Service behavior |
| --- | --- |
| Ready | Publish active state with release ordering, then notify the worker |
| Pending | Keep the service inactive; a later bootstrap attempt may check again |
| Retained failure | Permanently prohibit activation and device access |

The worker and clients acquire the published state before accessing device state.
A notification sent before the worker starts waiting remains pending. Unauthorized
or duplicate activation does not invoke the callback. Failure preserves admitted
platform tokens; the adapter has no release, rollback, or reset path.

A production bootstrap performs this sequence:

1. Before scheduling, initialize one physical counter provider, the common clock
   registry, and the shared lock service with that provider.
2. Supply an independently approved handoff record and initialize the startup
   adapter. Prepare native DMA memory policy and protected reservations.
3. Create the privileged bootstrap task and register it with the adapter.
4. Initialize the deferred DMA service with that task and
   `a733_dma_startup_prepared`, then bind application tasks.
5. Start scheduling. The bootstrap task polls bounded platform preparation and
   activates the service only after readiness.

The production task-creation privilege hook must recognize both the registered
bootstrap task function and the private DMA service worker. All other clients
retain their existing privilege policy. Independent board recovery and validated
Device mappings remain prerequisites for the bootstrap's native accesses.

## Focused checks cover activation and retained startup failure

The [composition fixture](../checks/dma-startup.c) runs the real clock registry,
physical-counter provider, shared FreeRTOS mutex implementation, platform core,
and service on two emulated AArch64 cores. Linker wrappers model peripheral
registers; this check does not execute physical MMIO or DMA transfers.

One image runs in two bounded modes because the singleton service cannot both
activate successfully and enter permanent failure during one lifetime. Both
modes passed on their first executions:

- Successful startup checks copied handoff authentication, a regression observed
  inside registry admission, mutex contention across cores, preactivation silence,
  unauthorized and duplicate activation, publication, and client OPEN/CLOSE.
- Retained failure injects a write that changes modeled hardware before reporting
  failure. The token remains held, later startup polls perform no device access,
  activation remains permanently closed, and the worker makes no device calls.

A separate focused mode checks aliased activation metadata, read-only token
observation, registry contention, and refusal after the registry or shared
register authority becomes uncertain. That mode passed without repeating the
successful startup or partial-write cases.

The [CMake fragment](../cmake/dma-startup.cmake) is included by the central build.
The integrated `dma-startup-model` target compiles and links; component emulator
results are reused after matching source and generated-port identities.
Integration records are in `hardware-integration-20261016d`.
Raw logs, commands, artifact identity, and input
hashes are in `~/.cache/sunstoneos/evidence/dma-startup-20261016/`.
The final observer/refusal check has an archived executable and hash. The earlier
success and partial-write checks retain logs and source snapshots; their original
executable was replaced by the targeted rebuild before its hash was recorded.
The earlier DMA transfer, cache/memory admission, I2S capture, clock, and lock
evidence remains applicable to unchanged lower-layer behavior. Those suites
were not repeated. Physical controller handoff, counter synchronization, bus-stall
recovery, transfer completion, and microphone capture remain unqualified here.
