# Native OV5640 control and camera integration

The [OV5640 driver](../drivers/camera/ov5640.c) implements sensor power sequencing,
identity checks, one MIPI CSI-2 mode, stream start/stop, and bounded shutdown.
Host checks and AArch64 compilation pass. Physical sensor control, frame capture,
receiver/D-PHY operation, and A733 image processing remain unqualified.
The driver has no Linux runtime dependency.

## The first sensor mode is two-lane VGA UYVY

The fixed profile uses the Linux OV5640 initialization table, low-resolution table,
clock calculation, crop/timing definitions, and image controls. It requires a
24 MHz external sensor clock and emits 640 × 480 UYVY pixels over two CSI-2 data
lanes. The virtual channel is zero and the CSI-2 data type is YUV422 8-bit, `0x1e`.
Each active line carries 1,280 image bytes; a tightly packed frame occupies
614,400 bytes before any receiver stride or allocation padding.

The link clock is 192 MHz, corresponding to 384 Mbit/s per data lane.
The 48 MHz timing rate divided by 1,600 clocks per line and 1,000 lines per frame
gives a nominal 30 frames/s. The physical clock and frame rate have not been measured.
The sensor handles automatic exposure, gain and white balance. The initial
anti-flicker setting is 50 Hz. Initialization selects ordinary images or the
sensor's color-bar test pattern, which register `0x503d` = `0x84` enables as in
the Linux driver. The [native binding](camera-native.md) takes this selection
from its boot record. Additional modes, runtime exposure controls, autofocus and
orientation controls remain to be implemented.

The reference sensor driver is
[Linux `drivers/media/i2c/ov5640.c`](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/drivers/media/i2c/ov5640.c),
SHA-256 `cdebbcd75dd5212d26a4d762144e07fbcdb9355fe9d73fb6b7883f9d76845783`.
The adapted code and unchanged register tables retain its GPL-2.0-or-later license
and original copyright notices. The separate older board sensor driver is not the
MIPI programming reference for this component.

## Platform admission precedes sensor I/O

One privileged worker serializes the [public API](../include/ov5640.h).
The worker initializes fresh zeroed storage once, requests open with an absolute
microsecond deadline, and polls until ready or faulted. Each poll performs at
most one I2C or platform operation, plus clock reads. Masked register updates
use separate read and write polls. Delays yield to the worker; the driver neither
spins nor allocates memory.

A platform can supply an optional block-write operation. The driver then writes
a run of up to 40 configuration entries at consecutive register addresses in
one I2C transaction, relying on the sensor's register-address auto-increment.
Masked entries, entries followed by a delay and the test-pattern register are
always written alone. Every register receives the same value in the same order
as with single writes. With block writes, configuration takes 125 write
transactions instead of 308. The native binding supplies the operation; the
sensor's auto-increment behavior on this board has not been observed.

The platform acquire callback must reserve the sensor, bus, pins, clocks and
power resources. It publishes a nonzero lease before any change that could
require recovery. Successful admission proves stable 24 MHz XCLK, suitable
module rails, PWDN asserted, and RESETB asserted when RESETB has its own control.
Module-specific circuitry may combine these controls; its adapter must prove
equivalent timing and electrical behavior. Neither connector voltage nor a
matching connector establishes this.

After admission, the driver waits at least 5 ms, deasserts PWDN, waits at least
1 ms, deasserts RESETB, and waits at least 20 ms before reading the chip identity.
A module whose RESETB has no separate control, such as one that ties RESETB to a
pull-up, selects the power-down-only sequence before open. That sequence omits
only the RESETB step and keeps both waits. The Linux driver uses the same
sequence when no reset GPIO is supplied.
It requires `0x5640`. Sensor I2C uses 7-bit address `0x3c`, a 16-bit big-endian
register address, and one data byte. This address is distinct from an 8-bit
address byte containing the read/write bit. An I2C read sends the register
address and uses a repeated start for the data byte.

Initialization disables frame transmission before the reference table exits
software standby. It preserves the table's 300 ms delay, programs the VGA mode,
configures the lanes to enter the low-power LP11 idle state, and waits at least
500 microseconds. Readback verifies the clock, dimensions, format, virtual
channel, packet-disable state and selected test pattern. Ready means the sensor
has accepted this configuration; it does not prove that frames can be captured.

