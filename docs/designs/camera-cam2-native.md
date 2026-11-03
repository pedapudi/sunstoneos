# Select one native camera connector from authenticated clock ownership

Status: host-tested. Both connectors pass the native composition model and the
admission checks; no board run exercises CAM2.

The native camera application captures a finite continuous stream and
publishes one immutable NV16 copy from either camera connector. A session uses
one connector at a time; there are no concurrent cameras and no separate
service.

## Route selection

The authenticated media configuration selects exactly one MCLK:
`camera_mclk_mask = 2` means CAM1 and `4` means CAM2. Zero, combined and other
masks are refused before allocation (`apps/camera/admission.c`). The native
constructor freezes the derived port. Each poll checks the selector before
media preparation, and live authority requires it to keep matching that port.
The boot record and session layout carry no connector field beyond the mask.

| Resource | CAM1, port 0 | CAM2, port 1 |
| --- | --- | --- |
| CSI data/clock pins | PK0–PK9, function 2 | PK10–PK19, function 2 |
| Module clock | PE5, function 2; `0x02003804` | PE9, function 2; `0x02003808` |
| Module power-down | PE6, asserted high | PE10, asserted high |
| Sensor bus | TWI11, PE7/PE8 function 8 | TWI9, PE14/PE15 function 8 |
| Local PHY | `0x05810100` | `0x05810200` |
| Packet port | `0x05811000` | `0x05811400` |
| Parser | `0x05820000` | `0x05821000` |

The ISP1/VIPP0/DMA0 path is shared and exclusively owned. The route selects
the parser. Common analog initialization accesses all local blocks while
sensors are quiet; local receiver programming uses only the selected port.
There is no generic routing table.

Inherited pin entries have fixed roles: ten CSI pins, MCLK, power-down, two
bus pins and optional reset. Reset is active low and must avoid every pin of
the selected route. The power-down-only profile needs no reset pin or
inherited reset entry. Pin admission verifies the actual inherited latch
levels before reconfiguration.

The required `settle` and `deskew` fields keep their admitted values; CAM2 has
no default for either. Host admission binds both the camera record, including
timing and reset wiring, and the media record, including connector selection,
to the same reviewed entry facts, so changing only the MCLK mask invalidates
that evidence. Native and host checks reject missing deskew and selected-route
reset conflicts. No model field is physical timing or electrical authority.

## Pinned references

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

[Board wiring](../board-wiring.md) records CAM2 on schematic sheet 13. The
pin tables and TWI9 provider encode the function codes, and the media provider
programs MCLK2 under its ownership and clock-rate contracts. OV5640 reset
polarity and sensor programming come from the pinned sensor implementation.
[Stop evidence](../camera-stop.md) requires retention after sensor stop
because complete transaction drain is unproved.

## Cost

The capture and copy paths use one native poll per tick and copy at most 4 KiB
per call; the finite stream uses 150 copy calls for 614,400 bytes. Port
selection adds no MMIO, cache operation, barrier, lock, allocation or poll
beyond a frozen-port comparison in the authority checks and integer
arithmetic on local addresses. CAM1 and CAM2 make the same modeled access
count, with the status-read interval unchanged.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Native driver code and read-only data | 14,458 B; selection adds 419 B | [camera-cam2-native.jsonl](../evidence/camera-cam2-native.jsonl) `camera-cam2-native-software` |
| Modeled register accesses, finite stream, either connector | 8,126 including 2,359 writes | same |
| Copy | 614,400 bytes in 150 calls | same |
| Maximum modeled status-read gap | 4,044 µs | same |

## Checks

The native composition model covers CAM2 still capture, power-down-only
control, retained rearm and finite streaming: it requires the selected pin
masks, TWI9 gate and MCLK2, parser-one routing, the complete copied payload and
retained faults, and forbids local CAM1 writes. It also exercises zero and
multiple selectors, changed selection after construction, reset conflicts,
missing timing and a wrong inherited power-down. Compiled AArch64 session
admission (ordinary and supervised) covers CAM2 and a selector change without
a new digest. The host image parser calls the stream ELF parser, session
profile and reviewed-entry validator, rejects connector, timing and reset
mismatches, and requires defined `STT_FUNC` symbols in executable sections
backed by an executable load segment. These are admission checks; launcher
and full collection checks are separate.

## Limitations and open work

- No board run exercises CAM2; physical image correctness, drain and
  resource release are unproved.
- OV5640 module compatibility with the CAM2 supplies is unestablished.
