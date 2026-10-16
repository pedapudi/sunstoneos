# A733 CSI receiver configuration

The [native receiver controller](../drivers/camera/a733_csi_receiver.c) configures
a local D-PHY, CSI-2 packet port and image parser for the
[OV5640 VGA profile](camera.md). It supports the two port indices associated
with the board's exposed camera lane groups. Host register comparisons and
lifecycle checks pass, and the controller compiles for AArch64. Physical reception,
camera DMA, ISP processing and sensor-to-memory capture remain unqualified.

## Admission reserves the entire capture path

The [API](../include/a733_csi_receiver.h) uses a single privileged, serialized
owner. The caller initializes fresh zeroed storage, supplies a fixed configuration,
and opens with an absolute microsecond deadline. Each poll performs at most one
platform or register operation, plus monotonic clock reads. Register updates use
separate read, write and verification polls. There is no allocation or polling
loop inside the driver.

The initial configuration accepts port/parser pairs zero/zero and one/one,
a 324 MHz camera top clock, an explicit settle count from 1 to 255, and an
explicit deskew count from 1 to 31. It uses two data lanes, virtual channel zero,
UYVY 8-bit CSI-2 packets, progressive 640 × 480 output and continuous parser
capture. These restrictions define an initial component profile; remaining
receivers, formats, lane counts and synchronization modes remain required work.

The platform admission callback must establish all of the following before it
returns ready:

- Exclusive local PHY, port and parser ownership, with the local blocks reset.
- Stable shared clocks and global PHY power, reference settings and calibration.
- Suitable pin supplies, the physical sensor-to-port/parser route and a quiet sensor.
- Prepared downstream routing and capture memory, including IOMMU/cache ownership.
- Qualified independent recovery for physical execution.

Admission publishes a nonzero token before a partial hardware mutation. Pending
calls retain the same token. The controller checks selected global reference
settings and rejects active local blocks, unexpected modes and enabled local
interrupts before changing registers. Platform evidence must establish the
remaining reset state and calibration; those readbacks do not prove it.

The board device tree selects 324 MHz for the camera top and ISP clocks.
This is a source setting, with no measured frequency or performance claim.
The reference driver supports explicit PHY settle overrides. Its automatic
formula uses a hardware frequency counter:
`145 × floor(top_hz / 1,000,000) / 1,000 + floor(counter / 800) − 10`,
with integer division. A stale counter is insufficient timing evidence.
The native controller therefore requires an admitted settle count and never
substitutes a guessed default. The source deskew defaults are two for port zero
and seven for port one; physical link qualification must confirm them.

## Local setup ends with packet-port enable

The controller clears documented stale status flags while the local blocks are
disabled. It configures data/clock lanes, MIPI receive mode, offset calibration
fields, termination/high-speed controls, settle timing and deskew. Lane maps use
physical lanes zero/one for port zero and four/five for port one.

Packet configuration enables unpacking, UYVY ordering, data type `0x1e`, virtual
channel zero and progressive frame/line triggers. Parser setup follows the
reference enable/disable/enable sequence, configures MIPI input and UYVY ordering,
sets a zero-offset 640 × 480 crop, disables frame-rate decimation and enables
continuous capture. Packet-port enable occurs last, after downstream admission
and all local setup have completed.

Armed means the local registers passed verification. A production adapter may
use this state as one condition for the OV5640 receiver capability, together
with downstream ownership and readiness. Armed alone does not establish buffer
allocation, a valid complete capture route or receipt of a frame.

The register mapping follows the source probe's resource assignments:

| Region | Port/parser zero | Port/parser one |
| --- | --- | --- |
| Shared combo-PHY top | `0x05810000` | `0x05810000` |
| Local PHY | `0x05810100` | `0x05810200` |
| CSI-2 packet port | `0x05811000` | `0x05811400` |
| Image parser | `0x05820000` | `0x05821000` |

The callback backend must use aligned 32-bit Device accesses and ordered writes.
The controller does not modify shared combo-PHY top registers. Leases must exclude
all other writers to the local blocks throughout read/modify/write sequences.
A native MMIO backend and integration with common clock/pin ownership remain
outstanding.

## Status and shutdown preserve uncertain ownership

A requested status sample reads and latches documented packet and parser flags.
It acknowledges only the observed non-error bits using write-one-to-clear writes.
An intervening different status bit survives acknowledgement. Repeated events
represented by the same latched bit can coalesce; flags are not frame counters.

Any packet error, including an ECC correction warning, enters a terminal fault.
The parser's documented `MUL_ERR` flag also faults. Error flags remain uncleared
for diagnosis. Frame-end status does not prove DMA completion or memory visibility.
The owning service must schedule status samples while streaming and handle
fault recovery through independently established isolation.

Close first requires the platform to stop the sensor and establish that no new
packets can arrive. It then disables the packet port, parser capture, parser
clock path and local PHY/frequency counter, verifying each changed field.
A separate platform callback must isolate and drain the downstream DMA/ISP path
before the release callback can free resources. Shared PHY power remains owned
by its platform manager so another camera can continue using it.

A failed callback, changed admission token, wrong readback, expired deadline or
backward clock retains ownership and enters a terminal fault. Subsequent calls
perform no device I/O. There is no automatic retry, reset, rollback or lease expiry.
A successful release clears the token even if its post-operation clock check
finds that the deadline expired; completed release cannot be undone. Closed
objects cannot reopen. Recovery and later admission require independent platform
proof that outstanding device references have ended.

## The A733 capture route still needs implementation

