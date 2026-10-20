# Native HDMI resource binding

The native HDMI binding connects the controller to the common clock registry,
rail ledger, PMIC provider, PCK resource manager, register mutex and architectural
address-translation helper. It prepares VO1 power and clocks before the controller
reads EDID or enables its TV0 colorbar. The implementation is
`platform/a733/hdmi_native.c`, with construction inputs in
`include/a733_hdmi_native.h`.

A runnable board image still needs qualified cold-boot and analog inputs,
a monitor and cable, and an admitted recovery policy. The repository supplies no fabricated
analog profile. Native construction and instruction emulation cannot establish
those physical prerequisites. The [controller's remaining modes and functions](hdmi.md)
remain required for full HDMI support.

## Image construction owns the physical prerequisites

Before scheduling, the image creates one private native object, its designated
FreeRTOS task, and the existing shared providers. It calls
`a733_hdmi_native_init` with those provider addresses and the task handle.
The PMIC provider must already be initialized; its worker can become ready after
scheduling. Construction performs no device access.

The immutable image configuration identifies an actual cold handoff. The
default mode identifies independently qualified recovery. A distinct supervised
mode admits only the non-DMA TV0 720p60 limited-range pattern under the
[supervised RAM policy](a733-smp-supervised.md). It supplies a supervision epoch
and leaves the independent-recovery epoch zero. The controller rejects both
epochs present or both absent. Supervised mode retains ownership until manual
power removal; output can remain enabled if a fault prevents stopping it.
The separate retained-framebuffer mode additionally requires a live native DE
and memory provider. It authenticates the exact ARMED source before enabling
TV0; the non-DMA reservation cannot authorize that path. See the
[framebuffer image](hdmi-framebuffer.md). No mode permits resource reuse or
claims global drain. The initial handoff establishes exclusion of remote software,
all VO1 consumers and both complete resistor-control words at `0x03000160` and
`0x03000164`. No other native component currently writes those words. An
irreversible single-owner reservation protects their use within this image.
Future writers must join the same authority before coexistence.

The configuration also supplies the complete expected analog image after the
owned reset sequence. The charge-pump, reserved and test fields require known
provenance. A profile ID without that provenance is insufficient. Construction
refuses absent IDs or epochs; physical qualification of their meaning remains
an image-construction obligation.

The native object, configuration, managers, common counter and PMIC private
backing must not overlap. The PMIC provider's actual private-storage predicate
checks that boundary. Copies and configuration mutation are refused. The fixed
owner remains reserved after failed construction or initialization.

## Running tasks acquire real resources

The designated EL1 task calls the embedded controller's prepare, poll, start,
service and stop APIs. It must run outside interrupts and critical sections.
The caller supplies absolute deadlines from the shared architectural counter
and schedules pending calls. The separate PMIC worker advances bus requests.
No unconditional readiness callback is supplied by this binding.

The binding acquires exclusive VO1 bit 63 and shared oscillator/reference/fabric scopes
from the actual common registry. VO1 covers both TV paths, HDMI/eDP, their TOP
and PLL controls, local clocks/resets and PCK domain 10. Common-registry
admission prohibits shared VO1 ownership. The TOP PLL uses raw DCXO; the SFR clock uses normalized SYS24M. The native
binding checks enabled/stable oscillator state and enabled/locked PLL_REF
through the shared decoder. It admits raw 24 or 26 MHz only, records both
parent rates in the runtime reservation, and retains all three shared scopes.
The expected raw rate occupies the request's former four-byte tail padding.
The request remains 72 bytes, and serialized session sizes and existing field
offsets remain unchanged. New images reject a zero expected rate or an observed
rate that differs from the authenticated request. The runtime reservation gains
two rate fields; native providers and their controller consumers must be
compiled against the same header.

Shared DCDC2 and CLDO2 leases come from the rail ledger. Real PMIC requests
read both outputs and retain their provider borrows for platform lifetime.
Admission requires enabled DCDC2 at 800 mV and CLDO2 at 1800 mV with the
supported AXP8191 identity. These observations establish programmed values;
electrical compatibility and settling belong to the qualified cold handoff.

CLDO2 uses enable register `0x21` bit 4 and selector `0x30` bits 4:0.
Codes 0–29 decode to `500 + 100 × code` millivolts. Codes 30 and 31 remain
undecoded. This extends the existing read-only provider without changing its
request or snapshot layout or adding PMIC writes.

The actual AArch64 translation helper checks read and write identity mappings
with Device-nGnRnE attributes for every accessed page. Translation admission
precedes PCK resource acquisition, because that provider may access RTC/R_CCU
while enabling its shared clock. PCK acquisition runs after scheduling through
the existing resource manager and can therefore use the common task mutex.
The resulting persistent lease is retained through stop or failure.

## Cold initialization preserves unrelated controls

Before a reset write, the binding requires PCK10 control/status agreement in
either OFF or ON. Selected TV/HDMI bus gates and SFR must be off, together with
alternative eDP, TV, CEC and HDCP module clocks. The qualified handoff must
also exclude prior local output and pending transactions. An ambiguous domain
or active selected clock causes refusal.

Under the common register mutex, the binding asserts VO1, TOP1, TV0 and HDMI
MAIN/SUB resets. It preserves unrelated bits, including HDCP reset. The complete
group remains asserted for at least 10 microseconds from the final assertion.
This adopts the published `ccu_reset_reset` pulse policy; it does not establish
a measured HDMI electrical minimum.

For an OFF domain, the binding writes the source-defined PCK10 delays:
`0x1f1f1f`, `0x1f1f`, `0x08080808`, `0x0808` and `0x8` at offsets
`0x170`, `0x174`, `0xc00`, `0xc04` and `0xc10`. It requests state 8 at
`0x0706a000`, preserving other control fields, and allows 10 ms for status 8.
An already-ON domain retains its delay fields. No other domain is modified.

Exclusive VO1 ownership includes only its dedicated AHB leaf at CCU `+0x5c0`
bit 4. The shared FABRIC lease excludes global fabric writers, and the common
register mutex serializes masked updates that preserve every other field.
The pinned clock driver and TV0 device-tree binding identify this dedicated
leaf. The manual defines ordinary gate bits in this word. Although the Linux
helper calls `0x010000ff` a key, those bits overlap other live gates; this binding
does not copy that broad OR operation.

The binding then enables the VO1 AHB gate, deasserts the selected resets,
selects SYS24M source 0 for the 24 MHz SFR clock, and enables the selected
bus/SFR gates. The selection changes only while the SFR gate is off.
Every update preserves unrelated fields and verifies readback. The controller
then verifies the complete analog profile and performs its existing bounded
EDID, PHY, TV0 and output sequence.

The common register mutex serializes each native access. Metadata health checks
run outside that mutex. An acquire-load of the registry's published fault flag
runs after mutex admission and before register access, including when a task
waited for the mutex. A late failure retains an owned mutex and all tokens.
Subsequent fault polling performs retention metadata only.

Stop preserves rail, PCK and clock ownership, including REF. Neither clock gating nor TV
completion proves independent output drain. Releasing those resources, changing
rails, restarting after uncertainty, or assigning VO1 to eDP needs additional
completion and recovery evidence.

## Focused native composition passes

The focused instruction check passes with the actual binding, controller, registry,
rail ledger, PMIC state machine, pin manager, PCK manager, common-lock wrapper,
counter and translation code. Queue scheduling, bus/device responses and analog
admission are modeled. One completed cold preparation reaches EDID, active output
and retained stop. Saved CPU/memory positions support the three fault checks.

The selected faults cover a registry fault published during lock acquisition,
a PCK transition deadline and native configuration mutation. Admission checks
also cover private PMIC backing, exclusive-only VO1 and CLDO2 selector endpoints.
The trace verifies reset hold and PCK10 programming. The native adapter compiles
against both the single-core and patched SMP FreeRTOS ports. The prior controller mode
and source-comparison evidence remains reusable; no older suite is replayed.

Primary references are the pinned A733 `pck600_domains.c`, `ccu_reset.c` and
`ccu-sun60iw2.c`, plus the AXP8191 output table and header in the pinned U-Boot
source. Evidence and preserved inputs belong under
`~/.cache/sunstoneos/evidence/hdmi-native-20261017/`. Passing software checks
cannot qualify rail voltages, reset timing, analog behavior or visible output.
