# Prepare NPU clocks and power under retained ownership

The [platform controller](../drivers/npu/a733_npu_platform.c) implements bounded
local startup for the A733 NPU. Its initial operating point is 492 MHz at 800 mV,
as listed in the pinned board kernel. Host checks and AArch64 compilation pass.
Physical startup, inference, and recovery remain unqualified.

The controller requires a suitable inherited PLL configuration and a settled
supply rail. It programs the NPU module clock, releases its external resets,
and powers PCK domain 4. PCK is the SoC power-domain controller. READY permits
platform integration to consider opening the [NPU controller](npu-controller.md);
it does not authorize a command or activate the [NPU service](npu-service.md).

## Startup preserves shared clocks and voltage

The pinned kernel defines these resources:

| Resource | Source-defined control | Initial behavior |
| --- | --- | --- |
| NPU module | CCU `0x02002000 + 0xb00`; divider bits 0–4, selector bits 24–26, gate bit 31 | Select PLL_NPU and divide to 492 MHz |
| NPU bus and resets | CCU `+0xb04`; bus gate bit 0, core reset bit 16, AXI reset bit 17, AHB reset bit 18 | Require gated, reset-asserted state; release AHB, AXI, core, then enable the bus |
| NPU SRAM reset | CCU `+0xb04` bit 19 | Require deasserted; preserve it |
| NPU critical leaf gates | CCU `+0x5c0` bit 6 and `+0x5e0` bit 18 | Require enabled; preserve entire words |
| PLL_NPU | CCU `+0x2a0`; parent PLL_REF, N bits 8–15, M bits 20–22 | Verify enabled, locked, unmodulated and stable; preserve configuration |
| PLL_NPU gate control/status | CCU `+0x1920` bits 0 and 16; `+0x19a0` bit 16 | Require No-Auto, software enable and effective gate enabled |
| NPU clock-management mode | CCU `+0x1b1c` bit 0 | Require disabled; enabled-mode handoff remains unsupported |
| PCK clock | R_CCU `0x07010000 + 0x1ac` bit 0 | Require enabled and leased |
| NPU power domain | PCK `0x07060000 + 0x4000` | Require OFF; program source timing values and request ON |
| Supply | AXP8191 DCDC2 | Require enabled, settled 800 mV and a stable lease |
| Interrupt | SPI 65, GIC interrupt 97, level high | Require masked delivery and exclusive event ownership |

The module selector supports PLL_NPU, PERI0 outputs at 800/600/480 MHz, VE0,
VE1, and DE. Initial startup admits PLL_NPU only. The PLL calculation uses
`24 MHz × (N + 1) / (M + 1)`, with the source's allowed multiplier range.
This requires input divider M1 to equal one. The primary manual defines its
control at bit 1, although the kernel clock description omits that field.
Startup refuses a set bit 1 in PLL_NPU or a selected PERI0 parent.
Accepted inherited parent rates are 492, 984, and 1,968 MHz. Module divisors
are respectively 1, 2, and 4. A 1,008 MHz inherited parent is refused.
The reference PLL must produce exactly 24 MHz from the detected oscillator;
19.2, 24 and 26 MHz inputs use their actual multiplier/divider ratio. Modulation and
selected-parent automatic gating must be disabled.
The primary manual defines gate-control bit 0 as `0: Auto`, `1: No-Auto`.
PLL_NPU therefore requires `+0x1920` bits 0 and 16 set, together with effective
gate status at `+0x19a0` bit 16. For each selected PERI0 output, the controller
requires its No-Auto route bits and corresponding software-enable bits at
`+0x1908`, plus matching effective status at `+0x1988`. Parent and divided-leaf
gates both participate in this check. Unused routes do not constrain admission.
The verifier reads these controls and statuses; it does not change any gate.

The source operating-point table assigns 800 mV to 492 MHz for every listed
NPU speed bin, for both oscillator variants. This implementation supports the
normalized 24 MHz reference path. The startup authority must establish that the identified
board has a supported NPU operating-point profile. The initial profile is a
functional bring-up choice; it is not a performance target.

