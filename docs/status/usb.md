# USB device, host and Type-C

**Status: implemented.**

Native USB0 device transport and bounded xHCI/TinyUSB
host classes have host and instruction-model evidence. Device results do not
qualify source power or host peripherals.

**Board-qualified scope:** High-speed inherited-clock USB0 PIO status, echo,
logging and return pass. Native takeover clears SoftConn, completes a 40-byte
exchange, quiesces USB and returns responsively in the frozen records
`board-session-20261023/port-3-3/r9-v3-usb/` and
`board-session-20261023/port-3-3/r9-v4-usb/` under
`~/.cache/sunstoneos/evidence/`.
The [device](../usb.md) and [platform](../usb-platform.md) records bind inputs.

Cold USB0 clock/reset/PHY preparation is host-checked but keeps the device
disconnected. Physical cold start, fabric/security ownership and wider
USB1/USB2/DWC3 operation remain unqualified. The inherited SESSION refusal and
32-bit PCTL SoftConn failure are resolved in the byte-store takeover path;
[the worklog](../hardware-worklog-20261031.md) retains their exact input scope.

## Host and Type-C

**Board-qualified scope:** None.

TYPEC24, the USB-C/DisplayPort connector, requires a USB-C adapter or hub.
The native stack supplies Type-C ownership, USB2 and selected Gen1 profiles,
bounded hubs, HID, BOT sector reads/recovery, serial bridges, UVC and UAC1
microphone capture. Serial/camera services expose retained application views;
optional MJPEG consumption uses the hardware JPEG decoder. Hot-plug inside an
admitted adapter/hub connection is implemented; removing TYPEC24 requires a
new source session.

Steady HID, serial and UVC completion uses IRQ notifications and the shared
architectural timer. Raw owner and application notification waits avoid tick
rounding; application retirement still has a legacy tick loop. Ready
work has a 32-pass bound. Noninterruptible startup and delayed GIC status use
bounded 50 µs retries without extending device deadlines. Lost or late timer
delivery expires the supervisor's observation window. Storage and keyboard
mailboxes notify both their owner and consumer at publication boundaries.
Physical timing and idle current remain unmeasured. The
[real-time audit](driver-realtime-audit.md) records the remaining service,
batching, copy and power gaps.

The JPEG variant retains its existing owner cadence: setup, decoder task-end
and chunked readout have no connected IRQ81/readiness wake path. Serial
submission supports an admitted EL1 caller directly, but applications run at
EL0 and still use the owner mailbox. A bounded privileged submission interface
and more than one outstanding TX per endpoint remain open. Controller/PHY
idle gating needs a proven halt and completed DMA writes; it is not enabled.

The corrected class package and composed sanitizer/traffic checks are under
`~/.cache/sunstoneos/build/integrate-streams-1031/usb/`, including `images/`.
[The image guide](../usb-host-images.md) lists exact hashes and the superseded
queue entries. Reused addresses, short HID descriptors and legacy keyboard
state publication have focused corrections. Public C270 traffic contains
negotiation without completed image payloads; FT2232H FPGA/FIFO captures do not
establish serial baud timing.

The linker and existing uncached allocator own `0x42200000..0x42a00000`
for USB classes. The first 5 MiB lease holds the controller; retained JPEG
allocations fit in the remaining 3 MiB. This replaces the conflicting
`0x47000000..0x48000000` range and leaves the NPU pool at
`0x47400000..0x47c00000` disjoint. Physical RAM ownership and remote-writer
exclusion still belong to runner admission. An optional session extent checks
geometry without treating a digest as authority.

The protected runner accepts finite `usb-class` probes through the existing
watchdog marker, real quiescence hook and normal-return receipt. It requires
coordinator approval for USB source/cutoff authority, including alternate
probe launch kinds. The four OS images retain resources and still lack real
host drain and a returning lifetime; the probe route does not admit them.
VBUS enable requires coordinator approval and an admitted source session.
Gen1 needs at least 900 mA plus image-bound COMB0/topology authority; no physical
bundle exists. Earlier queue entries need corrected hashes or revalidation.

