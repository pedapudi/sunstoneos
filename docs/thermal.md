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
No-Auto controls and effective output status must match. Reserved PERI0 bit 24
does not select a source. Parent and APB1
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
reported to policy; neither automatic throttling nor a system shutdown policy is
implemented. These are required follow-on work for complete thermal management.

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
servicing; physical interrupt delivery remains unqualified. Sleep, thermal policy
and physical accuracy qualification remain outstanding.

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
