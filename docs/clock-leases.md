# Shared clock and power ownership

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
and DMIC. Media scopes cover video PLLs, CSI/ISP, display engines, display
outputs, both system IOMMUs and the video decoder. Thermal scopes cover its
controller and the shared GPADC24M source word. DCDC2 has separate rail
ownership without clock ancestry. Other hardware needs additional scopes
before it can share this authority. UART0–6 and both always-on UARTs have
independent local scopes, with shared APB_UART or APBS1 parent ownership. Each
SPI controller also has its own local scope. The security engine reserves its
complete shared queues and control words; its RC16M source has independent
ownership without reference-clock ancestry. USB0 and USB1 have independent
exclusive controller/PHY scopes. Both reserve the shared USB01 reference and
its reference-clock/oscillator ancestry. Upstream MSI-LITE2 and USB_SYS gates
remain under FABRIC ownership; local port tokens do not authorize their changes.
SDC0–3, the radio module and S_TWI0 also have independent exclusive scopes.
Either eMMC route reserves SDC2 and SDC3 together before hardware access because
both controllers connect to the same card. The token grants no access to the
unselected controller. Storage-controller tokens do not select their module
clock parents; platform admission reserves and verifies those parents separately.
SDC3 uses bit 58. Its focused storage check covers both corrected routes and
ownership conflicts in the recorded build.
S_TWI0 includes its autonomous engines and reserves shared APBS1 ancestry.
PCIe controller and COMB1 PHY scopes are exclusive. Global SERDES configuration
and the shared USB/PCIe power domain allow stable shared ownership and require
exclusive ownership for transitions. Selected clock parents, physical power
and other consumers remain separate prerequisites. These additions allocate
bits 59–62. VO1 uses bit 63 for both TV paths, HDMI/eDP and their shared
TOP, PLL, clock, reset and power-domain controls. TWI11 uses bit 64 for its
controller and CCU+0xeac gate0/reset16. Its exclusive token holds FABRIC shared
to prevent APB1 retuning. Native admission must separately establish the selected
APB1 source and rate, pin ownership and exclusion of remote writers.
S_TWI1 uses bit 65 for the Type-C controller bus and reserves the same APBS1
ancestry as S_TWI0. USB2 uses bit 66 for its independent DWC3/xHCI port,
USB2 PHY, PCK8 and local clock/reset fields. The native provider must separately
reserve its selected parents and shared FABRIC, SERDES and USB/PCIe domain.
The initial high-speed path adopts existing stable power domains; its token
does not authorize COMB0/COMB1 analog or common-reset changes. Source power,
pins and current Type-C attachment remain separate authority.
PWM0 uses bit 67 for its complete controller, including shared channel-pair
controls, gate, reset and interrupt. Its exclusive lease holds FABRIC, REF and
OSC stable. GPADC uses bit 68 for its controller, gate, reset and interrupt 70.
Its exclusive lease holds FABRIC and GPADC24M stable, including the latter's
REF and OSC ancestors. GPADC and the thermal sensor may share that stable
source; either consumer prevents another owner from retuning it.
APBS0 uses bit 69 for the always-on GPIO bus source/divider at R_CCU offset
0x0c. Shared users prevent retuning; an exclusive reservation permits an admitted
change. Its supported source0/source4 profiles retain REF and OSC conservatively.
The PMIC reserves APBS0 alongside APBS1 because its PL0/PL1 pins use that bus.
Bits 70–127 remain invalid.
The registry fault indicator uses a lock-free 32-bit atomic.
Permanent-token owners can observe failure without reentering the registry lock.
The native DMA composition covers late fault observation; PCIe controller
validation and integration remain separate.
The [rail ledger](rail-leases.md) separately coordinates 39 PMIC outputs through
actual DCDC2 and GPU DCDC4 guards; clock ownership does not grant rail authority.

Shared dependencies expand from either audio output to audio PLL1, then to
the reference clock and oscillator. Peripheral PLL0 and PLL_NPU also depend on the
reference clock and oscillator. Exclusive child ownership keeps its ancestors
shared and stable. Local controller scopes are exclusive; the caller separately
leases the selected parent.

All clock writers must participate in this registry. A common register mutex
also protects register updates. The registry does not replace pin, regulator,
DMA-channel or memory ownership. Its local audio scopes include module clocks,
bus gates, reset and controller/FIFO routing.

The registry has 128 fixed slots and a nonblocking atomic lock. A busy
call returns immediately. The owner must poll against its own deadline; a
conflicting acquisition does not call the clock callback. Successful metadata
changes require a monotonic microsecond observation before their deadline.
Callbacks must be bounded and non-reentrant.

Scope masks are 128-bit unsigned integers. Tokens occupy 48 bytes with
16-byte alignment; the fixed array of 128 tokens occupies 6144 bytes. The
native registry occupies 6208 bytes with 16-byte alignment. Its 32-bit atomic
lock serializes mask operations; no 128-bit atomic or dynamic allocation is
required. The 128 reservation slots count active tokens, independently of the
number of scope bits.

