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

## One resource core serves the decoder and encoder

The decoder and encoder bindings share one resource core in
`include/a733_video_unit.h` and `drivers/video/a733_video_unit.c`. The core
validates the clock and PCK leases, retries a busy registry, takes and releases
the common register mutex and performs every register access against the job
deadline. It also checks the fabric clocks, the DCXO reference, the selected VE
PLL, the PERI0 outputs, the inherited powered baseline and the compound
identity. A clock-reference or busy-handling rule therefore has one
implementation for both units.

A constant descriptor holds the facts that differ between the two units:

| Field | Decoder | Encoder |
| --- | --- | --- |
| Register window | `0x01c0e000` | `0x01c10000` |
| PCK power domain | 2, at `0x07062000` | 3, at `0x07063000` |
| Module clock register | CCU `0xa88` | CCU `0xa80` |
| Reset and bus gate at CCU `0xa8c` | Bits 18 and 2 | Bits 16 and 0 |
| Local gates at CCU `0x5c0`, `0x5e0`, `0x5e4` | Bits 0, 14 and 18 | Bits 1, 12 and 1 |
| Autonomous power control at CCU `0x1b10` | Checked | Not checked |
| Interrupt | 81 | 80 |
| Module mux value that selects PLL_VE0 | 1 | 0 |
| Exclusive clock scope | `A733_CLOCK_VE_DEC` | `A733_CLOCK_VE_ENC` |
| Identity block | Window offset `0xe0`, three admitted versions | Window offset `0x8e0`, one admitted version |

The codec bindings keep their own job types, register allowlists and
sequencing. A codec-neutral decoder layer, `struct a733_video_decoder`, holds
the decoder's admission, cold or inherited start, identity, memory and reset
steps. The JPEG, H.264 and HEVC bindings pass their own typed jobs through its
`admit` and `memory` hooks, so no binding presents its job as another codec's
structure. The HEVC binding selects decoder mode 4 and admits the H.265 engine
registers described in [the HEVC document](hevc-native.md). The encoder layer,
`struct a733_video_encoder`, passes typed jobs in the same way for the JPEG and
H.264 encoder bindings. The cold NS-ROM start, the JPEG drain and the
retained-copy gate remain decoder-specific.

A host fixture drove the JPEG and H.264 decoder bindings and both encoder
bindings through nominal, cold-start, identity, contention, uncertain-write,
refused-mutex, parent-selection and job-mismatch cases, and recorded digests
that match those of the separate decoder and encoder implementations that
preceded the shared core. The fixture has no retained check (see
[the evidence file](evidence/video-platform.jsonl)).

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
unhealthy reservations refuse access. A busy result means another task is inside
the registry's short critical section; the observation has no effect. As
[the clock registry contract](clock-leases.md) requires, the binding polls again
against its own deadline. It retries for at most 2 ms, two 1 ms scheduler ticks,
so an equal-priority holder on the same core can finish after a time slice.
If the registry is still busy, a callback that supports pending work returns
pending, and a register callback faults and retains its resources. The shared
register mutex already waits up to the job deadline, so a refused mutex entry
also faults. The encoder binding applies the same rule. Tokens remain held in
both DONE and FAULT.

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
and output dividers, and decoder divider. The reference clock must come from an
enabled, stable DCXO, and its multiplier and divider must convert that
oscillator's 24, 19.2 or 26 MHz rate to 24 MHz. The decoder and encoder bindings
apply this rule through the shared `a733_clock_reference.h` helper; the 26 MHz
board's reference uses multiplier 96 and divider 104. The resulting rate must match private
startup metadata and remain at or below 720 MHz. The authority provider must
independently approve that frequency and rail combination for the board.
The binding changes no voltage. It programs a PLL only in a cold start at the
vendor's 624 MHz, described below; otherwise it retunes no PLL.

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
0 and 4 at decoder-window offset `0x804`. The binding preserves unrelated fields
and retains ownership through the timed interval. It checks the readbacks of the
reset word; the `0x804` pulse it writes as the kernel does, read, write, read,
write, with no readback check, because no source documents how those bits read
back.

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
barriers. They refuse an address outside the pages that
`a733_video_unit_native_map` checked once: every page the video bindings
access, identity-mapped Device-nGnRnE. `a733_video_decoder_init` and
`a733_encode_platform_init` call it, so the page walks fall outside every hold;
an image that uses the native functions without either init makes the check at
its first native access. The encoder shares these functions.
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

