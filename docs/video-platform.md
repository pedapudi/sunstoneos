# Native A733 JPEG platform binding

The native VE binding connects the [JPEG executor](video-codecs.md) to ordered
AArch64 register accesses, authenticated clock and power leases, and a common
register mutex. A composed host model reaches JPEG completion only after a
separate drain callback succeeds. Physical execution remains unqualified.

The binding accepts an authenticated, quiet handoff with an accessible decoder,
or an admitted cold state identified through immutable nonsecure ROM. Cold
startup powers and clocks the decoder before reading its compound identity.
Cold mode performs the source-defined VCU reset after ROM admission; both paths
admit compound identity before codec configuration or decode. The sources
do not establish that VE identity registers are readable during external reset.

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

The common media clock registry supplies PLL parent closure and the exclusive
local-scope rule. Every platform writer must honor the same registries and mutex.
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

In inherited mode, the decoder's AHB gate, memory master gate, memory clock, bus
gate and module clock must already be enabled, with external reset deasserted.
PCK policy and status must both report ON. Cold mode establishes these states
after ROM admission. Autonomous VE power control at CCU `0x1b10` must be disabled,
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
substitute callbacks. The CCU write path permits decoder reset and bus bits,
module enable and the three decoder-local gates listed below. Every other field
in each shared word is preserved. Cold mode also permits the fixed domain-local
PCK timing values and policy field.

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

## Memory authority and physical qualification remain required

Actual memory translations, full input/output burst extents, cache maintenance,
independent recovery and accepted-transaction drain remain required before a
bounded physical decode. The physical compound identity is still unknown.
Physical cold-start qualification, additional decode families and encoders
remain part of the hardware objective.

## Cold startup identifies a fixed ROM profile before power-on

`A733_VIDEO_COLD_NS_ROM` implements decoder startup from an admitted OFF state.
The selected PLL, fabric and shared PCK clock must already be stable under the
existing leases. This mode does not initialize those shared resources.
The admission service must establish exclusive decoder ownership, both master
reservations, prior transaction drain, independent recovery and genuine fixed
ROM mappings. The binding then observes ROM bytes itself.

The manual maps nonsecure boot ROM to `0x17000..0x21fff`. The [published A733 ROM](https://github.com/smaeul/sunxi-blobs/blob/42aabf4f2c93158a0cd88a000f9af56ce024882a/sun60iw2p1/ns-brom/blob.hex) constructs
its FEL descriptor at ROM offset `0x2a68`. Instructions at `0x2a88..0x2ac0`
copy the signature and load the identity and scratch-address literals.
The binding compares fifteen selected instruction and data words, including
signature `AWUSBFEX`, identity literal `0x00190300` and scratch address
`0x000ee800`. FEL tools extract SoC ID `0x1903` from that identity word.
These observations identify one published ROM profile. Other revisions refuse.

This check depends on privileged mappings actually reaching immutable ROM.
A copied buffer, caller boolean, build target or cached host descriptor cannot
establish that condition. The currently captured board image covers fixed boot
ROM at `0..0x10fff`, not the nonsecure ROM region. Its hash is
`a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa`;
it differs from the published fixed-ROM image. A physical match for this
nonsecure ROM profile remains unobserved.

The board kernel's `sunxi-sid.c` reads SYSCTRL offset `0x24` only after setting
bit 15 and masks the result with `0xffff0007`. Its device tree provides the
revision field but no expected immutable family value. The captured ROM also
contains a SID read helper at `0x29fc`, addressing `0x03006200 + offset`.
Neither path establishes a sourced read-only A733 family predicate here.
The binding therefore does not read or write SYSCTRL or SID.

After ROM admission, the binding requires decoder PCK policy and status OFF,
external reset asserted, module and local gates disabled, and autonomous module
gating disabled with effective module status OFF. It also checks the selected
parents and masked, inactive interrupt. Each poll performs one bounded step:

| Step | Physical register and operation |
| --- | --- |
| Power delays | Write and verify `0x07062170 = 0x1f1f1f`, `0x07062174 = 0x1f1f`, `0x07062c00 = 0x08080808`, `0x07062c04 = 0x0808`, `0x07062c10 = 8` |
| Request power | Update PCK `0x07062000`, mask `0xf`, value `8` |
| Wait for power | Poll `0x07062008 & 0xf == 8` under the job deadline; reset remains asserted |
| Release external reset | Set CCU `0xa8c` bit 18 |
| Enable local paths | Set CCU `0x5c0` bit 0, `0x5e0` bit 14, `0x5e4` bit 18, then `0xa8c` bit 2 |
| Enable module | Set CCU `0xa88` bit 31, preserving admitted source/divider |
| Validate accessible state | Recheck the inherited-state admission conditions, including effective clock status |
| Cold VCU reset | At decoder `0x01c0e804`, set bits 0 and 4, then clear them, preserving other fields |

The timing values and policy transition follow `pck600_domains.c`'s
`SUN60IW2_PCK_VE_DEC` entry and power-on function. Reset release and gate order
follow `cedar_ve.c`'s decoder enable path. Register access completes with `dsb sy`.
The native writer admits only these local masks and the exact timing values.
There is no rollback or power-down on an uncertain operation; leases remain
held, and an uncertain register access also retains the mutex.

The BSP gate macro assigns its “key” register to the gate register itself.
Its helper ORs `0x010000ff`, `0x41055800` or `0x40302` into the three gate
registers. Those constants overlap unrelated writable gate fields in manual
sections 4.1.6.82–84. The native binding follows the manual's owned-bit
read-modify-write semantics and preserves every other field. Readback verifies
register values; it does not qualify the physical clock path.

The A733 kernel performs the cold VCU pulse without inspecting compound identity.
Its `CONFIG_ARCH_SUN60IW2` path applies before any of the three runtime policies
can be selected. The source specifies two read-modify-write operations with no
explicit delay or completion poll. The binding performs them on separate bounded
polls with access barriers; they do not establish external drain.

After this cold reset, the JPEG executor reads and admits compound VE identity
before codec configuration. Unsupported
identity leaves the powered resources retained. The existing memory service
must still establish translations, physical alias exclusion, complete burst
extents and cache publication before codec preparation. Cold power-on occurs
before JPEG parsing because it is part of the executor's acquisition phase.
It does not trigger decode or grant memory access by itself.

The focused composed check in `checks/a733-video-cold.c` uses the published ROM
bytes, real clock/PCK ownership managers and the existing JPEG fixture. It
covers successful completion with an independently pending drain, ROM mismatch,
missing effective PLL output, power acknowledgement timeout, uncertain write,
and unsupported compound identity. A focused changed-order case uses identity
`0x0003331000021320`: the mandatory cold VCU pulse precedes identity reads,
while its later package reset still uses decoder offset `0x04`.
The check verifies external reset remains asserted during
power transition and unrelated gate values survive startup. Sanitizer execution
and native AArch64 compilation pass. These are source/model results; native
memory/cache/drain authority and physical decoder output remain unqualified.
