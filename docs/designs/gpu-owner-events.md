# GPU owners wait for publications and absolute deadlines

Status: implemented and host-tested for the integrated OS build
(`SUNSTONE_A733_OS`). One native session completes ten lifecycle cycles and a
peer job with correct output and native retirement. The
[owner evidence](../evidence/gpu-owner-events.jsonl) records the physical scope
and cumulative wait counts. A no-work idle rate, sustained repeatability and
WFI residency remain unmeasured.

The GPU service task owns lifecycle and recovery. A READY service with routed
interrupts blocks on its task notification until a request, a device interrupt
or the earliest outstanding job deadline. An idle direct queue arms no timer.
A direct submission publishes its existing hardware kick in the caller, then
notifies the service so that it can arm the new hang deadline. That notification
adds no service dependency before the kick or between completion and collection.
The completion interrupt still acquires the fence and wakes the caller directly.

Firmware-started, compatibility, power-sync and register-readback predicates have
no interrupt completion contract. The pinned vendor implementation polls these
fields: the reference locations are in [GPU startup](../designs/gpu-firmware-start.md),
[GPU shutdown](../gpu-stop.md) and [GPU power](../gpu-power.md).
Native startup, shutdown and power controllers sample each pending hardware
predicate once and return an absolute retry time. The service releases its
transport lock before programming the scheduler timer. Each native MMIO callback
releases its register mutex before returning. The introduced timer waits hold
neither lock. Existing register-mutex acquisition can still block inside a
callback while the transport lock is held (`platform/a733/shared_locks.c`);
this separate limitation retains its existing ownership and failure checks.

Unsatisfied controller predicates use a 50 us observation interval, capped by the
operation deadline. The firmware-start reference uses that interval; the CORE,
PLL and clock-update observations share it. A retry that matures while the worker
plans its wait is refreshed after transport unlock. This keeps timer setup
from turning a short retry into a CPU polling loop. An early notification cannot
bypass the persisted observation time.

The CCU reference (`ccu_common.c:47–61` at board kernel `2ac08e8c7`) requires at
most 100 PLL reads, three consecutive stable samples and a 3 us hold after every
sample, including the final one. The native controller retains those counts and
a 4 us minimum under its whole-microsecond clock. The following 20 us output-gate
hold retains 21 us. Firmware reset release likewise retains 4 us for each 3 us hold.
Minimum holds advance once their stored time matures; notifications never restart
or shorten them. Clock-update waits preserve their 100-read cap. Every path keeps
its original absolute operation deadline and ownership on failure.

The declared intervals of a healthy PLL wait total 104 us: two 50 us observations
and a final 4 us hold. Two such waits and their following holds total 250 us before
MMIO and scheduler overhead. A missing LOCK reserves 100 intervals of 50 us observations, or 5 ms
plus MMIO and dispatch overhead; a missing clock update reserves 99 intervals of 50 us, or 4.95 ms
plus MMIO and dispatch overhead. These are calculated scheduling budgets, not board measurements.
The runtime's OPEN/CLOSE wrappers keep their 1 s window; initial context
creation keeps its 3 s request window. Startup and recognition timing require
physical comparison before landing.

The GPU binds its existing power-domain row wake before the scheduler starts.
A row wait arms only its operation deadline; a published answer notifies the
worker and permits an immediate software-predicate recheck. An answer before
wait registration is observed by the first poll. Other provider, compatibility
and firmware-control phases retain a precise 1 ms observation cadence where no
completion notification is bound. An interrupt-free legacy service retains its
existing polling profile.

## A publication sequence closes the client wake race

Each service client captures a publication sequence before reading its completion
predicate. Its wait publishes the selected notification index under the kernel
lock, rechecks the sequence, and blocks through `sunstone_deadline_wait_until`.
A publication before registration changes the sequence; a publication after
registration leaves a notification pending. Disarming under the same lock clears
late hints before the application can enter another wait.

