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

The board audit connects AVCC to CLDO3. A733 Datasheet V0.93 section 5.2 lists
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