Stream start requires a nonzero receiver capability. The platform validates
that capability against the complete mode and retains its clocks, routing,
capture buffers and ownership before the driver enables packets. Stream stop
disables the MIPI interface and frame transmission, then verifies both control
registers. The receiver capability remains retained across stop and restart.
The same sensor instance refuses a different receiver capability.

Close is accepted only after the sensor is ready with packets disabled.
The platform release callback must prove receiver isolation and drain before
powering down the module and releasing its resources. Sensor register readback
alone does not establish receiver or DMA quiescence. Closed objects cannot reopen;
a fresh object requires a fresh platform admission.

A failed callback, changed lease identity, wrong readback, backward clock or
expired deadline enters a terminal fault. The driver retains the sensor and
receiver identities and issues no further callbacks. Recovery must independently
establish isolation before reclaiming resources. A release callback that already
succeeded cannot be undone; an overdue successful release clears the sensor lease
and leaves a terminal fault. Callback contexts and the driver object must remain
reserved while operations or retained faults can reference them.

## Connector routes require module-specific electrical verification

The [board wiring record](board-wiring.md) identifies the V1.2 schematic.
Sheet 13 routes MCSIA to CAM1 and MCSIB to CAM2. Both connectors have four
physical data lanes; the initial OV5640 profile uses lanes zero and one.
The [SoC pin definitions](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi)
identify PE5 as MCLK1 and PE9 as MCLK2.

| Connector pins | CAM1 | CAM2 |
| --- | --- | --- |
| 2/3, 5/6 | MCSIA data lanes 0/1, negative/positive | MCSIB data lanes 0/1, negative/positive |
| 8/9 | MCSIA clock, negative/positive | MCSIB clock, negative/positive |
| 11/12, 14/15 | MCSIA data lanes 2/3, negative/positive | MCSIB data lanes 2/3, negative/positive |
| 17 | PE6 module control | PE10 module control |
| 18 | PE5, MCLK1 | PE9, MCLK2 |
| 20/21 | TWI11 clock/data | TWI9 clock/data |
| 22 | `VCC_3V3_CSI` | `VCC_3V3_CSI` |
| 1, 4, 7, 10, 13, 16, 19 | Ground | Ground |

The [board device tree](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/arch/arm64/boot/dts/allwinner/sun60i-a733-orangepi-zero3w.dts)
selects IMX219 configurations and marks PE6/PE10 as power-down controls.
It does not establish OV5640 module compatibility. Its disabled alternative
sensor also names different reset pins. Use the schematic and the actual
module's circuit to establish reset/power behavior, I/O voltage, cable orientation
and lane assignment before connection. The board description supplies the
camera pin domains from a 1.8 V rail; the connector's 3.3 V module supply does
not establish the sensor's individual digital, analog or I/O supply voltages.

## CSI, D-PHY and ISP support remain separate implementation requirements

