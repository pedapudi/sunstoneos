# Shared clock ownership for GPU, audio, DMA and NPU

The common clock registry prevents participating drivers from changing clocks
that another driver holds stable. It records shared and exclusive leases in
private platform storage. A lease authorizes its owner to use or change a
resource; it does not establish the hardware's clock rate or power state.

Focused host checks pass dependency conflicts, concurrent acquisition, atomic
audio-profile publication, and composition with the actual I2S and GPU resource
managers. The registry and both bindings compile for freestanding AArch64.
Production startup wiring and physical coexistence remain unqualified.

## One registry protects shared parents

Initialize one zeroed registry before concurrent use. Each caller supplies a
trusted owner identity and receives a token containing its identity and scopes.
Token identities never repeat; exhaustion refuses further acquisition.
Release requires the complete current token and verified hardware quiescence.

The registry covers the oscillator, reference clock, peripheral PLL0, audio
PLL1, its DIV2 and DIV5 outputs, local audio controllers, DMA0, PLL_NPU, the
NPU controller and interconnect configuration. Local audio scopes cover I2S0 through I2S4, ASRC, OWA transmit
and DMIC. Other hardware needs additional scopes before it can share this authority.

Shared dependencies expand from either audio output to audio PLL1, then to
the reference clock and oscillator. Peripheral PLL0 and PLL_NPU also depend on the
reference clock and oscillator. Exclusive child ownership keeps its ancestors
shared and stable. Local controller scopes are exclusive; the caller separately
leases the selected parent.

All clock writers must participate in this registry. A common register mutex
also protects register updates. The registry does not replace pin, regulator,
DMA-channel or memory ownership. Its local audio scopes include module clocks,
bus gates, reset and controller/FIFO routing.

The registry has thirty-two fixed slots and a nonblocking atomic lock. A busy
call returns immediately. The owner must poll against its own deadline; a
conflicting acquisition does not call the clock callback. Successful metadata
changes require a monotonic microsecond observation before their deadline.
Callbacks must be bounded and non-reentrant.

An expired release keeps its reservation. Clock failure or regression prevents
further registry mutations and preserves existing tokens. An owner can also
retain its token after an uncertain hardware operation, disabling later
registry mutations. A busy retention call must be retried before the owner
relinquishes its existing exclusive authority.

## Audio publication preserves ownership through PLL startup

The [audio binding](../include/a733_audio_leases.h) supplies resource callbacks
to the existing [I2S platform controller](i2s-platform.md). It forwards register,
time and mutex callbacks with their original context. The binding, parent and
local controller have one serialized owner; the registry supports independent
callers concurrently.

Cold audio startup requires exclusive ownership of audio PLL1, both outputs
and all covered local audio consumers. The existing parent controller verifies
that those consumers are inactive before programming the PLL. It validates
the selected profile, lock, output gates, modulation and automatic gating.

After validation, publication atomically replaces the change token with a
fresh shared DIV5 token and records profile availability. No other consumer can
acquire the clock between those operations. Publication itself performs no
hardware validation. Active-profile adoption still requires the existing
parent controller's register checks.

Local I2S0 close restores its clock/reset state and pins before releasing local
ownership. Reference and audio-parent tokens remain held for platform lifetime.
Partial failures retain tokens. There is no automatic rollback or parent
teardown.

## GPU startup binds the existing resource manager

The [GPU parent binding](../include/a733_gpu_clock_parent.h) connects the common
registry to the existing GPU resource manager without changing that manager's
interface. Initialize both managers and reserve a private GPU client identity
before admitting any GPU, audio or other clock clients.

Poll the binding to readiness. It first acquires shared peripheral PLL0,
reference and oscillator ownership in the common registry. It then acquires
the corresponding parent lease in the GPU manager, which verifies the inherited
24 MHz reference and 400 MHz parent. Each poll performs at most one acquisition.
The binding does not program clocks, gates, regulators or GPU power.

Both tokens remain held until reboot. They prevent parent changes through
either manager while ordinary GPU local leases can open, close and reopen.
Future retuning requires coordinated, verified release. Deleting either token
would violate the other manager's ownership contract.

Startup must stop admission if the binding fails. A timeout or invalid inherited
profile retains any acquired tokens. Terminal polls perform no callbacks.
The registry and GPU manager must use the same monotonic microsecond clock.

## DMA preparation requires controller and interconnect authority

The DMA0 scope is exclusive and requires a platform handoff covering the entire
controller, its channels, dedicated bus/MBUS gates, reset and CPU/MCU routing.
Idle channel registers cannot establish that remote firmware has relinquished
ownership. The [startup binding](dma-startup.md) retains this scope throughout use.

The fabric scope protects AHB, NSI and MBUS interconnect selectors, dividers
and global source gates. Shared fabric ownership does not stabilize the
selected PLLs: callers must also lease those parents. Dedicated peripheral
leaf gates belong to their controller scopes; unrelated gate fields must stay
unchanged without separate authority.

The registry can acquire fabric and selected-parent stability together with
exclusive DMA0 ownership. Hardware selector/rate verification, remote-MCU
handoff, clock/reset setup and production composition remain platform work.

## NPU startup retains shared PLL and local power ownership

The [NPU platform controller](npu-platform.md) acquires stable PLL_NPU,
reference, oscillator and fabric scopes together with exclusive NPU ownership.
The NPU scope includes its local clock, resets and power domain. Separate
PCK-clock and DCDC2 rail leases remain prerequisites for production startup.

PLL_NPU can also supply memory, fabric and video clocks. Retuning it requires
exclusion of every consumer; an idle NPU does not establish that exclusion.
The NPU's shared SRAM also requires its power domain to remain on. Memory
reservations must preserve the domain lifetime for every CPU and DMA user.

The focused NPU platform fixture checks actual registry conflicts and refuses
shared ownership of the local NPU scope. Native compilation passes; evidence
is in `~/.cache/sunstoneos/evidence/npu-platform-20261016/`.
Existing registry clients retain their earlier evidence.

## Focused evidence covers ownership composition

`checks/a733-clock-leases.c` combines registry cases with the existing I2S
platform fixture. AddressSanitizer and UndefinedBehaviorSanitizer pass parent
conflicts, publication, stale-token refusal, capacity and identity exhaustion,
deadlines, and two concurrent exclusive claimants. Actual I2S cold startup,
active adoption, close/reopen and retained failure use the common binding.

`checks/a733-gpu-clock-parent.c` uses the actual GPU resource manager. Its
sanitizer check passes paired parent acquisition, change refusal through both
managers, local close/reopen, contention, wrong-profile retention and deadlines.
The existing lower-layer suites were not repeated.

The focused `checks/dma-clock-leases.c` extension passes shared-DMA refusal,
fabric change conflicts and peripheral/reference-parent protection. The changed
registry compiles for AArch64. Evidence is in
`~/.cache/sunstoneos/evidence/dma-platform-20261016/clock-evidence.json`;
unchanged GPU/audio cases retain their earlier results.

Commands, observed results, source snapshots and AArch64 object identities are
under `~/.cache/sunstoneos/evidence/clock-leases-20261016/`. The first GPU fixture
compile required a diagnostic adjustment for an included, unused legacy test
entry point. The corrected fixture passed; runtime behavior did not change.

These checks establish software ownership behavior. Board startup still needs
to create the shared registry, complete GPU parent admission and supply the
audio binding to its parent controller. Physical clocks, reset recovery and
concurrent workloads require separate qualification.
