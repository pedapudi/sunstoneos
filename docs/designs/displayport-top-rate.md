# Use the vendor's 5.4 Gb/s DisplayPort rate

The fixed four-lane native DisplayPort profile uses 5.4 Gb/s per lane, also
called HBR2. The pinned Trilinear controller reports that maximum through
`trilinear_get_max_rate()`. The pinned Cadence PHY supplies the matching rate
sequence and signal-level table. Native output remains 1280 by 720 at 60 Hz,
with a 74.25 MHz pixel clock and RGB888 on the link.

The source manifest is `apps/displayport/sources.json`. Its Linux revision is
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`; source code executes through the
existing FreeRTOS adapters. The source maximum comes from `trilinear_dp14.c`,
the PHY tables from `sunxi-cadence-combophy.c`, and training-pattern selection
from `sunxi_edp.c`. No new source or firmware dependency is needed.

One internal constant selects the rate in PHY initialization, signal-level
programming, sink admission and controller programming. A sink must advertise
at least 5.4 Gb/s and four lanes. A lower-capability sink is refused before
transmitter enable. This bounded profile retains no automatic rate fallback.
Channel equalization selects training pattern 3 when the sink advertises it;
otherwise it selects pattern 2. The receiver enters training before the source.

For the fixed output, the vendor's calculations give bandwidth code 20,
MVID 74250, NVID 540000, transfer-unit word `0x09060040` and secondary-data
window 945. The transfer-unit word describes six whole symbols and nine
sixteenths per 64-symbol unit. The source calculation truncates the fraction.

An immutable linked rate marker identifies 5400 Mb/s to host preparation.
Physical analog and SERDES authority must explicitly cover that rate. The
shared preparer must reject missing or lower-rate authority and a mismatched
linked marker before this image can be scheduled. Existing session sizes and
application interfaces remain unchanged; the build identity binds the driver
and marker. Shared build-tag and source-record updates accompany the preparer
patch.

Initialization runs once. Training keeps the existing maximum of ten clock
recovery attempts and five equalization attempts, with four lane-level settings
per receiver update. Source-table execution can change the number of PHY
register operations; the host comparison records that count against the prior
1.62 Gb/s image. Accepted operation adds no allocation, data copy, translation,
lock, periodic wakeup or extended timeout. The service and stop paths retain
their existing bounds.

All failures retain clocks, rails, PHY ownership, mappings and pixel memory.
The 5.4 Gb/s source table uses the existing supplies and power domains; the
change adds no voltage or domain transition. No physical frequency, signal
quality, sink output, interrupt or drain qualification is implied.

The focused host check, `checks/displayport-top-rate.py`, compares the linked
PHY trace with the pinned vendor functions for both cable polarities and
legal signal levels. It checks the
transport calculations separately, observes actual source and receiver rate
and training-pattern writes, and refuses lower-rate sinks. Existing controller
fault cases cover AUX timeout, repeated DEFER, short replies, lost clock
recovery and configuration mutation. Host preparation checks reject the old
authority and accept only explicit 5.4 Gb/s authority with the linked marker.

A board run needs coordinator approval of this clock point, the shared
preparer patch, a reviewed watchdog return path, independent AUX host-clock
evidence and a suitable Type-C DisplayPort sink. The current retained image
has no qualified normal return, so it remains blocked from scheduling.

Host evidence is under `~/.cache/sunstoneos/build/display-g2d-1031/`.
`dp-top-rate-3/result.json` records 22 matching PHY traces and four linked
controller cases covering both polarities with training patterns 2 and 3.
Preparation performs 1483 modeled MMIO/wait operations, compared with 1487
on the prior 1.62 Gb/s image. Each signal-level update has 26 operations.
The rate table accounts for the four-operation decrease; no timeout is extended.

`dp-controller-hbr2/summary.json` records the nominal controller lifetime and
five retained-fault cases. `dp-rate-admission.json` records 18 actual admission
loop cases, including frozen revalidation. Physical authority, generic board
admission and allocation checks are explicit model boundaries in those cases.
Every fixture fact carries `modeled: true`; production authority rejects it.
These later admission cases reuse the unchanged linked PHY/controller evidence.