Images adopting the 128-bit mask ABI must rebuild their producers and consumers
together because it changes token size, alignment, containing structures and
mask-valued function arguments. Adding PWM0 and GPADC scopes preserves that ABI.
Tokens remain private runtime metadata and have no wire or persistent format.
Historical evidence remains valid for its recorded sources and ABI; it does not
establish execution of a rebuilt image.

The focused `checks/clock-wide.c` check covers independent TWI11 and VO1
ownership, FABRIC conflicts, upper-half token authentication, stable publication
and retained faults. It also checks refusal of every unassigned high bit.
The check passed once with AddressSanitizer and UndefinedBehaviorSanitizer.

The focused `checks/pwm-adc-clock.c` case covers the added controller scopes,
their parent dependencies, thermal/ADC coexistence and parent-change conflicts.
It passes with the same sanitizers under
`~/.cache/sunstoneos/evidence/pwm-native-20261018/clock/`. Initial integration matched
the tested registry, header and original case. A subsequent boundary assertion
checks refusal of the first unassigned scope; its result is preserved separately
under `clock-boundary/` without replacing the original evidence. Fixture boundaries
now derive from `A733_CLOCK_ALL`; reconciliation records their equivalent tested
value and the unchanged registry/header hashes. The older suites were not rerun.
`checks/clock-wide.py` compiled 61 production consumers reached through the
common clock header with the cached eight-core configuration. The radio session
also used its existing networking allocation options and pinned TCP headers.
Older
MMC, PCIe and width fixtures receive compatible width or invalid-bit assertions
and are compiled without replaying their scenarios. Results and input identities
are recorded under `~/.cache/sunstoneos/evidence/clock-wide-20261017/`.

The focused `checks/clock-usb-scopes.c` case covers the two additional USB-path
scopes. It checks coexistence with PMIC, camera and USB0 ownership, conflicts
with shared parent changes, exclusive-only local admission and token identity.
One AddressSanitizer/UndefinedBehaviorSanitizer execution and the changed core's
AArch64 compilation passed. Inputs, executable and logs are preserved under
`~/.cache/sunstoneos/evidence/clock-usb-scopes-20261017/`. These assignments
preserve the mask and token layout; earlier width and peripheral workloads
were not repeated. The Type-C and USB host providers retain their own pending
integration and physical qualification requirements.

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

The fabric scope protects AHB, NSI, MBUS and APB1 interconnect selectors, dividers
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
PCK-clock and DCDC2 rail leases are acquired by the [startup adapter](npu-startup.md).
DCDC2 has a separate stable-supply scope in the common registry. Every PMIC
writer must acquire exclusive rail authority; the PMIC mutex alone only
serializes register transactions. DCDC2 ownership has no clock ancestors and
does not alter audio-profile publication. Physical supply and clock qualification
remain prerequisites for production startup.

PLL_NPU can also supply memory, fabric and video clocks. Retuning it requires
exclusion of every consumer; an idle NPU does not establish that exclusion.
The NPU's shared SRAM also requires its power domain to remain on. Memory
reservations must preserve the domain lifetime for every CPU and DMA user.

The focused NPU platform fixture checks actual registry conflicts and refuses
shared ownership of the local NPU scope. Native compilation passes; evidence
is in `~/.cache/sunstoneos/evidence/npu-platform-20261016/`.
Existing registry clients retain their earlier evidence.

## Clock publication transfers ownership without a release window

After a trusted coordinator validates the physical profile, atomic publication
converts its exclusive PLL_NPU, display PLL1, USB01 reference or GPADC24M scope
to shared ownership. A fresh
token replaces the old identity while preserving the owner and all other scopes.
Validation and time checks precede the metadata change. Refusal leaves the old
reservation and output unchanged; publication itself proves no hardware state.

The [NPU startup handoff](npu-clock-handoff.md) writes the replacement directly
into persistent startup storage and transfers the source authority only after
its actual controller completes. A late fault retains the reachable token.
Focused emulator checks cover successful adoption and failures before and after
publication. The USB extension passes a focused host check for reference
publication and concurrent independent port reservations. Its physical source
proof and consumer adoption belong to the USB platform controller. The [display handoff](display-clock-handoff.md) adopts the completed PLL and
media producers into actual scanout ownership. It refreshes every borrowed live
token before the next callback and retains the reachable token after late faults.
Its focused host composition and native compilation pass.

The thermal controller reserves GPADC24M and its conservative reference and
oscillator ancestry. Changing that shared source requires exclusion of every
ADC and thermal consumer. After source setup and verification, the coordinator
may atomically publish GPADC24M as stable while retaining all other scopes.
Publication does not itself initialize the clock or authorize a new consumer.
Local THS ownership is exclusive. Native thermal
admission also verifies APB1 and the selected source under the register mutex.

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

The APBS0 reservation and GPADC24M publication changes pass one new focused
registry case under `pwm-service-20261018/prerequisite/`. A second case executes
the actual PMIC provider against modeled registers and checks its APBS0 ownership,
source/divider validation, reference dependency and refusal after a profile change.
Both frozen artifacts were reconciled during integration without another run.
Existing native images retain their earlier provider layout; these host results
do not establish new native or physical operation.