The model's oscillator reports an enabled, stable 24 MHz DCXO. Further cases
admit the 26 MHz board's oscillator and reference words, `0x983fb0f7` and
`0xf8675f00`, in inherited mode and in `checks/a733-video-cold.c`; the
preparation trace is unchanged. A disabled or unstable DCXO, and a 26 MHz DCXO
with equal multiplier and divider, are refused before any VE access. A clock
registry held busy during JPEG programming and a PCK registry held busy while the
drain is pending leave the write trace unchanged. A clock registry that stays
busy past the retry interval retains the fault without holding the register
mutex.

A host comparison executed the original `libVE.so` `VeReset` instructions with the
matching policy-table index and compared reference and native preparation traces;
complete final register images and ordered reset pulses match for all three
admitted identities, with kernel reset operations modeled from `cedar_ve.c` and
`ccu_reset.c`. It needs the vendor libraries, which are not in the repository,
so no retained check repeats it. Its result establishes the composed software
path; it does not qualify hardware or reconstruct the JPEG
malformed-input run described in [video codecs](video-codecs.md). Source,
fixtures, binaries, reference input hashes and results remain outside Git with
SHA256 manifests; see [the evidence file](evidence/video-platform.jsonl).

## Register-lock holds bound band-2 waits

The counts come from the composed model (`checks/video-cold.py`, its `holds`
lines) at the vendor point from the FEL words. A native access adds a `DSB SY`
before and after. The core reads the clock when a hold starts and when it ends,
not on each access. A write adds the native
writer's read of the old word, and the write policy's reads: three for each
PLL_VE0 write (the three consumer module words) and up to two for a `0xa88`
write with bit 31 clear.

| Phase | Model accesses (writes) | Native accesses | Lock |
| --- | --- | --- | --- |
| ROM identity, before the entry hold | 15 (0) | 15 | none |
| Entry | 22 (0) | 22 | held |
| Row request and answer | 0 | 0 in the decoder; 6 PPU writes and their reads in the owner if the domain is OFF | none |
| Hold A | 22 (7) | 40: 22, plus 7 old-word reads, 9 PLL policy reads and 2 `0xa88` policy reads | held |
| PLL lock wait | 4 to 100 PLL reads | same | none |
| Hold B | 34 (4) | 41: 34, plus 4 old-word reads and 3 PLL policy reads | held |
| `prepare`, identity | 3 (0) | 3 | held |
| `prepare`, each of 18 steps | at most 3 (1) | at most 4 | held, one step per call |
| Quiesce | 18 (7) | 38: 18, plus 7 old-word reads, 12 PLL policy reads and 1 `0xa88` policy read | held |

The longest register-lock hold is hold B, 41 native accesses. A task that
needs the register lock waits behind the decoder for at most the longest hold
plus the time the decoder task is preempted while it holds the lock, by
interrupts and by tasks above its priority on its core. The mutex's priority
inheritance raises the decoder task to the waiter's priority, so tasks between
the two priorities do not lengthen the wait.

The OS image's decoder task repeats `acquire` and `prepare` calls while they
advance, rather than one call per tick: `prepare`'s eighteen steps run back to
back, each in its own hold, so no hold exceeds four accesses there. From the
ON answer, a cold start therefore makes about 120 native accesses, about
240 µs at the measured 2 µs each, plus at most about 420 µs of waiting.

The binding's cold record (`struct a733_video_cold_record`) stamps each phase
in microseconds: the entry hold (`entry_at` to `entered_at`), hold A (`on_at`
to `hold_a_at`, which includes taking the lock), the lock wait (`hold_a_at` to
`locked_at`, which ends after the settle), hold B (`locked_at` to
`hold_b_at`) and the close's hold (`close_at` to `closed_at`).
apps/vdec-check/host.py reports them as `durations_us`.

Run `1794714415147688285-6b3ec3f5` on port 3-1, with CPU-L at 494 MHz,
measured the entry hold at 75 µs for 37 native accesses, the 15 ROM reads
included: about 2 µs per access. Hold A took 59 µs. That image stamped
`locked_at` before the 21 µs settle, so its hold-B span of 99 µs holds the
settle too, and hold B itself took at most about 78 µs. The largest measured
hold is therefore about 80 µs at that clock, and a task that needs the register
lock waits up to that plus the decoder task's preemption while it holds the
lock.