Service responses, lifecycle progress, queue completion and peer collection
publish events. A caller waiting for READY, STOPPED, PARKED or recovery completion
sends no repeated STATUS requests. It requests one final status record after the
published phase reaches its target. CLOSE that returns BUSY waits for a subsequent
ownership event within its original deadline. A missing event expires that
deadline and retains the outstanding ownership.

Native direct fence waits use runtime notification index 2, shared with their
precise timeout. Context creation registers each caller once; joining registers
the peer. The application retains index 0 and other service requests retain
index 1. The fence and transport-lock predicates are checked after every wake,
including timeout. Waiter removal and late-hint cleanup share the kernel lock
with notification publication.

The OS-created trusted peer has no application address-space tag. Its explicit
task identity selects runtime index 2, matching the GPU completion interrupt.
Its startup-ready publication also gives index 2 after publishing the ready
flag. A ready flag observed before registration needs no wake.

## Failure return preserves retirement proofs

The registered GPU quiesce callback still seals admission, drains accepted
fences, closes firmware, verifies native stop, parks the platform and suspends
the service. The same absolute quiesce deadline bounds all phases. A deadline
wake does not read a firmware timeline or turn a missing interrupt into successful
completion. The service's existing recovery and retained-fault paths decide the
outcome. An unretired queue cannot release the return terminals.

The service records notification wakes, deadline wakes and notifications that
end an indefinite wait with no queued request in `event_wakes`, `deadline_wakes`
and `idle_wakes`. It accumulates time spent in completed indefinite waits in
`idle_wait_us`. A clock reversal or overflow saturates that duration, making the
telemetry invalid without changing device or retirement behavior. The final
wait interrupted by worker suspension contributes no duration.

After worker suspension, the GPU return receipt copies those four counters as
four little-endian 64-bit words after its unchanged eight-word retirement proof.
The proof's size word remains 64. Image geometry admits report objects of exactly
64 or 96 bytes; legacy images retain their 64-byte parsing. Telemetry is decoded
after the proof, and malformed telemetry follows the runner's existing deferred
error path through DRAM and rail restoration.

The lifecycle application holds its initial context open without submissions
for one second. It uses precise application waits and rechecks an absolute
counter target after every notification. The receipt reports cumulative idle
notification wakes per second of completed indefinite waits across the whole
lifecycle, including this declared quiet phase. It measures owner notifications,
not core WFI residency. The host fixture executes an admitted quiet owner and
requires an indefinite notification wait with no unsolicited wake.

`checks/event-gpu.py` executes linked owner and client code with modeled device
steps and task delivery. It checks quiet READY, completed fences, an exact job
deadline, transport-lock contention, a publication before wait registration,
a publication during registration, expiry, direct fence delivery, sub-tick close
deadlines, accumulated idle duration and performance ownership that prevents
CLOSE. `checks/event-gpu-receipt.py` checks both receipt lengths and rejection of
malformed proofs and telemetry. `checks/board-return-preservation.py` executes
the runner's restoration path with both valid and malformed GPU telemetry.
`checks/os-gpu-quiesce.py` checks native shutdown with modeled service replies.
These cases establish software
wait behavior; they do not measure board interrupt latency, power or WFI.

`checks/event-gpu-nospin.py` executes the native controllers with modeled
register values and time. It covers retained sample counts, early notifications,
minimum holds, source-ordered ON/OFF, domain publication and deadline failures.
`checks/event-gpu-peer.py` executes the real task-index selector, startup,
runtime deadline wait and linked GPU IRQ and transport-unlock publishers.
Modeled delivery takes its index from those linked producers. An untagged peer
waits on the same index as its completion producer; the host cases reject
independently mismatched startup and completion signals and a tag-only selector.
Legacy diagnostic builds retain in-call spins and tick pacing because their
platform ports do not provide the OS deadline backend.
