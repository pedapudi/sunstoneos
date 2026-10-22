# Native A733 thermal monitoring

The thermal driver implements cold initialization, factory calibration and
polling for the A733's five physical sensors. The supervised
[thermal application](thermal-application.md) binds it to the native shared
providers, deferred interrupt service and copied USB observation. It also programs alarm, hysteresis
and critical-temperature interrupt thresholds and collects their pending events.
Physical temperature accuracy and interrupt delivery remain unqualified.

| Channel | Location |
| --- | --- |
| 0 | Cortex-A76 CPU cluster, CPUB |
| 1 | DDR |
| 2 | NPU |
| 3 | Cortex-A55 CPU cluster, CPUL |
| 4 | GPU |

The manual and A733-specific thermal driver agree on these five sensors. Device
tree thermal zones referring to channels 5–7 do not establish additional hardware.
No sensor pins or external thermometer are needed for register implementation.
An independent temperature reference is needed for physical accuracy checks.

## Calibration uses the A733-specific layout

The driver reads only the SID shadow words at `0x03006244`, `0x03006248` and
`0x0300624c`. Their lowest 72 bits contain six consecutive 12-bit fields: factory
temperature in tenths of a degree Celsius, followed by sensor codes 0–4.
The last word's upper 24 bits are ignored. Device-tree DVFS data begins at bit 8
of that word, consistent with the calibration boundary.

For raw code `r`, temperature in millidegrees Celsius is:

- `-62 × (r - 2822)` when `r > 1769`.
- `-59 × (r - 2835)` otherwise.

For factory temperature `f` in tenths of a degree, the calibration register value
is `2048 - trunc((100 × f - temperature(factory_code)) / -62)`.
Signed division truncates toward zero, as in the A733 reference implementation.
The result must fit twelve bits. Missing or erased factory fields, or an
unrepresentable result for any channel, refuse initialization before any writes.
No default correction is presented as calibrated temperature.

The formula jumps from 62.894 °C at code 1769 to 65.224 °C at code 1770.
This source-defined discontinuity is preserved. Alarm configuration therefore
accepts raw calibrated codes rather than inventing a smooth temperature inverse.
Lower codes represent hotter temperatures within each branch. Hot codes must be
below hysteresis codes; enabled critical thresholds must be no higher than hot
codes when both are configured. Policy must account for the conversion boundary.

The manual's calibration table at section 7.1.6 describes a single sensor at
SID offset `0x14`; it conflicts with the active A733-specific implementation.
The implemented packed layout comes from `sun60iw2_ths_calibrate`. The SID helper
and A733 U-Boot header independently establish the shadow offset `0x200`.
The manual corroborates the calibration fields for all five sensors,
but does not independently establish the A733 conversion coefficients.

## Resource ownership and real clock setup

The driver borrows an immutable, live common-registry token. THS ownership is
exclusive and includes its controller, bus gate/reset and interrupt. Its shared
GPADC24M dependency retains REF and OSC stability. FABRIC ownership includes
stable APB1 selection; PERI0 is also required when APB1 uses its 600 MHz output.
All overlapping clock writers must use the registry and common register mutex.

The driver uses the detected raw oscillator and the shared normalized REF24
validator. GPADC clock word CCU `0x0fc0` selects SYS_CLK24M with mux zero or raw
DCXO with mux one (A733 manual section4.1.6.170). Cold configuration uses direct
DCXO at 24 MHz and normalized SYS_CLK24M for supported 26/19.2 MHz oscillators,
always with divider one. It requires exclusive GPADC24M ownership and
authenticated exclusion of every ADC/THS consumer. Stable shared ownership
permits adoption of either already enabled 24 MHz path. Direct 26/19.2 MHz DCXO,
unsupported sources and divisors are refused.

Native reads validate the oscillator, reference PLL, selected APB1 parent and
APB1 divider. When PERI0 is selected, its integer factors, software gates,
No-Auto controls and effective output status must match. The PLL factor check
is the shared `a733_clock_peri0_valid` rule in
`include/a733_clock_reference.h`, also used by the ADC and CE drivers.
Reserved PERI0 bit 24 does not select a source. Parent and APB1
registers remain read-only. The driver enables the ADC clock before deasserting
THS reset and enabling its bus gate at CCU `0x0fe4`.