DCDC2 is always-on in the board device tree and also supplies eDP and HDMI
resources. NPU startup neither changes its voltage nor disables it. A resource
callback must observe the rail configuration, establish settling, and reserve
it against all PMIC writers. A nonzero callback token alone is not that proof.
The native register adapter has no PMIC write access.

PLL_NPU also appears in the source selectors for MBUS (`+0x588`, selection 5),
video encoder (`+0xa80`, selection 6), video decoder (`+0xa88`, selection 6),
and DRAM0 (`+0xc00`, selection 4). Current startup authority does not establish
exclusion of all those consumers. Cold PLL provisioning remains required when
the inherited rate is unsuitable. It needs authenticated consumer handoffs,
selector observations, and stable reference ownership before retuning.
An idle NPU cannot establish memory or interconnect quiescence.

## One owner retains every admitted resource

The [public contract](../include/a733_npu_platform.h) copies trusted startup
metadata and callbacks before modifying state. One privileged owner serializes
polls. The acquisition callback authenticates the complete configuration against
private startup records, including the controller handoff and recovery proof.
The common registry grants stable PLL_NPU, reference, oscillator, and fabric
leases plus exclusive NPU-local ownership in one token.

The added `A733_CLOCK_PLL_NPU` scope expands through REF and OSC.
`A733_CLOCK_NPU` permits exclusive ownership only; it covers local module,
reset, dedicated leaf gates, and PCK domain 4. It does not authorize shared PLL
or voltage changes. Existing audio-change masks retain their meaning.
Separate PCK-clock and DCDC2 leases remain held for platform lifetime.
All relevant writers must participate in the same ownership system.

Fabric configuration is supplied as exact AHB, NSI, and MBUS selector/divider
words and rates. The controller admits normalized SYS24 and PERI0 profiles
used by [DMA startup](dma-platform.md). It verifies the selected parents and
leases PERI0 when necessary. Unsupported DDR, DE, or NPU fabric selections are
refused. FABRIC ownership alone does not protect a separately programmable PLL.

Acquisition can return READY, BUSY, PENDING, or RETAINED. BUSY has no effects.
PENDING preserves admitted tokens while missing resources are acquired.
READY requires the complete resource bundle. RETAINED stops startup and keeps
all identities. If a callback replaces an existing token, the controller keeps
both the previous and returned bundles for recovery diagnostics and stops.
No automatic release, rollback, reset, or retry follows a fault.

Register operations use one common bounded mutex. Resource callbacks run outside
that mutex. Each read, write, lock completion, and final unlock receives a
deadline check. The clock provider must enforce one monotonic microsecond bound
across admission and platform calls. A late final read or unlock cannot publish
READY. These units are separate from a controller's request timeout interface.

Cold startup requires the module and bus gated, with core, AXI, and AHB resets
asserted and SRAM reset deasserted. Both domain command and status must be OFF.
Critical leaf gates and the shared PCK
clock must already be enabled. Startup releases AHB, AXI, and core resets,
sets the module rate, enables the bus and module clocks, and requests domain ON.
This preserves the pinned driver's clock-before-runtime-power sequence.
The controller waits for domain status 8, then at least two milliseconds,
and checks the final local state before returning READY.

The manual maps shared SRAM at CPU addresses `0x00074000`–`0x000f3fff` and
requires the NPU power domain to remain on while that SRAM is used.
The startup handoff must therefore exclude inherited CPU and DMA users of this
memory. Later allocations must retain the NPU domain for their lifetime.
External reset or power-off cannot reclaim those allocations without proving
every consumer has stopped. Device virtual SRAM addressing remains a separate
controller/memory contract.

The [native adapter](../drivers/npu/a733_npu_platform_native.c) restricts writes
to local module/reset fields and domain 4 policy/timing words. Read-modify-write
operations preserve unrelated fields while holding the common mutex.
Device mappings, authenticated resource binding, and shared monotonic timing
remain production composition prerequisites. Secure startup must also authorize
EL1 access to the CCU and domain registers before these callbacks execute.
A physical bus stall can prevent a
CPU callback from returning; software deadlines require independent watchdog
recovery to cover that failure.

## Command admission and execution remain separate

The reviewed startup path requests no separate device firmware image. It uses
register setup followed by the controller's native bootstrap commands.
The host model compiler and its prepared command artifacts remain external
dependencies for numerical fixtures. This source observation does not establish
support for every possible firmware or model mode.