## Memory authority and physical qualification remain required

Actual memory translations, full input/output burst extents, cache maintenance,
independent recovery and accepted-transaction drain remain required before a
bounded physical decode. The physical compound identity is still unknown.
Physical cold-start qualification, additional decode families and encoders
remain part of the hardware objective.

## Cold startup identifies a fixed ROM profile before power-on

`A733_VIDEO_COLD_NS_ROM` implements decoder startup from an admitted OFF state.
Fabric and the shared PCK clock must already be stable under the existing
leases. The selected PLL must be stable too, except at the vendor's operating
point: with module word `0x81000000` and 624 MHz, the cold start holds PLL_VE0
exclusively and runs its enable table before opening the module gate
([video PLL design](designs/video-pll.md)).
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

The cold start needs the consumer's domain-2 row
([power-domains.md](power-domains.md)); `a733_video_platform_init` refuses a
cold configuration without one. The binding writes no PCK600 register: the
power-domain owner programs the vendor delay row and the ON command, or adopts
a domain already ON. The sequence has six phases.

| Phase | Register-lock hold | Accesses |
| --- | --- | --- |
| ROM identity, before the entry hold | no | The 15 ROM words, once. The boot ROM is immutable, so the reads take no register lock. |
| Entry, before the row request | yes | Fabric and parent clocks; reset asserted (`0xa8c` bit 18 clear), bus gate and module gate closed; `0x1b10 & 0x30001 == 0x20000`; INTID 81 quiet. `0x5c0`, `0x5e0` and `0x5e4` are read and recorded in any state. No PCK600 read. |
| Row request and wait | no | `a733_power_domain_request(row, 1)` once; the binding returns pending until the owner answers. REFUSED or FAILED fails the unit with nothing written. |
| Hold A | yes | Open `0x5e4` bit 18 (below); A1 reads the three critical gates and refuses unless `0x5c0` bit 0, `0x5e0` bit 14 and `0x5e4` bit 18 are set; release reset (`0xa8c` bit 18); open the bus gate (`0xa8c` bit 2); the module selector and divider and the PLL_VE0 table up to lock enable ([video-pll.md](designs/video-pll.md)). |
| PLL lock wait | no | At most about 420 µs. |
| Hold B | yes | PLL output gate and module gate (`0xa88` bit 31); the powered baseline, which rereads PCK2 policy and status; the cold VCU pulse at `0x01c0e804`, bits 0 and 4 set and then cleared with no readback check (the two words read are recorded). |

From the row's ON answer to a started decoder the three last phases run in one
acquire call. A busy register lock is effect-free: the call returns pending and
the next call resumes at the same phase. The baseline's PCK2 reads stay
although the owner has answered ON, because the encoder, on domain 3, shares
the baseline and has no row.

**The MBUS clock gate.** CCU `0x5c0` bit 0, `0x5e0` bit 14 and `0x5e4` bit 18
are `CLK_IS_CRITICAL` (`ccu-sun60iw2.c` lines 622, 663 and 688-692). The port
3-3 FEL dump reads `0x5c0 = 0x110103ff` and `0x5e0 = 0xf1055803`, both bits
set, and `0x5e4 = 0x4`, bit 18 clear. The binding opens `0x5e4` once, as the
kernel's critical-clock registration does (`ccu_gate.c`
`ccu_gate_helper_enable`, lines 213-240): a read-modify-write that ORs in the
key value `0x40302`, then a readback. The kernel says key bits read as zero;
the manual (4.1.6.84) documents bits 1, 8 and 9 as the VE encoder, CSI and ISP
MBUS clock gates, and the FEL words of `0x5c0` and `0x5e0` show their key bits
set. The readback therefore requires bit 18 set and every bit outside the key
unchanged, and the binding records the written and read words. A word that
already holds the whole key is not written. This is the binding's only gate
write; it never writes `0x5c0` or `0x5e0`.