THS base is `0x02522000`. Initialization refuses inherited enabled sensors or
interrupt controls. It programs the factory corrections, fixed acquisition
control `0x01df002f`, filter value 5 and period field 28. It clears stale pending
flags, installs configured thresholds and enables all five sensors last.
The A733 reference driver uses period 28; the manual's generic example uses 58.
Both use the same documented period field.

The platform must authenticate readable SID shadows, AVCC/ADC and digital supply
stability, Device mappings, register security permissions, remote-writer exclusion
and reviewed operation-specific recovery. A supervised RAM-only session keeps
every resource until confirmed whole-board power removal; its software deadline
cannot recover a stalled MMIO access.
No thermal PCK domain is described by the source. The board schematic labels for
analog supplies require physical revision/rail confirmation; this driver changes
no voltage, PCK policy or eFuse contents.

## Polling and deferred interrupt service

`a733_thermal_read` observes data-ready and alarm status, collects the latest
valid values and writes one only to observed pending bits. Zero data codes are
not published as valid measurements. Samples include a channel-valid mask,
raw codes, converted temperatures, event masks, sequence and observation time.
A sample can contain only events; callers must check its channel-valid mask.

Data-ready status establishes a conversion since the preceding acknowledgement.
Channels are not sampled simultaneously, and overwritten conversions are not
counted. A conversion concurrent with acknowledgement can be coalesced; the
next update restores data-ready status. This API reports latest values and event
presence, without claiming complete event counts or exact acquisition timestamps.

The manual identifies GIC interrupt 71, equivalent to SPI 39. It lists UART0 as
interrupt 34 while the device tree uses SPI 2, corroborating the numbering.
The native adapter does not configure GIC security, routing, masking or EOI.
The platform must reserve and mask that interrupt before setup, bind an actual
ISR/deferred service, and then use the existing GIC primitives to enable it.
The service runs in task context under the common register mutex.

An acknowledged status bit can reassert immediately. W1C success does not prove
that the interrupt line is inactive. Alarm and critical-temperature events are
reported in each sample. The fixed board policy described below acts on the
converted readings, not on these events.

## A fixed board policy throttles and stops work

`a733_thermal_policy_update` runs in the controller's owner task after every
`a733_thermal_read`. It keeps the latest valid reading of each channel and
publishes one of three levels:

- **Normal.** No trip is active and every channel has a recent reading.
- **Throttle.** A channel has reached its passive trip and has not yet cooled
  below the trip minus its hysteresis, or a channel has had no valid reading
  for more than 1 s. Consumers reduce load but keep making progress, so a
  watchdog that is fed on progress keeps being fed. A workload scheduler would,
  for example, run one worker instead of two.
- **Critical.** A channel has reached its critical trip, a channel has had no
  valid reading for more than 5 s, or supervision has failed. Supervision fails
  when the controller returns a result other than a sample, `AGAIN` or `BUSY`,
  when a sample repeats an earlier sequence number or carries an impossible
  time, or when the supplied time moves backward. The consumer stops admitting
  work and requests a supervised stop or board power-off. The level is terminal
  until power removal.

| Channel | Passive trip | Hysteresis | Critical trip |
| --- | --- | --- | --- |
| 0, CPUB | 90 °C | 2 °C | 110 °C |
| 1, DDR | 85 °C | 2 °C | 110 °C |
| 2, NPU | none | none | 110 °C |
| 3, CPUL | 90 °C | 2 °C | 110 °C |
| 4, GPU | 90 °C | 0 °C | 110 °C |

