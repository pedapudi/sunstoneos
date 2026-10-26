# Select one native camera connector from authenticated clock ownership

The native camera application already captures a finite continuous stream on
CAM1 and publishes one immutable NV16 copy. CAM2 has a native TWI9 provider,
clock lease and controller-level receiver support, but the camera binding fixes
its pins, MCLK, receiver and parser to CAM1. The implementation connects the existing
OV5640 consumer to CAM2 without adding concurrent cameras or a new service.

## The existing session selects one complete route

The authenticated media configuration selects exactly one MCLK:
`camera_mclk_mask = 2` means CAM1 and `4` means CAM2. Zero, combined and other
masks are refused before allocation. The native constructor freezes the derived
port. Each poll checks the selector before media preparation, and live authority
requires it to keep matching that port.
The boot record and session layout remain unchanged.

| Resource | CAM1, port 0 | CAM2, port 1 |
| --- | --- | --- |
| CSI data/clock pins | PK0–PK9, function 2 | PK10–PK19, function 2 |
| Module clock | PE5, function 2; `0x02003804` | PE9, function 2; `0x02003808` |
| Module power-down | PE6, asserted high | PE10, asserted high |
| Sensor bus | TWI11, PE7/PE8 function 8 | TWI9, PE14/PE15 function 8 |
| Local PHY | `0x05810100` | `0x05810200` |
| Packet port | `0x05811000` | `0x05811400` |
| Parser | `0x05820000` | `0x05821000` |

The ISP1/VIPP0/DMA0 path remains shared and exclusively owned. The route selects
the chosen parser. Common analog initialization retains its existing access to
all local blocks while sensors are quiet; local receiver programming uses only
the selected port. No generic routing table or new bus provider is needed.

Inherited pin entries keep their current roles: ten CSI pins, MCLK, power-down,
two bus pins and optional reset. Reset remains active low and must avoid every
pin of the selected route. The power-down-only profile requires no reset pin
or inherited reset entry. Pin admission verifies the actual inherited latch
levels before reconfiguration.

The required `settle` and `deskew` fields retain their admitted values; no CAM2
default is added. Host admission binds both the camera record, including timing
and reset wiring, and the media record, including connector selection, to the
same reviewed entry facts. Changing only the MCLK mask invalidates that evidence.
Native and host checks reject missing deskew and selected-route reset conflicts.
No model field is physical timing or electrical authority.

## Pinned references establish connector routes

The Linux source revision is `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.
The [source lock](../../sources.lock.json) pins the following files:

- `arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi`, SHA-256
  `1c7313f655bdc4d018201664adcb1fa1d100280388b8975e193ebbfc6ad4ddfa`:
  `csi_mclk2_pins_a`, `mipib_pins_a`, `twi9`, `csi1` and `mipi1` define
  the clock pin, data pins and controller addresses.
- `arch/arm64/boot/dts/allwinner/sun60i-a733-orangepi-zero3w.dts`, SHA-256
  `31145367f604fd756f2c6a67447e5134e4be0ad5fae87672cfaa9b7c6d055a39`:
  `twi9_pins_default` selects PE14/PE15; `sensor2` selects TWI9, MCLK2 and
  active-high PE10 power-down. That node describes an IMX219, so it does not
  establish OV5640 module compatibility or supply voltages.

[Board wiring](../board-wiring.md) records CAM2 on schematic sheet 13.
The existing pin tables and TWI9 provider encode the applicable function codes;
the existing media provider programs MCLK2. Their ownership and clock-rate
contracts are reused. OV5640 reset polarity and sensor programming retain the
pinned sensor implementation. [Stop evidence](../camera-stop.md) still requires
retention after sensor stop because complete transaction drain is unproved.

## Costs and selected checks

The capture and copy paths retain one native poll per tick and at most 4 KiB
copied per call. The finite stream still uses 150 copy calls for 614,400 bytes.
Port selection adds no MMIO, cache operation, barrier, lock, allocation or poll.
A frozen-port comparison in existing authority checks rejects a changed media
selector. Address selection adds integer arithmetic to existing local accesses.
The model budget is the same access count for CAM1 and CAM2, with the existing
status-read interval preserved. Native object size and model access counts are
recorded before and after implementation.

Extend the existing native composition model with CAM2 still capture, retained
rearm and finite streaming. Require the selected pin masks, TWI9 gate and MCLK2,
parser-one routing, complete copied payload and retained faults; forbid local
CAM1 writes. Exercise zero/multiple selectors, changed selection after
construction, reset conflicts, missing timing and wrong inherited power-down.
Use the existing application admission and host preparation checks to bind
CAM2 to reviewed facts and reject a changed selector under CAM1 evidence.
Build the existing stream image and exercise the actual compiled admission.
Reuse unchanged slot-race, startup-interval, USB-copy and stop/drain evidence.
No board execution or physical image-correctness claim follows from these checks.


## Software evidence covers both connectors

Results are under `~/.cache/sunstoneos/evidence/camera-cam2-1026/`:

- `native-route-corrected/` runs the real native binding and resource
  controllers with CAM2 still capture, power-down-only control, retained rearm,
  finite streaming, changed selectors and wrong inherited power-down. The first
  fixture used a parser number instead of the source-defined selector encoding
  (`parser × 4`); its failed observation remains in `native-cam2/`.
- `native-cam1/` covers the affected CAM1 paths. `live-selector/` additionally
  checks a valid output copy after selector mutation on both connectors, so
  invalid destination admission cannot conceal a missing authority check.
- `application-admission/` executes compiled AArch64 ordinary and supervised
  session admission, including CAM2 and a selector change without a new digest.
- `host-executable-admission/` calls the actual stream ELF parser, session
  profile and reviewed-entry validator. It rejects connector/timing/reset
  mismatches and names that are undefined, non-function, debug-section or
  outside executable file-backed segments. `host-image-compatibility-final.json`
  also verifies the preserved supervised still ELF. These are image/session
  admission checks; launcher and full collection checks were not rerun.

The stream image links at
`~/.cache/sunstoneos/build/camera-cam2-1026/sunstone-a733-camera.elf`.
The native driver object has 14,458 bytes of code/read-only data and eight bytes
of mutable globals, compared with 14,039 and eight bytes before the extension.
The additional 419 bytes select the connector and enforce matching authority.

The preceding driver and both current connectors each make 8,126 modeled
register accesses, including 2,359 writes, during the same finite stream case.
They invalidate two payload planes, copy 614,400 bytes in 150 calls and have a
4,044 µs maximum modeled status-read gap. Nominal still capture makes 2,348
writes, matching the accepted earlier case. `baseline-counts/` preserves the
preceding driver with the same counter-enabled fixture. No additional copies,
locks or cache operations are introduced by connector selection.

The host image parser also accepts either actual supervised initializer after
linker removal of unused functions. It requires defined `STT_FUNC` symbols in
executable sections, with the complete function backed by an executable load
segment. The image digest and reviewed camera/media facts retain their existing
binding. No new physical authority, drain proof or resource-release path is added.
