# Native A733 JPEG platform binding

The native VE binding connects the [JPEG executor](video-codecs.md) to ordered
AArch64 register accesses, authenticated clock and power leases, and a common
register mutex. A composed host model reaches JPEG completion only after a
separate drain callback succeeds. Physical execution remains unqualified.

The first binding accepts an authenticated, quiet handoff in which the decoder
is already powered, clocked and out of external reset. It observes the compound
VE identity before changing reset or engine selection. Cold power-on remains a
required boot-platform follow-on: the available sources do not establish that
VE identity registers are readable while external reset is asserted.

The implementation is in `include/a733_video_platform.h` and
`drivers/video/a733_video_platform.c`. It supplies
`a733_video_platform_jpeg_ops()` for `a733_video_jpeg_begin()`, with the platform
object as the callback context. Callers must provide private, immutable
configuration and serialize all access to each object.

## Admission authenticates existing resource ownership

The binding borrows exact tokens from the common clock registry and the existing
GPU resource manager. The latter also manages the shared oscillator and PCK
clock; using it does not acquire GPU-local hardware.

| Resource | Required ownership |
| --- | --- |
| Decoder clock, reset, local gates and PCK domain 2 | Exclusive `A733_CLOCK_VE_DEC`, bit 27 |
| Selected decoder PLL | Shared `A733_CLOCK_PLL_VE0`, bit 25, or `A733_CLOCK_PLL_VE1`, bit 26 |
| Reference clock and oscillator | Shared `A733_CLOCK_REF` and `A733_CLOCK_OSC` |
| AHB, NSI and MBUS configuration | Shared `A733_CLOCK_FABRIC`, plus selected parent scopes |
| Peripheral PLL, when selected by fabric | Shared `A733_CLOCK_PERI0` |
| PCK clock | Exact stable `OSC | PCK` lease in the existing resource manager |
| Both decoder memory masters and interrupt 81 | Authenticated platform admission covering all conflicting users |
| Input, outputs, physical aliases and electrical operating point | Separate platform admission and memory services |

The media clock registry change, commit `532fa6a`, supplies the PLL parent
closure and exclusive local-scope rule. The VE binding requires that dependency before central build
integration. Every platform writer must honor the same registries and mutex.
A mutex serializes writes but does not replace a lifetime reservation.

The binding validates tokens through `a733_clock_token_status()` and
`a733_gpu_resources_status()` before register operations. Forged, stale or
unhealthy reservations refuse access. A busy registry permits a later attempt
where the JPEG callback supports pending work; a register callback that cannot
proceed faults conservatively. Tokens remain held in both DONE and FAULT.

The admission callback authenticates the startup handoff, freezes input bytes and
aliases, excludes both decoder masters and reserves interrupt 81. It must prove
that inherited producers stopped and their transactions drained before reset.
It also supplies independent recovery, valid Device mappings and an electrical
operating point matching the observed decoder rate. Numeric tokens or idle
register values do not establish these facts. No current native authority
provider implements that complete contract.

## Clock and power observations precede VE register access

Physical CCU accesses use base `0x02002000`. Decoder power domain 2 uses PCK base
`0x07062000`; the shared PCK clock gate is at R_CCU `0x070101ac`.
The provider reads clock and power state before accessing the decoder window at
`0x01c0e000`.

The first profile accepts decoder clock mux zero or one, selecting VE1 or VE0.
It derives the rate from the inherited reference clock, PLL multiplier, PLL input
and output dividers, and decoder divider. The resulting rate must match private
startup metadata and remain at or below 720 MHz. The authority provider must
independently approve that frequency and rail combination for the board.
The binding does not retune PLLs or change voltage.

For selected VE PLLs, the binding requires enable, LDO, lock-enable, lock and
output-gate fields; it rejects modulation. At CCU `0x1918`, the selected low bit
must be one for No-Auto mode, and its corresponding upper software-enable bit
must be one. The effective gate-status bit at `0x1998` must also be one.
These controls have different roles and are checked separately.

Supported fabric sources are the 24 MHz oscillator and the documented peripheral
PLL outputs at 400, 480, 600 and 800 MHz, according to each fabric mux.
The binding verifies selected divisors and resulting rates. Peripheral paths
also require the relevant No-Auto and software-enable bits at `0x1908` and the
effective output status at `0x1988`.

The decoder's AHB gate, memory master gate, memory clock, bus gate, module clock
and deasserted external reset must already be enabled. PCK policy and status must
both report ON. Autonomous VE power control at CCU `0x1b10` must be disabled,
and its effective state must report ON. Unsupported or inaccessible entry states
retain reservations without attempting reset or power repair.

Interrupt 81 remains masked in the GIC distributor at `0x03400000`.
The binding checks distributor write propagation, masked enable state, absence
of pending/active state and level-triggered configuration. The admission provider
owns interrupt security, routing, priority and competing handlers. Completion
uses the JPEG executor's bounded polling; this binding does not enable GIC
delivery, clear pending state or send end-of-interrupt commands.

## Preparation follows the admitted runtime policy

The pinned A733/T736 package contains different reset policies for the compound
identities admitted by the JPEG executor. The native binding independently
rereads and validates identity before calling its memory provider or performing
reset. The package policy table and instruction paths establish these choices:

| Compound identity | Package policy index | Reset path |
| --- | --- | --- |
| `0x0003332100021321` | 45 | Kernel external-reset operation |
| `0x0003331000021320` | 51 | Decoder top offset `0x04`, bit 16 pulse |
| `0x0003052000012011` | 52 | Kernel external-reset operation |

For the external-reset path, `IOCTL_RESET_VE` calls the CCU reset controller.
That controller asserts decoder reset at CCU `0xa8c` bit 18, holds it for
10 microseconds, then deasserts it. The A733 kernel then sets and clears bits
0 and 4 at decoder-window offset `0x804`. The binding preserves unrelated fields,
checks readbacks and retains ownership through the timed interval.

The package disables the decoder by selecting mode seven before reset, then
selects MPEG mode zero for legacy JPEG. It establishes top-level output defaults,
disables scaling and rotation, and clears the width-extension fields for the
first profile's maximum width of 1920. Identity-dependent defaults are preserved.
The native implementation combines consecutive read-modify-write operations
when they produce the same final field values; reset pulses retain their order.

For these identities, `VeInitialize` sets internal DRAM interface mode five.
The binding reproduces its top-level field values. This is the package's codec
configuration, not a measurement of the board's physical DRAM type.
The package and kernel source identities are recorded in
[the codec source inventory](video-codecs.md).

Before reset, the memory callback must establish mappings for both decoder
masters, complete device access extents, physical alias exclusion and cache
publication. It may not change VE clocks, reset or registers. Successful mapping
alone does not authorize reset: the admission callback must already have proved
inherited transaction drain. Hardware reset does not supply that proof.

## Native accesses retain ownership on uncertainty

The native read and write functions use explicit physical-address allowlists,
volatile 32-bit accesses, the JPEG byte-write locations, and AArch64 `DSB SY`
barriers. They require identity-mapped Device-nGnRnE pages at EL1.
Non-AArch64 builds refuse these native functions; host models use explicit
substitute callbacks. The CCU write path permits only decoder reset bit 18 and
preserves every other field in the shared word.

Every operation has monotonic absolute-deadline checks. An uncertain register
operation retains the common mutex, resource tokens and memory reservations.
No subsequent callback runs after terminal failure. Successful operations release
only the transient mutex. The component has no resource-release, power-off,
retry or reclamation API.

After JPEG status reports completion, the executor disables codec interrupts and
acknowledges observed status. The binding then invokes a separate drain provider.
That provider must establish stopped producers, completed accepted external
transactions, fault-free IOMMU masters and output-cache invalidation. Only its
successful return permits DONE. A completed reset, codec status bit, register
readback or CPU barrier cannot substitute for external transaction drain.

## Focused checks preserve source and dependency identities

`checks/a733-video-platform.c` composes the binding with the existing JPEG parser,
executor and real clock/PCK token managers. Its host model supplies register
storage and explicit authority, memory and drain callbacks.

The initial sanitizer check passed all three identity paths, pending drain,
unknown identity, invalid No-Auto mode, powered-off entry, active interrupt,
memory failure, uncertain write, late write, drain failure and forged-token cases.
It checked the external-reset hold interval and retained mutex after uncertain
writes. A focused follow-up passed missing effective VE/PERI gate status,
autonomous-power enabled/off states, and valid peripheral-derived fabric clocks.
Those refusals occur before any VE register read or write.

`checks/a733-video-platform-reference.py` executes the original `libVE.so`
`VeReset` instructions with the matching policy-table index. It records both
reference and native preparation traces. Kernel reset operations are explicitly
modeled from `cedar_ve.c` and `ccu_reset.c`; hardware reset effects are not
simulated. Complete final register images and ordered reset pulses match for all
three admitted identities. The gate-status correction preserves the previously
compared preparation writes byte for byte. AArch64 compilation passes with
warnings treated as errors.

The original reviewed source and evidence are frozen under
`~/.cache/sunstoneos/video-platform/review-20261016/`.
The effective-gate correction is frozen under
`~/.cache/sunstoneos/video-platform/review-gates-20261016/`. Both preserve the exact external media-clock dependency used to compile
and execute the checks. Source, fixtures, binaries, reference input hashes and
results remain outside Git with SHA256 manifests.

The earlier JPEG parser check's historical archival gap remains documented in
`video-codecs.md`. This component check uses the preserved baseline fixture and
current native parser source. Its result establishes the composed software path;
it does not reconstruct the earlier malformed-input run or qualify hardware.

## Cold startup and memory authority remain required

The next platform stage must establish a cold identity-readable state without
assuming that powered-off VE registers are accessible. Immutable SoC identity
and published resource mapping may permit initial admission before VE reset
release; that design still needs source-backed checks and native implementation.

The Linux startup sequence powers PCK on, deasserts reset, enables the decoder's
AHB and memory gates, enables bus/module clocks, then pulses the VCU register
fields. PCK initialization uses domain-local delays at offsets `0x170`, `0x174`,
`0xc00`, `0xc04` and `0xc10`, followed by policy eight and status acknowledgement.
The adopted-handoff binding does not perform this cold sequence.

Actual memory translations, full input/output burst extents, cache maintenance,
independent recovery and accepted-transaction drain remain required before a
bounded physical decode. The physical compound identity is still unknown.
Full codec support, including cold startup, additional decode families and
encoders, remains part of the hardware objective.