Startup cannot broaden the command admission boundary. The compiled convolution
model with SHA-256
`db42d815100417f4b8913896462ed031beafae35fb4ee1c28569ac950a9e8fca`
retains its [fixture-specific preparation and limitations](nbg-conv-loader.md).
Memory mappings, descriptor semantics, privileged command registration, cache
publication, and numerical acceptance remain separate requirements. READY does
not prove device execution, correct inference, bus drain, or safe shutdown.

## Focused evidence and source identities

One host session with address and undefined-behavior sanitizers checks source
startup order, the admitted inherited profiles, preserved register fields,
registry conflicts, partial acquisition, retained write failure, missing power
acknowledgment, and late completion. It also checks that no callback runs after
a fault. The source extractor derives clock/reset fields, domain timings,
operating-point values, and startup ordering from the cached pinned files.
The changed controller, native adapter, and registry compile for AArch64.
After inspection of the primary manual, a focused follow-up checks refusal of
unsupported input dividers, asserted SRAM reset, and enabled clock-management
mode. It also checks that retained acquisition ends callbacks immediately.
Only the changed controller and native adapter required another native compile.
Existing DMA, NPU controller/service, and prior registry evidence remain reusable.

Artifacts, executable hashes, and pre-build input snapshots are retained under
`~/.cache/sunstoneos/evidence/npu-platform-20261016/`.
The initial successful image remains in `attempt-1`; the focused follow-up and
final inputs are in `admission`. Their source snapshots and the recorded delta
support reuse of the successful startup checks. `reference.json` in each
directory records source identities. The source repository is
[the board kernel](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f),
pinned to `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.
The added operating-point source is
[`sun60iw2p1-cpu-vf.dtsi`](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/arch/arm64/boot/dts/allwinner/sun60iw2p1-cpu-vf.dtsi),
with SHA-256 `d5535630db3d0acdeace832d1dfad684fab9ab0c31c482b493e36a7f3387e5ad`.
Its cache manifest is `~/.cache/sunstoneos/sources/npu-platform/manifest.json`.
The primary reference is *A733 User Manual V1.00*, sections 2, 4.1.6.6,
4.1.6.56, 4.1.6.120, 4.1.6.271, and 5.3. Its PDF SHA-256 is
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`;
the shared source lock names it `a733-user-manual-1.00`.
No board access or performance measurement occurred.

The parent-gate correction has separate evidence under
`~/.cache/sunstoneos/evidence/npu-parent-gates-20261016/`. A focused source
comparison checks manual sections 4.1.6.249, 4.1.6.255, 4.1.6.258 and 4.1.6.264.
The host model accepts each selected PERI0 output route, then refuses every
missing required No-Auto, software-enable or effective-status bit before writes.
The changed controller and native read allowlist compile for AArch64.
The startup fixture now supplies forced-on controls and matching effective
status. Its lifecycle logic is unchanged; earlier startup, service and
convolution evidence is reused without replaying those cases. These software
checks do not establish physical gate behavior.

The [shared reference decoder](clock-leases.md#detected-oscillator-and-normalized-reference)
requires DCXO enabled/stable and exact REF24. The pinned kernel gives PLL_NPU
and PERI0 fixed PLL_REF parents; neither rate is computed from raw DCXO.
Manual §4.1.6.6 marks PERI0 bits24:23 reserved, so they do not select a parent.
The remaining divisor, modulation, branch-gate and effective-status checks stay
in force. These observations occur at baseline under retained parent leases;
they do not establish continuous hardware monitoring or physical qualification.

The focused detected-reference checks are preserved under
`~/.cache/sunstoneos/evidence/accelerator-clock-reference-20261018/`.
Actual provider code accepts exact 26 MHz N96/M104 and 19.2 MHz N100/M80
profiles and retains wrong ratios, lost reference lock, and disabled or unstable
DCXO before writes. GPU/NPU PERI0 cases preserve reserved bit24; the NPU PLL
case also retains final control corruption without opening the output.
These modeled-register checks do not establish physical operation. No native
image was rebuilt; accelerator launch still requires a final image built from
the accepted startup and providers, plus actual resource and recovery admission.