Remaining work includes physical devices, DMA visibility, interrupt delivery,
source current/cutoff, sustained traffic, capture contents/continuity, serial
flow control and overruns, repeated attachment and recovery. Wider classes,
isochronous OUT, higher SuperSpeed modes, PD/VCONN, role swaps and applicable
alternate modes remain open. See [host USB](../usb2-host.md),
[Type-C](../typec.md) and [host image evidence](../usb-host-images.md).

## Shared camera contract and software evidence

UVC binds from class/subclass and descriptor data; serial vendor bridges use
the existing VID/PID tables. No camera product ID selects the driver. UVC
shares modes and retained frame spans with CSI through
[`sunstone_camera.h`](../../include/sunstone_camera.h). The native USB consumer
uses its mode/view conversion helpers without copying frame payloads. The
generic adapter checks deadlines and retains leases on expired operations.
Native exact-mode selection, shared application-kit lifecycle binding and
verified stream stop remain incomplete; the compatibility USB camera API
continues to serve current applications.

Evidence is under `~/.cache/sunstoneos/build/usb-host-board-1101/`:
`memory/check-final/` covers actual allocator/linker/table/session behavior;
`memory/usb-npu-result.json` checks the selected combined CMake/ELF layout.
`final-runner/` and `final-runner-arena/` cover admission, typed extents and
the exact uncached-arena exception. `final-generic/` and
`final-shared-camera/` cover retained class traffic with read-only aliases.
`final-deadline-health/`, `final-precise-production/` and
`final-mailbox-publication/` cover deadline loss, IRQ wake and mailbox
publication. Independent reviews cover memory, runner, camera and completion
changes. Registers, timer delivery and cache effects are modeled; these
records establish no physical host qualification.

The raw and JPEG OS baseline links pass with their respective precise and
legacy owner selections. The supervised, resident and resident-audio baselines,
selected integration gate and check index pass under
`integration/gate-usb-board-final-runtime/`. The largest recorded build/check
child uses about 143 MiB; the shared host retains over 113 GiB available.
No containers or board operations run. These baseline builds are software
checks; no new qualification image package is frozen.

## TYPEC24 power wiring and fault limits

The V1.2 schematic, sheets 9 and 11, connects PL8 through TYPEC_EN only to
the enable input of U3, the SY6280 switch. R45 pulls that input down through
100 kΩ. U3 switches VCC-5V to VBUS_TYPEC, which supplies TYPEC24 and the
FUSB302's VBUS sense input. Its input supply is shared: VCC-5V also connects
directly to TYPEC16 input power, the main PMIC input, FUSB302 VCONN, HDMI
through D1 and expansion-header pins 2 and 4. A dedicated PL8 enable does not
make the TYPEC24 load independent of board input power.

R46, 4.7 kΩ, connects U3 pin 3 to ground. The schematic labels that pin OCB;
the SY6280 datasheet identifies it as the current-setting input ISET. The
formula `ILIM(A) = 6800 / RSET(Ω)` gives 1.447 A nominal. The datasheet's
0.75–1.25 A limits apply to 6.8 kΩ at 5 V and 25 °C. They do not establish
the board's exact current limit or available input headroom.

U3 regulates excess current and shuts down if sustained overload raises its
junction above a typical 130 °C. It restarts at a typical 110 °C. This thermal
restart does not provide a latched fault cutoff. U3 has no external fault output.
The FUSB302's OCP bit reports its internal VCONN switch; OVRTEMP reports the
FUSB302's temperature. Neither detects U3 current limiting or temperature.
VBUS loss can reveal some overloads, but an overloaded output may remain
above the controller's voltage threshold.

The existing provider revokes host tokens on CC change, VBUS loss, FUSB302
fault, expired observations or lost authority. Its separate GPIO task lowers
PL8 even when the I2C task retains its bus mutex. That task requires a running
CPU, scheduler, clock, GPIO and interconnect. CPU/interconnect failure or reset
therefore has no established independent guarantee of PL8 disable. The
host-armed system watchdog and the switch's enable pulldown do not establish
that guarantee without a verified reset-to-output sequence.

