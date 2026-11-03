# Use the vendor's 5.4 Gb/s DisplayPort rate

Status: implemented and host-tested. No board run qualifies the rate; the
retained image has no qualified normal return and stays blocked from
scheduling.

The fixed four-lane native DisplayPort profile uses 5.4 Gb/s per lane, also
called HBR2. The pinned Trilinear controller reports that maximum through
`trilinear_get_max_rate()`, and the pinned Cadence PHY supplies the matching
rate sequence and signal-level table. Native output is 1280 by 720 at 60 Hz,
with a 74.25 MHz pixel clock and RGB888 on the link.

The source manifest is `apps/displayport/sources.json`, at Linux revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`; source code executes through the
FreeRTOS adapters. The source maximum comes from `trilinear_dp14.c`, the PHY
tables from `sunxi-cadence-combophy.c`, and training-pattern selection from
`sunxi_edp.c`.

One internal constant (`A733_DP_LINK_RATE_MBPS`, 5400) selects the rate in PHY
initialization, signal-level programming, sink admission and controller
programming. A sink must advertise at least 5.4 Gb/s and four lanes; a
lower-capability sink is refused before transmitter enable, and there is no
automatic rate fallback. Channel equalization selects training pattern 3 when
the sink advertises it and pattern 2 otherwise. The receiver enters training
before the source.

For the fixed output, the vendor's calculations give bandwidth code 20, MVID
74250, NVID 540000, transfer-unit word `0x09060040` and secondary-data window
945. The transfer-unit word describes six whole symbols and nine sixteenths per
64-symbol unit; the source calculation truncates the fraction.

An immutable linked rate marker identifies 5400 Mb/s to host preparation.
Physical analog and SERDES authority must explicitly cover that rate. The
shared preparer rejects missing or lower-rate authority and a mismatched linked
marker, so such an image cannot be scheduled. Session sizes and application
interfaces are unchanged; the build identity binds the driver and marker.

Initialization runs once. Training allows at most ten clock-recovery attempts
and five equalization attempts, with four lane-level settings per receiver
update. Accepted operation adds no allocation, data copy, translation, lock,
periodic wakeup or extended timeout, and the service and stop paths keep their
bounds. All failures retain clocks, rails, PHY ownership, mappings and pixel
memory. The rate uses the existing supplies and power domains and adds no
voltage or domain transition.

## Checks and qualified figures

`checks/displayport-top-rate.py` compares the linked PHY trace with the pinned
vendor functions for both cable polarities and legal signal levels, checks the
transport calculations separately, observes actual source and receiver rate
and training-pattern writes, and refuses lower-rate sinks. Controller fault
cases cover AUX timeout, repeated DEFER, short replies, lost clock recovery and
configuration mutation. Host preparation checks reject lower-rate authority and
accept only explicit 5.4 Gb/s authority with the linked marker. All fixture
facts are modeled; production authority rejects them.

| Figure | Value | Record |
| --- | --- | --- |
| Matching PHY traces; linked controller cases | 22; 4 (both polarities, patterns 2 and 3) | [displayport-top-rate.jsonl](../evidence/displayport-top-rate.jsonl) `displayport-top-rate-host` |
| Preparation MMIO and wait operations | 1483 (1487 at 1.62 Gb/s) | same |
| Operations per signal-level update | 26 | same |

## Open work

A board run needs coordinator approval of this clock point, matching preparer
validation, a reviewed watchdog return path, independent AUX host-clock
evidence and a suitable Type-C DisplayPort sink. Physical frequency, signal
quality, sink output, interrupts and drain are unqualified.
