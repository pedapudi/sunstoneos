# Accelerator supplies use the shared PMIC task

GPU and NPU startup can bind the [runtime PMIC provider](pmic-runtime.md).
Each consumer acquires an actual stable rail lease, submits an asynchronous
observation, and retains the completed request for its platform lifetime.
The provider's outstanding borrow prevents release of that parent lease.
The observations establish programmed enable and voltage fields; electrical
voltage, settling behavior, wiring and hardware operation remain unqualified.

The shared provider owns S_TWI0, PL0/PL1 and their clock authority. Consumers
never poll its bus state machine. A separate designated provider task must
continue polling while GPU or NPU startup waits for a result. Every writer of
these supplies must participate in the common rail ledger, including its
existing DCDC2 and GPU DCDC4 ownership bridges.

## Binding disables competing inherited-bus readers

Construct one counter, common locks, clock registry and GPU resource manager.
Construct the rail ledger against that same registry and manager, then construct
the PMIC provider. Obtain its lifetime identity before binding consumers.
An identity copy does not establish provider readiness.

Before scheduling or acquiring any manager resources, call
`a733_npu_startup_bind_pmic()` for NPU startup and
`a733_gpu_platform_bind_pmic()` for the GPU platform. Both bindings must name
the same provider and global manager. The native adapter authenticates the
manager's callback and context tuple, including the NPU shared-resource wrapper.
All configuration, adapters, consumers, managers and provider backing remain
private, immutable and at their original addresses.

Selecting the provider immediately disables the native GPU inherited-bus
reader. Until a GPU view is bound, its rail callback refuses acquisition.
An NPU startup without its corresponding provider binding also refuses progress
when the shared adapter selects the provider. Binding failure can leave this
safe refusal state installed; it does not restore legacy access.

The existing native initializer retains compatibility with isolated inherited
I2C use. That mode is incompatible with an active shared provider. The
provider's privileged admission must exclude every independently initialized
legacy adapter and early reader; binding cannot discover such external users.

## GPU admission copies an authenticated snapshot under the manager lock

The GPU platform acquires a stable DCDC4 lease and obtains an actual PMIC
snapshot before calling the resource manager. It requires page zero, chip
identity 3, revision zero, enabled DCDC4 and the source-defined 800 mV selector.
Health checks authenticate the provider lifetime, exact request, outstanding
borrow and stable parent before arming the native callback.

The resource manager holds its own lock when invoking that callback. The
callback consumes a single authorization and copies only the prepared raw
bytes into that manager's rail record. It requires the registered GPU client to
hold the actual complete lease, including exclusive local authority, while the
manager lock is held. It performs no provider or rail-ledger
lookup, which would reenter the GPU manager through the DCDC4 guard. Any
authorization unused because acquisition returned BUSY is cleared immediately.
The platform checks health outside the lock before power progression and again
before reporting ON. Contention after completed power-up preserves that progress
and delays publication; it does not repeat the power sequence.

The final request and rail lease survive GPU OFF and subsequent starts. These
platform-lifetime objects cannot be released through the consumer API. A new
startup deadline governs each GPU operation; expiration of an earlier operation
does not invalidate the still-held supply lease. Provider or authority failure
permanently refuses later progress while preserving admitted resources.

## NPU startup retains two observations around the settling interval

NPU startup reserves DCDC2 through the actual rail ledger before acquiring its
PCK lease. It then submits the first 800 mV observation. After completion it
waits at least 5,160 microseconds on the common physical counter. Only then does
it reap the first request, keeping the stable parent lease, and submit the
second observation. The second completed request remains held throughout NPU
service lifetime.

Startup retains its absolute deadline through contention and both observations.
After local power-up, the wrapper checks provider health before reporting READY.
The service preparation callback authenticates that same retained observation
and the existing clock/PCK tokens. Completed startup remains usable after its
old acquisition deadline; service health is an independent requirement.

An expired or uncertain operation does not trigger a reset, retry, release or
fallback to the early reader. Provider transfer failures retain the provider's
bus ownership according to its existing recovery contract. Consumer faults
retain their rail leases and outstanding requests.

## Validation scope

The focused native instruction fixture composes the actual PMIC transaction
state machine, rail and clock registries, pin manager, consumer, GPU native
adapter/platform and NPU startup. Its scheduler ownership, mutex dispatch,
hardware responses and architectural counter input are modeled. Existing
FreeRTOS mutex and scheduler evidence covers those unchanged implementations.
The fixture contains no accelerator command execution or physical access.

All six selected cases passed native instruction execution. They cover
two-consumer admission, the minimum NPU observation
interval, shared-unbound refusal, exact snapshot destinations, unused GPU
authorization, retained parent/request ownership, post-startup health, late
provider faults and deadline enforcement during metadata contention. Source,
fixture, executable and result identities are retained outside Git before each
execution. No physical supply or device qualification follows from this check.

The preserved evidence is under
`~/.cache/sunstoneos/evidence/pmic-consumers-20261016/batch-5/`.
`inputs.json` records exact inputs, `build.json` records the native compilation
and link, and `results.json` records each fresh-machine case and executable
digest. Earlier directories retain build-only setup failures; the six cases
passed on their first executable run. Runtime and fixture sources match the
independently reviewed snapshot. No existing provider, scheduler or power
lifecycle suite was repeated.

Native targets using the modified GPU or NPU bindings must link
`drivers/power/a733_pmic_consumer.c` and the existing PMIC provider, rail ledger,
clock registry and pin-manager dependencies. The isolated legacy APIs still
compile against these bindings; dependency linkage is not shared-mode admission.