The [source pins](../../sources.lock.json) identify these cached references
under `~/.cache/sunstoneos/sources/`:

| Reference and location | SHA-256 |
| --- | --- |
| Schematic V1.2, `board-documents/schematic-v1.2.pdf`, sheets 6, 9 and 11; render `board-documents/sheet-11.png` | `4e0e92afbb1cca97a9c8593b8f2679178d9738f242ea431ed3650e893fe28ed0` |
| SY6280 rev1.0E, `usb2-electrical/sy6280-rev1.0e.pdf`, pages 2, 4 and 7 | `589001cd56c64cc860556e9ecc4e17d14bfbac22ab04df06d8573e8c37be33b1` |
| Vendor DTS, `board-kernel/sun60i-a733-orangepi-zero3w.dts`, lines 16–24 and 2578–2587 | `31145367f604fd756f2c6a67447e5134e4be0ad5fae87672cfaa9b7c6d055a39` |
| FUSB302 rev2 primary-source extract, `typec-host/fusb302-primary-web-extract.json`, pages 14 and 28, embedded lines L702–704, L753–754 and L1430–1433 | `72ebcbba6a1df5257cce5ee1aa7107d8360c52cc0e7882e4ca742259c3e5cce5` |

The FUSB302 extract is a JSON-encoded string from an earlier primary-PDF
extraction; its hash is not a PDF hash. The cached full-PDF fetch returned
HTTP 403. The vendor DTS revision is
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. Its active-high PL8 regulator and
1,000 µs ramp delay corroborate the wiring. Its name `usb0-vbus` does not
change the physical connector route. [Type-C sources](../typec-pd-sources.json)
pin the corresponding vendor driver.

## Proposed source qualification and bounded bench test

**Status: physical enable is blocked at the design gate.** The simplicity,
hardware-performance and correctness reviewers verify the source corrections.
They withhold approval for enabled tests until independent cutoff, shared-input
headroom and exit handling have measurable limits. No power-control code or
source authority is changed. The coordinator must approve the target board
and source session before any physical PL8 enable.

The first source profile supplies USB2 default current with at least 500 mA
available to TYPEC24. Admission requires measured shared-input headroom,
including board load, cable drop, current limit and startup transients. Under
the selected board workload plus a 500 mA connector load, require VCC-5V to
remain within 4.75–5.25 V. Require VBUS_TYPEC to reach that range within the
rise budget and stay there during operation, without overshooting 5.25 V.
Record the source, cable, board revision, fitted switch, resistor and
measurement bounds.
The nominal 1.447 A switch limit cannot replace these measurements. Leave
Gen1, higher-current advertisement and larger PD contracts unadmitted.

That 500 mA budget does not admit a hub and four arbitrary peripherals. Count
the hub's own draw, each device's descriptor budget and startup transients.
A webcam can need the complete 500 mA budget. For the requested simultaneous
keyboard, mouse, webcam and serial topology, use a separately powered USB2 hub
whose upstream port has demonstrated no backfeed. Do not raise source current
from the SY6280's nominal limit. TYPEC16 continues to power the board.

Reuse the existing provider's CC debounce, VBUS checks, permanent revocation
and independent GPIO shutdown. Preserve its current budgets: a 120 ms stable
sink termination (Rd) observation before enable, VBUS rise within 275 ms,
service-entry gaps at most 2 ms, observations younger than 10 ms, and at most
5 ms to finish a
pending GPIO shutdown. Confirm low VBUS within the 650 ms shutdown budget
using a qualified comparator threshold no greater than the 800 mV safe-zero
limit and an external measurement.
Measure before and after any implementation change with identical inputs.
Record p50, p99 and maximum service gap and shutdown latency, CPU time and
idle input current; an unexplained regression blocks integration.