The [A733 datasheet](https://dl.radxa.com/cubie/a7a/docs/hw/datasheet/A733_Datasheet_V0.93.pdf),
version 0.93, section 3.7.1, specifies CSI-2 1.1, D-PHY 1.2 and up to
2 Gbit/s per lane. The three receivers have four, four and two lanes.
The board's two connectors expose the first two lane groups.
The native receiver must configure the two-lane profile explicitly and preserve
ownership of the unused lanes and shared PHY resources.

The pinned [camera platform definitions](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/vin/platform/sun60iw2_vin_cfg.h)
and device tree identify these register groups:

| Function | Register base addresses |
| --- | --- |
| Camera clock controller and top-level routing | `0x05800000`, `0x05800800` |
| CSI receivers | `0x05820000`, `0x05821000`, `0x05822000` |
| Combo-PHY register windows | `0x05810100`, `0x05810200`, `0x05810300` |
| Associated MIPI packet-port windows | `0x05811000`, `0x05811400`, `0x05811800` |
| Camera DMA channels | `0x05830000` through `0x05835000`, stride `0x1000` |
| ISP and scalers | `0x05900000`; `0x05910000` through `0x05910c00`, stride `0x400` |

The receiver implementation needs power and clock leases, reset ordering,
PHY lane/settle configuration, parser routing, packet error handling and interrupt
acknowledgement. Capture needs admitted buffers, IOMMU/cache visibility, stride
and format handling, frame ownership, cancellation and verified drain.
The camera DMA block is distinct from the general-purpose DMA controller.

UYVY sensor output permits an initial capture path using the sensor's own image
processing. Native A733 ISP support still requires raw formats, scaling,
statistics, exposure/white-balance integration, tuning and applicable concurrent
sensor modes. No ISP functionality is established by the OV5640 host checks.
The receiver must establish the real connector-to-parser route; device-tree
sensor numbers are not sufficient proof of physical lane routing.

## Hawkeye full-resolution support remains unresolved

The [Hawkeye documentation](https://docs.arducam.com/Raspberry-Pi-Camera/Native-camera/64MP-Hawkeye/)
identifies a 9,152 × 6,944 full-resolution image. The A733 datasheet gives
3,264 × 4,224 for online ISP operation. Its offline limits disagree between
sections: section 3.3.1 gives 5,874 × 4,224, while section 3.7.1 gives
5,664 × 4,248. Both exclude a full Hawkeye frame through the documented ISP path.
Do not infer a valid raw bypass from this discrepancy. Receiver/parser widths,
line stride, DMA addressing, packet sizes and a real sensor mode must all be
established independently. Lower-resolution modes remain candidates. Hawkeye
sensor/actuator programming, firmware terms and physical module compatibility
remain separate work; OwlSight documentation describes a different product.

## Validation evidence is reusable for unchanged inputs

The changed behavior is bounded sensor configuration and stream ownership.
The principal risks are wrong clock/timing bytes, premature transmission,
skipped settling delays and resource reuse after partial failure. One combined
host batch resolves the software aspects of those risks:

```sh
python3 checks/ov5640-reference.py \
  --source "$HOME/.cache/sunstoneos/sources/camera/drivers/media/i2c/ov5640.c" \
  --evidence "$HOME/.cache/sunstoneos/evidence/ov5640-20261016"
```

The fixture passes under AddressSanitizer and UndefinedBehaviorSanitizer.
It checks minimum delays, chip mismatch before writes, bounded polls,
pending admissions, normal stop/restart/close, receiver retention, uncertain
GPIO/register/release operations, clock failure/regression, deadline expiry,
changed lease identity, readback mismatch and terminal silence. A block-write
run must reproduce the single-write access sequence register for register, with
no masked or delayed entry inside a block; failed and overdue block writes
retain the lease.
An independent harness executes unchanged Linux clock, timing and control
functions with the pinned initialization tables. Its complete modeled 65,536-byte
sensor image matches the native driver for the selected color-bar profile.
The reference harness supplies transport and Linux-type stubs; it does not
execute a Linux kernel or simulate sensor electronics.

The AArch64 freestanding object compiles with Clang 21 and contains 5,364 bytes
of code and read-only data, with no writable static data. Its largest reported
direct stack frame is 64 bytes. It requires the runtime `memmove` implementation.
No runtime performance was measured. Raw command logs,
source hashes, extracted reference functions, binary identities and target
metadata are under `~/.cache/sunstoneos/evidence/ov5640-20261016/`.

The [native capture path](camera-native.md) captures retained still images from
CAM1. Its boot record can request up to fifteen discarded images before the
readable one. Each discarded image is a complete sensor stop and start through
the [retained rearm sequence](camera-capture.md). Continuous capture and wider
modes remain to be implemented.

CAM2 has no native binding. The receiver, route and capture coordinator accept
port and parser one, and the media clock provider can prepare MCLK2 at
`0x02003808`. A CAM2 profile still needs a native TWI9 controller at
`0x02519000` with its own clock-registry entry, because the TWI engine and the
clock registry provide only TWI11 for the camera. It also needs a qualified
deskew count for port one. The pinned device tree places CAM2 data on
PK10–PK19, MCLK2 on PE9 and the module control on PE10.

Physical validation requires an electrically matched OV5640 module,
independent recovery and a coordinator-owned board test. The first
sensor-to-memory acceptance result is a color-bar frame with verified
dimensions and plane ordering, followed by transport error handling and
repeated stop/start through discarded images.
`apps/camera/observe.py --image <frame> --color-bars --ppm <file>` checks a
saved frame for vertical-bar structure and writes a review image. No board
request or physical camera test has run for this component.