The trips come from the `thermal-zones` node of the pinned `sun60iw2p1.dtsi`,
except the DDR passive trip. Each CPU and GPU zone there has two passive
trips. The second one is bound to a CPU or GPU cooling device and is the
policy's passive trip. The first, at 70 °C for the CPUs (55 °C for CPUB in the
board device tree) and 60 °C for the GPU, only starts fans and power
accounting, which this board does not use. The GPU zone specifies no
hysteresis, so its throttle can switch on and off at consecutive readings near
90 °C. The zones that refer to sensors 5–7 describe no hardware and are
omitted. The 1 s staleness bound equals the zones' 1000 ms polling delay. The
5 s bound limits how long throttled work continues without temperature data.

The vendor DDR zone has only the 110 °C critical trip. The board adds a
passive trip at 85 °C because JEDEC specifies LPDDR5 standard refresh up to
85 °C die temperature, and no decoded boot0 setting shows derated or
temperature-driven refresh. The input is the SoC's THS DDR channel, which
measures near the memory controller and has an unknown offset from the DRAM
die. Today THROTTLE has no consumer in the OS image, whose thermal service acts
only on CRITICAL, so the trip changes the published level and nothing else.
Its board cost is unmeasured. Connecting THROTTLE to the CPU clock driver,
which would cap both clusters at 1014 MHz, waits for the DDR trip measurement
in the [DRAM controller design](designs/dram-controller.md#performance-budgets).
That measurement records the recognition real-time factor over 10 minutes
with and without the trip, DDR minus CPUB temperature per sample, and the time
each channel spends at or above 85 °C. It belongs in the same board session
that qualifies the throttle.

Every trip lies in the code ≤ 1769 branch of the conversion formula, where
temperature decreases strictly as the code increases. For example, code 1309
converts to 90.034 °C, code 1394 to 85.019 °C and code 970 to 110.035 °C. The
discontinuity at code 1769 therefore cannot reorder a reading relative to a
trip. The hardware alarm and critical thresholds are separate per-session raw
codes. The thermal application requires all five alarm and critical
interrupts, while the returning client disables them, so the policy cannot
depend on either setting.

Other tasks and cores read the level with `a733_thermal_policy_level`, and
the channels that have had at least one valid reading with
`a733_thermal_policy_seen`. Each update publishes the channels after the
level, so a task that reads the channels and then the level sees at least the
level that those readings produced. The owner's `notify` callback runs once
for each level change, after both are published. Its result is
ignored for normal and throttle. For critical, the callback returns 1 after it
has refused new work and issued its stop request. A return of 0 repeats the
request at the next update. Any other value records an uncertain outcome, and
the policy does not repeat the request. The policy object keeps the channel
masks that caused the critical level for reporting. The policy changes no
clock, voltage, power or controller state itself. The
[CPU clock driver](cpu-clock.md#the-thermal-policy-selects-operating-points)
reads its level and channels: it raises a cluster only after that cluster's
sensor has a valid reading and the level is NORMAL, lowers raised clusters to
1014 MHz under THROTTLE, and refuses every transition at CRITICAL. No image
connects the two yet. Any physical CPU PLL or voltage increase still requires
this policy to run on a qualified controller first.

## Faults retain the controller

All operations have bounded work and absolute deadlines. A failed or uncertain
access retains ownership and, when held, the common register mutex. If registry
retention is busy, later calls retry only metadata. No failed operation publishes
a partial sample or successful initialization. Terminal calls perform no MMIO.

Clock-token validation runs again after acquiring the common register mutex,
because ownership can fail while a task waits. A failed token prevents controller
access and retains the mutex. Transient registry contention releases the mutex
without accessing the controller and permits another attempt within the deadline.

Successful initialization keeps all leases and clocks live. The driver provides
no release or power-down API. The finite application supplies deferred interrupt
servicing; physical interrupt delivery remains unqualified. Sleep and physical
accuracy qualification remain outstanding. The policy acts on readings that
are not yet physically qualified.

## References and focused validation

Primary code is the [A733-specific thermal driver](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/thermal/sunxi_thermal.c),
with its SID helper, board device tree and CCU definitions at the same revision.
The inspected [mainline thermal driver](https://github.com/torvalds/linux/blob/master/drivers/thermal/sun8i_thermal.c)
contains no A733-specific compatible or calibration routine.

The A733 User Manual V1.00, chapter 7 and CCU sections 4.1.6.170–172, specifies
controller registers and interrupt semantics. Its pinned PDF SHA-256 is
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
Downloaded thermal and SID sources remain outside Git under
`~/.cache/sunstoneos/sources/thermal/`, with URLs and SHA-256 hashes in `sources.json`.

The focused check compares every nonzero twelve-bit temperature code with the
pinned conversion constants, validates packed calibration and decodes the
modeled startup trace. The host model exercises cold/shared clock admission,
five-channel and event-only reads, zero samples, invalid trim, preserved adjacent
fields, inherited activity refusal, uncertain writes and retained deadline faults.
AArch64 compilation covers the new controller and native adapter. These checks
establish software behavior. Physical accuracy and interrupt delivery require
separate board qualification.

The recorded batch passed AddressSanitizer/UndefinedBehaviorSanitizer, the
pinned-source comparison and AArch64 compilation. Controller and adapter objects
contain 8,748 bytes of text/read-only data and no static writable data. The largest
direct stack frame is 240 bytes in initialization; nested calls and callbacks
have additional stack requirements. Exact inputs, commands, logs and artifacts
are under `~/.cache/sunstoneos/evidence/thermal-20261016/batch1/`.

The separate `checks/a733-thermal-lease-wait.c` check covers token failure and
transient registry contention during mutex acquisition, for both startup and
sampling. All four cases pass with sanitizers; the changed core compiles for
AArch64. Inputs and results are under
`~/.cache/sunstoneos/evidence/thermal-lease-wait-20261017/`. The earlier conversion
and controller workloads were not repeated.

The policy check at `checks/thermal-policy.c` drives the policy from the
actual controller through modeled MMIO. It covers each passive trip and its
hysteresis, channels without a passive trip, alarm and critical events that do
not change the level, a stale channel and its recovery, a channel with no reading
for 5 s, a critical trip, repeated and uncertain stop requests, a controller
failure, replayed samples and backward time. A modeled consumer runs one worker
while throttled and records continuing progress. The check also requires the
published channels to match the policy's own before each notification,
including a first reading that throttles. The check passes with
AddressSanitizer and UndefinedBehaviorSanitizer. The existing controller,
lease-wait, clock-reference and configured-stop checks also pass with the
policy code present. For the DDR channel the policy check requires no
THROTTLE at code 1395 (84.96 °C) and THROTTLE at code 1394 (85.02 °C), holds
it at code 1428 (83.01 °C), releases it at code 1429 (82.95 °C) and reaches
CRITICAL at code 970 while throttled. The DDR trip adds 68 bytes of text to
`a733_thermal.c` (10,408 to 10,476 bytes), 68 bytes to the thermal image and
64 bytes to the OS image; the monitor image, which links no policy, is
unchanged.

## Detected reference-clock compatibility has focused evidence

The focused clock check at `checks/a733-thermal-clock-reference.c` executes the
changed actual-driver paths with accepted callback fixtures and ASan/UBSan. It
covers 24/26/19.2 MHz clock startup, normalized adoption, preserved direct 24 MHz
adoption, PERI0 reserved bit 24, and retained refusal of wrong ratios, lock loss,
unstable/disabled oscillator, mismatched APB1 and raw non-24 MHz GPADC input. Cold
configuration preserves unrelated fields; adoption performs no GPADC writes.
PLL and APB1 registers remain read-only. Existing conversion, calibration,
threshold and controller-service evidence is reused without replay.

The second board's preserved `OSC=0x983fb0f7` and `REF=0xf8675f00` decode 26 MHz
with N=96 and M=104, producing normalized 24 MHz. The earlier frozen thermal image
refuses this clock profile. Its replacement and exact inputs are retained under
`~/.cache/sunstoneos/evidence/thermal-clock-reference-20261018/`. Focused
source review accepted source selection, arithmetic and ownership. These
register observations do not establish clock ownership or physical acquisition.