The pinned build selects combo CSI, ISP600, VIPP200 scalers and camera DMA140.
The general-purpose DMA controller does not implement camera capture.
`vin.c` selects a direct parser-to-DMA route only for the separate SUN50IW9
platform. Its A733 path programs ISP and scaler input selection.

The source marks YUV sensors as not requiring ISP processing. Logical ISP
instances four, five and six map to input selectors one, two and three;
instances without register resources skip ISP programming. The bridge function
also returns early for nonzero ISP selectors. These facts identify candidate
bypass routes, but they do not establish a complete working A733 YUV path.
The selected scaler, camera DMA channel, bridge clocks and parser-to-selector
mapping still need a consistent configuration and physical qualification.

The relevant memory and platform dependencies are explicit:

- The camera clock controller and top router use `0x05800000` and `0x05800800`.
  The board lists CSI, ISP, bus, memory-bus and three sensor-master clocks.
  Parent choices include the video PLL and 24 MHz reference. The video-input
  power domain and shared pin supply must remain reserved.
- Camera DMA140 encodes buffer addresses shifted right by two into a 32-bit field.
  This suggests a 34-bit representable address range; actual bus/IOMMU admission
  must establish the usable range. A reference readback helper truncates its
  result to 32 bits, so it is unsuitable as a native wide-address validator.
- The camera source assigns CSI IOMMU master one and ISP master two. Capture
  buffers need private mappings, complete physical-span checks, publication,
  cache synchronization and explicit lifetime ownership through drain.
- Parser output width and height fields are 13 bits. Camera DMA size and stride
  fields are 14 bits. An oversized sensor frame cannot be admitted by masking
  these values. Hawkeye's 9,152-pixel full width exceeds one parser output-width
  field; any tiled or bypass strategy requires a separate validated design.

No downstream allocator, DMA completion protocol or ISP configuration is supplied
by the receiver component. The next implementation must resolve the candidate
YUV route and implement camera DMA frame ownership before enabling sensor packets
on hardware. Full ISP support remains required independently of the first
sensor-generated UYVY capture.

## Global analog startup has an unresolved source defect

The active combo-PHY source uses reference steps two and three and a termination
calibration setting of `0x10` for A733. Its `cmb_phy_set_trescal()` function passes
`CMB_PHYA_TRESCAL_RESULT`, whose value is the shift count 16, as the RMW clear mask.
The header separately defines `CMB_PHYA_TRESCAL_RESULT_MASK` as `0x1f << 16`.
The shared RMW helper masks the requested set bits with its clear mask.
Consequently, that call clears bit four and does not program the result field.

The native receiver does not reproduce or silently correct that global operation.
A global PHY controller must resolve whether the result field is writable and
what the calibration sequence requires, using additional register documentation
or a controlled physical observation after recovery qualification. Its admission
proof must also cover shared PHY users and reset/power transitions.

## Sources and validation define the evidence boundary

All camera reference files come from
[the pinned board kernel](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/vin)
at revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.
Key source anchors are:

| Reference | Relevant definitions or functions |
| --- | --- |
| `Makefile`, `platform/platform_cfg.h` | A733 selects combo CSI, ISP600, VIPP200 and DMA140 |
| `vin-mipi/sunxi_mipi.c` | `cmb_phy_init`, `combo_csi_mipi_init`, `sunxi_mipi_cal_time_hs`, `mipi_probe` |
| `vin-mipi/combo_csi/combo_csi_reg.c` and `_i.h` | Local register programming, status fields and calibration discrepancy |
| `vin-csi/sunxi_csi.c` | UYVY format mapping, `__csi_set_fmt_hw`, parser enable/disable sequence |
| `vin-csi/parser_reg.c` and `_i.h` | Parser configuration, output dimensions and status handling |
| `vin.c`, `top_reg.c` | Sensor-type selection, ISP/scaler input routing and bridge enable rules |
| `vin-isp/isp600/isp600_reg_cfg.c`, `vin-isp/sunxi_isp.c` | Logical ISP mapping and absent-resource behavior |
| `vin-video/dma140/dma140_reg.c` and `_i.h` | Camera buffer addresses, sizes and strides |

The changed behavior is local receiver setup and ordered ownership release.
One focused host batch checks register-field correctness, status handling and
partial-operation failures:

```sh
python3 checks/a733-csi-receiver-reference.py \
  --sources "$HOME/.cache/sunstoneos/sources/camera/bsp/drivers/vin" \
  --evidence "$HOME/.cache/sunstoneos/evidence/csi-receiver-20261016"
```

The native fixture passes AddressSanitizer and UndefinedBehaviorSanitizer checks.
It covers both supported ports, bounded polls, pending admission, normal close,
producer-stop ordering, drain-before-release, observed-bit acknowledgement,
packet/parser errors, active-state refusal, dropped/failed/late operations,
clock regression and terminal silence. The reference harness executes unchanged
register functions and compares complete modeled register images for both ports.
Reference compilation supplies Linux type declarations and omits unused parser
synchronization helpers. It preserves signed mask expressions in the source;
sanitizers apply to the native implementation.

The freestanding AArch64 object contains 3,404 bytes of code/read-only data and
no writable static data. Its largest reported direct stack frame is 64 bytes;
the object requires runtime `memmove`. These are build resource records.
The evidence directory contains commands, input hashes, reference code, complete
register images and target metadata. The initial reference-only compile failed
because the harness lacked Linux type declarations; the corrected harness passed.
No sensor, PHY, parser or DMA hardware ran in these checks. No performance
measurement or previously passing sensor test was repeated.