Timestamp the fault observation, revocation, first GPIO stop, PL8-low
acknowledgment and externally confirmed VBUS below 800 mV. The 5 ms GPIO
timer starts when the GPIO task recognizes shutdown. Progress expiry can
take 10 ms after the last observation, followed by the GPIO task's measured
service gap and its disable interval. A measured gap of at most 2 ms would
give a conditional 17 ms bound while the CPU and GPIO path remain live.
The provider's 2 ms entry guard does not enforce the GPIO task's cadence.
Bound discharge within 650 ms of the shutdown request. A five-second test
must request shutdown by 4.330 seconds, reserving the detection and cleanup
intervals. Compare the same
image with source disabled before enabling it, at identical clocks, board
load, supply, cable and meter settings. Record meter uncertainty; an idle
current increase outside that uncertainty needs an explanation before acceptance.

The vendor selects 400 kHz for S_TWI1. The current native provider caps it at
100 kHz; that cap remains an implementation gap. The default needs a derived
400 kHz divider for each admitted oscillator rate, supported by the FUSB302
timing and the board's measured rise time. Preserve vendor write ordering,
STOP completion and deadlines. Do not retune the shared APBS1/OSC parent.

The first control run lasts at most five seconds and confirms PL8 low,
no attachment token and no VBUS rise on an empty TYPEC24. A physically empty
connector has no Rd termination, so the normal provider cannot enable it.

The requested empty-port enable/disable test needs a separate diagnostic
pulse, authorized only through the runner after the power gate. It never
publishes an attachment token or starts xHCI/PHY/DMA. After confirming PL8
low, it enables the owned switch for at most 1,000 ms from the first enable
effect, including the vendor's 1,000 µs ramp interval, and then disables it.
The complete image/session budget is five seconds. Arm independent protection
before issuing enable, or trigger it from the physical PL8/VBUS rising edge.
It must cut the output by 1.350 seconds after that edge and establish VBUS
below 800 mV by two seconds, including CPU/interconnect failure or reset.
Reserve the full 650 ms discharge bound after cutoff; software acknowledgment
cannot start or extend these independent limits. The protection must discharge
the output even if the CPU leaves PL8 high. Permit no repeated pulse or
automatic rearm. Record no attached data device, PL8 high/low acknowledgments
and externally measured VBUS rise/discharge separately. Normal return requires
confirmed source-off and interrupt restoration. No pulse image is implemented
or frozen before cutoff and coordinator approval.

Loaded-source qualification uses a reviewed Rd sink/load fixture and retains
normal attachment checks. It runs for at most five seconds: verify off,
attach, enable, observe the measured load, revoke and verify off. The
source-disabled control precedes either enable test. Missing protection or
measurement equipment blocks both enable tests; the Rd fixture is required
only for normal attachment/load qualification.

Cleanup first closes host admission and requests provider revocation. Keep
the Type-C and GPIO tasks running until PL8 low is acknowledged. Continue
the I2C task, when usable, until low VBUS is observed; a retained bus leaves
discharge uncertain. Record PL8 acknowledgment and low-VBUS evidence
separately. Retain all provider resources and prohibit automatic reattachment.
A failed disable, uncertain GPIO write or expired cleanup deadline requires
the supervised physical recovery already established for that board. Do not
interpret CPU return or system reset as proof that source power stopped.

USB idle gating additionally requires demonstrated controller halt and
completed DMA writes before releasing clocks, power or DMA storage. Keep
shared parents and other USB controllers live. The [real-time audit](driver-realtime-audit.md)
records the missing drain evidence; VBUS-off acknowledgment alone cannot
satisfy that prerequisite.

This implementation can provide bounded software shutdown for observable
CC/VBUS/controller faults while the required execution path remains live.
A requirement to latch off on U3 overload, or to guarantee disable across
CPU/interconnect failure and reset, needs independent hardware current
detection, a latching cutoff and a verified watchdog/enable path. Those
capabilities are not established by the fitted schematic. Their absence
remains a blocker to that stronger fault guarantee and must not be hidden
in qualification flags or source-session identifiers.

Success, a diagnostic mismatch, assertion, timeout, cancellation and host loss
must all request shutdown before retiring either provider task. Transport
failure stops further board communication. A CPU/interconnect stall or reset
needs an independent cutoff; software cleanup cannot establish source-off
in those cases. Before implementation, the coordinator must select that
protection and approve numerical overload current, cutoff latency and maximum
delivered energy. No such equipment or reset-to-PL8 proof is established here.
