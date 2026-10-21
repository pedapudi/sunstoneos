# Native GPADC passively observes board-selection dividers

The supervised application samples the four connected GPADC board-monitor
inputs and copies two sets of raw observations through USB. GPADC0 observes boot
selection, GPADC1 DDR-parameter selection, GPADC2 board identification and GPADC3
USB upgrade/COMB1 selection. The pinned [board wiring audit](board-wiring.md#pwm-outputs-and-analog-inputs-have-specific-board-uses)
establishes these routes. Sampling grants no authority to drive, change or
interpret a strap. Physical ADC operation remains unqualified.

The initial mode excludes GPADC4, which reaches an optional display connector,
and channels 5–7, which have no established usable connection. Dedicated analog
pads need no PIO mux or output transition. The separate LRADC0 and PMIC ADC are
not substitutes for this controller.

## Sources define the initial controller profile

The A733 device tree identifies GPADC at `0x02521000` and GIC interrupt ID 70
(SPI 38). The manual's generic interrupt example names a different IRQ; this
application follows the A733-specific device tree. CCU offset `0x0fc4` owns the
local gate and reset-deassertion bit. The `0x0fc0` 24 MHz parent is shared with
thermal monitoring. Existing parent validation checks the actual oscillator,
reference PLL, APB1 and applicable PERI0 profile without retuning them.

The register sequence follows manual section 18.12.8 and the pinned U-Boot
`sunxi_gpadc.c` continuous-mode/data-register implementation. The native driver
uses a 1 kHz sample divider and a 100 microsecond acquisition interval at the
admitted 24 MHz clock. The manual requires acquisition time greater than
`10 × R × C`, with 6.4 pF sampling capacitance. At the admitted maximum source
impedance of 1 MΩ, this bound is 64 microseconds; 100 microseconds exceeds it.
The actual divider network and its impedance still require physical admission.
The divider setting is not a measured conversion rate.

Only channels 0–3 are selected. FIFO, DMA requests, compare interrupts and test
injection are refused. The internal-reference selector must already be set;
the driver never writes it, calibration data, test selection, pads or straps.
Code zero is valid. No default board-ID interpretation or calibrated voltage
conversion is supplied.

## The analog supply is distinct from the reference

The board audit connects AVCC to CLDO3. A733 Datasheet V0.93 section 5.3 lists
VCC-ADC at nominal 1.8 V, separately from its VREFP-ADC reference row. Its supply
minimum and maximum remain unspecified. Section 5.6 limits analog input to
0–VCC-ADC. These nominal specifications do not qualify actual voltage or tolerance.

This profile admits only the nominal 1.8 V CLDO3 setting, explicitly bound as
selector 13 and 1800000 microvolts in the full session. An actual shared rail
reservation and read-only PMIC snapshot verify page, chip, revision, enable and
selector before controller startup. CLDO3 uses enable register `0x21` bit 5 and
selector `0x31`, with the pinned AXP8191 control table's 100 mV steps from 500 mV.
No PMIC voltage is changed. Programmed settings do not establish measured AVCC,
stability, divider population or safe input range; reviewed evidence must do so.

## The schematic bounds passive input loading but leaves population choices

The pinned [V1.2 schematic](https://drive.google.com/uc?export=download&id=1layhtSvMpQo-z_iMJjNfNVvhPhObpsvW),
sheet 10 (`10_A733 Analog&High-speed`), shows the following circuits. Resistor
values are design annotations, not an observation of either attached board.
The green labels R1–R12 are selection-table aliases; use the actual component
references below when identifying fitted parts.

| Input and SoC ball | Circuit drawn | Nominal voltage and source resistance |
| --- | --- | --- |
| GPADC0, 1D24 | `BOOT-SEL-ADC`: R24, 10 kΩ ±1%, to AVCC; no lower resistor drawn | AVCC and 10 kΩ for the circuit shown. The boot table also lists alternative divider populations. |
| GPADC1, 1D25 | `DDR-PARA-SEL-ADC`: R25, 10 kΩ ±1%, to AVCC; R28, `X-1%`, to ground | Depends on R28: 1, 2.7, 5.1, 8.2, 14, 27 or 68 kΩ, or absent. The table lists approximately 0.163–1.569 V, or 1.8 V when absent, at AVCC = 1.8 V. |
| GPADC2, 1D26 | `BOARD-ID-SEL-ADC`: R1126, 10 kΩ ±1%, to AVCC; no lower resistor drawn | AVCC and 10 kΩ for the circuit shown. The board-ID table lists the same optional lower values as the DDR divider. |
| GPADC3, 1F24 | `USB-UPGRADE-COMBO1-SEL-ADC`: R33, 10 kΩ ±1%, to AVCC; R34, 27 kΩ ±1%, to ground | About 0.730 × AVCC and 7.30 kΩ. At 1.8 V this is about 1.314 V; the schematic table labels this option 1.315 V. |

R27 and R1125 are separate GPIO selection pull-downs; neither is part of
the analog dividers.

For an upper resistor U and lower resistor L, the ideal unloaded voltage is
AVCC × L/(U+L), and the Thevenin source resistance is U in parallel with L.
An absent lower resistor gives AVCC with source resistance U. Every populated
pull-up option in the boot table is at most 10 kΩ; its omitted-upper option
has a 10 kΩ lower resistor. All shown divider options
therefore have source resistance at most 10.1 kΩ including 1% resistor tolerance,
assuming the depicted connections and a sufficiently low-impedance AVCC source.
The GPADC3 ratio spans approximately 0.7258–0.7337 with independent 1% resistor
tolerances; these are calculated design bounds, not measured input limits.

All depicted passive inputs lie between ground and AVCC. Sheet 10 connects
AVCC directly to both VCC-ADC at ball 1H21 and VREFP-ADC at 1F21. VREFN-ADC
at 1F22 joins analog ground; R32, 0 Ω, joins analog and board ground.
C198 and C200 provide the drawn 2.2 µF supply/reference decoupling; C199 is
470 nF on VCM-ADC. Sheet 6 connects CLDO3 to AVCC and also to VDD18-DRAM,
VCC-PLL, VCC-CPU-PLL and VCC-DCXO. These shared loads reinforce the requirement
to observe the inherited supply without retuning it.

The manual's section 18.12.5.2 requires acquisition time greater than 10RC,
with sampling capacitance 6.4 pF. At the derived 10.1 kΩ bound, 10RC is
0.647 µs after rounding upward, below the configured 100 µs acquisition time.
This calculation resolves the design-level impedance question. It does not
establish actual stuffing, AVCC source impedance, board damage or external
loading. The schematic's highlighted boot/configuration rows and `8GB` label
are not a fitted bill of materials for either board.

Manual section 18.12.8.32 distinguishes internal and external LDO reference
selection. The driver's required inherited internal selection remains separate
from CLDO3 authority. The schematic establishes a common AVCC/VREFP net; it
does not establish the internal LDO's actual voltage, readiness or accuracy.
Datasheet V0.93 sections 5.3 and 5.6 give typical 1.8 V supply/reference and
input range 0–VCC-ADC, with operating supply limits unspecified. Section 5.2's
2.1 V absolute maximum is not an operating tolerance or an admission target.

Before one passive board attempt, the coordinator must establish:

- **Target population:** identify the selected board revision and a trustworthy
  population record or physical inspection matching the four input routes.
  Confirm the AVCC feed resistors are present, R28's fitted value or absence,
  and any additional lower components or modifications. Exact selection values
  are needed for expected-code interpretation; the passive safety bound needs
  the same-rail topology and the source-resistance bound.
- **Electrical state:** bind stable, enabled CLDO3 at its admitted programmed
  1.8 V setting to that board and powered lifetime. Confirm the AVCC/ground
  connection and absence of external drive or loading on these inputs. If the
  revision or population cannot establish the range and impedance bounds,
  obtain board-specific voltage or resistance evidence before conversion.
  The PMIC snapshot verifies configuration, not voltage, ripple or damaged wiring.
- **Reference state:** retain the inherited internal reference selection and
  establish usable AVCC/reference operation under the reviewed electrical
  authority. Claim only raw conversions; voltage and reference accuracy need
  measurement against an independent reference.
- **Execution ownership:** satisfy the existing frozen-image, PMIC/TWI/pin,
  clock, controller, IRQ, security and recovery prerequisites. Retain all
  supplies, configurations and reservations after configured disable and ROM
  return as described by the returning-client procedure.

No resistor change, signal injection, boot-pad contact or voltage adjustment is
part of this test. Source inspection supplies the circuit calculation; physical
records must bind its assumptions to the actual target.

## One retained task owns sampling and deferred interrupts

The complete 784-byte session is authenticated and retained by common and local
EL3 seals before providers are constructed. Prefix consistency prevents mixing
a board admission with a different ADC extension. The session binds owner,
clock policy, APB1, supply setting, impedance bound and physical evidence.

A real common-registry token reserves the GPADC controller exclusively. Stable
GPADC24M adoption permits coexistence with another compliant thermal consumer.
Cold parent setup instead requires an exclusive GPADC24M token and excludes all
thermal/ADC users. A stable token never authorizes a parent change. REF, OSC and
fabric remain stable, with PERI0 retained when APB1 uses it. One common register
mutex protects startup, sampling and actual native accesses. The native adapter
checks the privileged running FreeRTOS owner, task/interrupt state, mutex holder
and exact register aperture/masks before I/O.

Startup requires independently established inactive controller, absent inherited
FIFO/DMA/handlers, security/mappings and remote-writer exclusion. It enables only
owned clock/gate fields, refuses inherited activity and enables conversions last.
No terminal path asserts reset, changes a voltage, restores pins or releases
resources. Software deadlines cannot recover a stalled MMIO transaction.

GIC interrupt ID 70 is level-high, Group1 nonsecure, routed to the primary CPU.
Its ISR performs a bounded mask and publishes an atomic pending indication.
It does no ADC access, common mutex operation or driver-state mutation. The owner
exchanges pending before service, copies each selected ready channel and then
acknowledges only observed flags. Rearm is synchronized with ISR publication and
bounded against repeated contention. A periodic task poll also services data;
it does not erase a notification arriving during the read.

Data registers hold latest values. A conversion may replace a value while a flag
is already set, and another conversion may arrive during copying or acknowledgement.
The API supplies neither lossless event counts nor simultaneous sampling. Each
record includes its own observation time, channel, raw code and service sequence.
After every channel has been observed, the accumulator resets its valid mask;
a second set requires a later service sequence and timestamp for each channel.
Duplicate channels do not count twice toward a complete set.

Completion permanently masks the GIC input and keeps every resource, ADC backing
and PMIC borrow retained. The report is a historical observation within the
supervised lifetime. Cancellation, deadline or uncertainty revokes copied-output
eligibility immediately. Only the owner retries clock/rail retention metadata;
it never continues healthy sampling to obtain a fault. Output checks run before
snapshot, after copy and before queued transmission.

## Build and capture one authenticated report

Configure the pinned board/toolchain dependencies with
`SUNSTONE_BOARD_SUPERVISED=ON` and `SUNSTONE_A733_ADC=ON`; build
`sunstone-a733-adc`. Its final manifest is `adc-application/manifest.json`.
`apps/adc/prepare.py` accepts `--image`, `--manifest`, `--admission`, `--template`,
`--output`, `--clang` and `--lld`. It binds the complete session and reviewed
entry facts before making the immutable SRAM-launch/input bundle.

`apps/adc/capture.py` reuses the existing supervisor, queue claim, port lock,
USB access/discovery and failure retention. Its arguments are `--bundle`, `--fel`,
`--evidence`, `--database`, `--claim`, `--owner` and
`--supervised-manual-reconnect`. USB PID `0xefe8` has a distinct ADC product and
serial. A 128-byte header authenticates the full session and 160-byte copied
payload. Two ordered four-channel sets follow programmed-supply/reference/divider
metadata. Pending/failure frames carry no output. Host success requires durable
raw frame and checked payload copies.

## Physical qualification and broader modes remain required

The coordinator may prepare one bounded RAM-only attempt after board quarantine
and recovery are resolved. Entry evidence must identify actual board/divider
population, passive observation authority, safe/stable AVCC and reference,
input range and source impedance. It must also establish CPU/memory/coherence,
USB/PMIC/counter authority, exclusive controller/IRQ/security ownership and the
selected shared-parent policy. No external signal or strap modification is requested.

Expected output is two complete later raw observation sets or a retained failure.
No board identification, calibration, voltage accuracy or interrupt delivery is
inferred from a software model. Keep the claim and backing until qualified whole
board power removal; cable reconnection alone permits no resource reuse. There
is no automatic reset or retry.

Repeated/concurrent monitoring services, calibrated voltage/reference accuracy,
threshold modes, FIFO/DMA and other usable ADC operating modes remain required.
The accessory-dependent GPADC4, separate LRADC0 interface and actual populations
remain tracked independently. This profile does not claim complete ADC support.

## Software evidence validates the bounded application path

The frozen batch in `~/.cache/sunstoneos/evidence/adc-application-20261018/`
contains eighteen new sanitized core cases, actual CLDO3 profile/selector checks,
one supervised native link, production admission/full-seal checks, sixteen linked
application cases, four host groups and a nondeployable offline launch bundle.
The actual core and common registry run against modeled MMIO before native
configuration. Cases cover cold/adopted clocks, raw zero and later fresh samples,
unsafe inherited modes, foreign-channel status, observed-only acknowledgement,
reassertion, unchanged output on access uncertainty and metadata-only retention.

Linked cases execute sealing, pure core initialization, native owner/MMIO filters,
application accumulation, ISR/deferred races and copied-output instructions.
Core and shared-provider results, GIC primitive outcomes, translations, time,
RTOS scheduling and USB transport are modeled. The wrong-exception-level guard
case models only the architectural `CurrentEL` register value at its identified
instruction. Unicorn did not track the requested exception-level change; an EL0
attempt trapped at privileged `MRS DAIF`. This is compiled-branch coverage,
not EL2 execution or EL0 isolation qualification. The native callback API requires
privileged entry and supplies no syscall boundary.

Existing thermal parent/GIC/shared-provider evidence and the accepted high-bit
clock-registry case are reused. These results establish no physical sampling,
voltage accuracy, interrupt delivery, strap interpretation or recovery behavior.
The bundle identifies no physical port and cannot authorize a device attempt.

## A finite returning client preserves the same electrical prerequisites

The separate [returning ADC client](../apps/thermal-returning/README.md) uses
the demonstrated small FreeRTOS platform, actual CLDO3 reservation and PMIC
snapshot, production ADC driver and common clock/mutex providers. It gathers
two fresh four-channel sets and performs configured disable before returning
through the RAM log. ADC interrupt delivery stays masked; expected pending
IRQ70 remains owned and is reported. Clocks, CLDO3, PMIC/pin reservations and
controller configuration remain retained. No analog-drain, restoration or
physical conversion result is claimed by the software checks.

ADC clock validation now uses the same normalized-reference decoder and mux
policy as thermal: SYS24 supports 24, 26 and 19.2 MHz oscillators; direct DCXO
requires 24 MHz. Cold setup preserves the direct 24 MHz path and selects SYS24
for the other admitted oscillators. PERI0 bit 24 remains reserved. A focused
sanitized check exercises actual registry and driver start/read/stop with these
profiles. This correction does not relax electrical or controller admission.

## LRADC0 raw observations require separate electrical authority

The same ADC application implements a fixed LRADC0 mode in session version 2.
The 784-byte session and copied report retain their extents. Version 1 keeps
its GPADC0–3 behavior. The LRADC mode copies two later observations of channel
0, each with a timestamp, sequence and six-bit raw code. It supplies no key map,
voltage conversion or board-ID interpretation.

**Physical LRADC sampling is blocked by an unresolved electrical mismatch.**
The V1.2 schematic connects LRADC0 at ball 1D23 through R23, 10 kΩ, to AVCC.
The inherited CLDO3 profile programs nominal 1.8 V. A733 manual section 18.13.1
and datasheet V0.93 section 3.10.9 specify a 0–1.35 V LRADC input range. The
schematic has no five-key resistor ladder matching the kernel board node.
Neither a saturated conversion nor that key configuration establishes safe
idle operation above the conversion range. The datasheet supplies no explicit
idle overrange tolerance. No qualifying electrical determination exists for
either board.

The offline producer requires a separate target- and image-bound electrical
determination for the actual R23/AVCC connection. It must explicitly establish
safe idle overrange at the admitted supply, with the documented 1.35 V range
and nominal 1.8 V pull-up stated separately. The ordinary reviewed-evidence
flag cannot replace this record. Actual population, supply, reference, mappings,
security and remote-writer exclusion remain required. The application only
reads the inherited CLDO3 setting; it never changes voltage or drives a pad.

The [LRADC source manifest](../apps/adc/lradc-sources.json) pins the effective
A733 Linux LRADC driver, device trees, clock driver, manual and schematic.
The native FreeRTOS driver adapts their register and clock sequence, alongside
the existing GPADC driver's ownership and retention code. It uses channel 0
only, matching the A733 external signal table. Linux is a source reference;
no Linux runtime or ADC firmware is loaded.

The controller is at `0x02524000`; the effective A733 device tree assigns
SPI 37, GIC interrupt ID 69. CCU offset `0x1024` owns gate bit 0 and reset bit
16. The manual identifies RTC32K as the sampling source; the Linux `dcxo`
parent annotation describes the bus gate and does not establish the sample
clock. The driver observes either the external low-speed source or normal
internal-RC divider path. It refuses source changes and automatic switching.
The slowest sample selector is 3. The manual calls its sample rate 250 Hz;
the Linux macro calls it 32 Hz. The manual also describes eight-sample
averaging. The report carries the selector without promising a measured
sample or update rate.

A real exclusive LRADC lease protects its controller, local gate/reset and
interrupt. Stable RTC32K, RC16M, oscillator, reference and fabric reservations
remain held, with PERI0 retained when APB1 selects it. Existing RTC initialization
and its adapters do not participate in the clock registry. This fixed mode
therefore separately requires exclusion of every legacy RTC source,
calibration and oscillator writer; the new RTC32K scope alone does not establish
coexistence. The source and relevant divider fields are checked throughout
sampling. No RTC source, parent clock or calibration register is written.

The authenticated private session selects a boot check that refuses inherited
interrupt-69 enable, pending or active state before distributor changes.
The application still inherits the original whole-board GIC authority and
GPADC interrupt setup. It makes no narrow coexistence claim. Interrupt 69
stays masked. Only the local DATA0 interrupt is enabled, following the pinned
driver; the task polls its pending flag under the existing register mutex.
There is no LRADC ISR or channel-1 access.

Startup requires the local gate off and established reset ownership. It
deasserts reset, opens the gate, refuses inherited conversion or local interrupt
enables, clears an observed stale DATA0 flag and verifies a clear boundary.
It then enables channel-0 normal sampling last. A copied observation requires
a newly pending DATA0 flag. After copying, the task acknowledges only that flag
and verifies another clear boundary before accepting a later observation.
Because the converter keeps sampling, the next conversion can set DATA0 again
between the acknowledgement and its readback. The driver acknowledges one such
reassertion and then requires a clear readback. Two conversions cannot complete
within those microseconds, so a flag that stays set after the second
acknowledgement is treated as an ignored write and retains resources.
The power-on data value 63 alone is not a conversion. These observations are
not lossless conversion counts.

After the second observation, the task clears controller enable and local
interrupt enable and checks their readback. This configured stop does not prove
analog drain. Every clock, supply, configuration and memory reservation remains
owned. Ambiguity, lost authority, expiry or uncertain access ends copied-output
eligibility and retains resources. There is no reset assertion, restart or
release path.

The copied 160-byte payload identifies LRADC with reference-kind field 2,
sample-selector field 3 and channel mask 1. It preserves the two existing
64-byte observation groups; each uses only its channel-0 record and zeros the
three unused records. The host validates raw codes 0–63, strictly later time
and sequence, zero padding and the complete session/output digests.

The focused software evidence contains 15 LRADC driver cases, one additive
lease-conflict check, one GPADC denied-entry correction case, one linked native
LRADC application path, four native write refusals and seven host refusals.
The native path executes the actual session validator/publication, task wrapper,
clock registry, register mutex, controller driver, native access adapter,
collector, serializer and host capture. It copies fresh raw codes 0 and 63,
then observes configured conversion and local-interrupt disable. PMIC/rail
service results, board readiness, RTOS APIs, translations, analog conversion
and USB transport are modeled. The function-call model runs at EL1h; it does
not construct or validate actual FreeRTOS task contexts or SMP scheduling.

The coordinator separately passed two SRAM admission cases and five inherited
interrupt-state guard cases on the same image. The corrected model session
reuses that exact launcher binary and its accepted publication fields; its
runtime stamp is derived rather than another executed launcher result. The
original fixture's single-core USB profile failed the native validator before
publication, providers or MMIO. Its failed state and source remain preserved.
The corrected fixture selects the cold-reference profile required by this
cached-SMP image. No native rebuild or old device matrix replay was needed.

Exact image, 171 linked objects, 451 actual compiler inputs, original and
corrected typed bundles, checkpoints and copied response are retained under
`~/.cache/sunstoneos/evidence/lradc-20261020/`. The `native-frozen` manifest
records the compiled closure; `operation-frozen` records composed results and
both fixture states. The electrical records in these bundles are explicitly
fictitious and authorize no physical use. Physical conversion, voltage safety,
reference accuracy, timing, IRQ delivery, analog drain and recovery remain
unqualified. No hardware, USB device or queue was accessed.