**Write policy.** `a733_video_platform_write_policy` admits `0xa8c` bits 18 and
2; `0xa88` bit 31 alone while the gate is open, and the selector and divider
while it is closed; PLL_VE0 with owned bits `0xf870ff02` while none of its
consumers runs on it; `0x5e4` only for a word that carries the whole key, with
owned bits `0x40302`; and the decoder registers. It refuses every other CCU
word, the AHB and MBUS master gates, and every PCK600 word. The read allowlist
keeps PCK2 policy and status for the baseline and no delay-row word.

**Failures.** A refusal on a value that was read completely releases the
register lock before the unit fails; an uncertain access retains the lock and
every token. Every failure inside a hold goes through one helper,
`a733_video_unit_refuse_held`, which the encoder binding also uses. The unit
records why it failed: `uncertain` for an access or lock operation that did not
return exactly 1, `late` for a deadline found passed. It also keeps the address
and word of its latest access.

### Close

`a733_video_decoder_close(p, d, faulted)` is the decoder's one close, for a
started decoder (`faulted` 0, the quiesce) and for one that failed
(`faulted` 1). It runs under one lock hold, in the vendor's order
(`cedar_ve.c` `disable_cedar_hw_clk_module` and `disable_cedar_hw_clk_reset`,
`ccu_gate.c` `ccu_pll_gate_helper_disable`): module gate off, bus gate off,
reset asserted, then PLL_VE0's output gate, lock enable, enable and LDO
cleared, each write read back. That leaves `0x01204d00` with the board's bit
24. Domain 2 stays ON and the critical gates stay as they are, so the end state
is an admitted entry state for the next power lifetime.

- The close writes only CCU words, so the state a fault left (PLL enabled with
  its output open or closed, the module gate open or closed) needs no decoder
  access.
- After a fault, a unit that failed on a value or a deadline is closed. A late
  failure inside a hold closes in that hold; otherwise the close takes the lock
  first. A unit whose failure was uncertain is never touched.
- A close step whose readback differs fails the unit with the lock released.
- Within one power lifetime the closed decoder stays closed: every later open
  or acquire returns -1 with no register access.

The OS service calls the close at most once
([video-decode-os.md](video-decode-os.md#faults-after-the-first-write)).

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

`checks/video-cold.py` runs the composed model `checks/a733-video-cold.c`. It
joins the binding, the JPEG executor, the clock and PCK ownership managers and
the actual PMIC provider, rail ledger and power-domain owner
(`checks/power-domains-model.h`) with the published ROM bytes and the JPEG
fixture. PCK domain 2 exists only in the owner model. The cases cover:

- a non-vendor point from domain 0/0: completion with a pending drain, a ROM
  byte changed, PERI0 status clear, a PPU that never acknowledges, an
  uncertain write, an unlisted identity and the 26 MHz board;
- the vendor point from the FEL words and fifteen other PLL entry and lock
  states, and the CCU words a hold-B fault left (module gate and bus gate open,
  reset released), refused at entry with no write;
- reset released at entry (refused with no request), either master gate clear after ON (refused at A1
  after the `0x5e4` open), `0x5e4` key bits that read zero, and `0x5e4` already
  open;
- two lifetimes: the first from 0/0 ends in the quiesce, and the second starts
  from its words, once with the CCU as the quiesce left it and once at the FEL
  words. Domain 2 stays 8/8 and is adopted. The owner's own domain-2 cases,
  adoption and policy and status disagreeing among them, are
  checks/power-domains-owner.py's;
- the VCU pulse when its bits read zero and when they stay set; both start, and
  an exact readback (the stage-1 board image's code) fails the first;
- faults after the first write and the fault close: INTID 81 pending once the
  module gate opens (refused in hold B's baseline) and a module gate that does
  not open (refused at its readback) both close to the quiesce's state; a bus
  gate that does not close refuses the close with the lock released; an
  uncertain first write is not touched.

The model asserts that the decoder writes nothing before the ON answer, never
a PCK600 word or a master gate, that A1 precedes the reset release, that the
phases from ON to started take one acquire call, and that every read passes the
native read allowlist (`a733_video_platform_native_readable`). Thirty-five
driver mutants must each fail it. It prints the accesses of each lock hold; the
budget in [video-pll.md](designs/video-pll.md) uses them. A focused
changed-order case uses identity `0x0003331000021320`: the cold VCU pulse
precedes identity reads, while its later package reset still uses decoder
offset `0x04`. These are model results; memory, cache and drain authority and
physical decoder output remain unqualified.
