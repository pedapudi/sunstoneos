# Camera stop and drain source audit

The native OV5640 stream can copy a completed frame and stop the sensor while
retaining the camera hardware and memory. The audited references establish a
parser operation that finishes the current frame, but they do not establish
every condition required to release the native camera's resources. The native
packet-quiescence and transaction-drain callbacks therefore continue to refuse.
This audit adds no shutdown sequence or physical qualification.

## A parser stop can finish the current frame

The A733 User Manual V1.00, §16.1.13.4, defines clearing parser video capture
(`VCAP`) while capture is active: capture ends at the current frame boundary,
after the frame has entered the parser output FIFO. Section 16.1.14.36 defines
DMA capture-done (`CD`) in video mode as the last frame having been written to
the buffer after video capture is disabled. That definition supplies a
frame-write completion condition. It is stronger than reading an enable bit
back as zero.

Together, these descriptions support a candidate that clears parser `VCAP`
while the sensor still supplies frame endings, then waits for a fresh video
`CD` event with no capture error. They do not provide the complete receiver,
VIPP parameter-fetch and system-IOMMU retirement protocol. A stale `CD` event
from an earlier operation cannot identify completion of the requested stop.
The DMA enable description in §16.1.14.20 defines only enable and disable;
the capture-status description in §16.1.14.35 identifies capture in progress.
Neither description defines a global bus-idle observation.

The current [stream coordinator](camera-capture.md) stops the sensor before
requesting proof that incoming packets have ceased. It cannot inherit the
parser-first behavior merely by returning success from that proof callback:
sensor shutdown may remove the frame ending that a later parser stop needs.
The native callback refuses before the parser-stop write, leaving resources
reserved. The [application](camera-application.md) already has a complete,
immutable copied frame at this point. Adding a parser-first completion state
without a usable release operation would not improve that output.

## Vendor stream-off does not supply the missing drain protocol

The OrangePi Linux source revision is
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. Its
`platform/platform_cfg.h:45–67,149–150` selects combo CSI, VIPP200 and DMA140
for A733 (`CONFIG_ARCH_SUN60IW2`). Paths below are relative to
`bsp/drivers/vin/` unless stated otherwise.

| Source anchor | Shutdown behavior and limit |
| --- | --- |
| `vin.c:1673–1698,1829–1861` | Online shutdown visits DMA before the parser; offline shutdown visits the parser before DMA. Each subdevice call is followed by a 100–120 µs sleep. These orders and sleeps do not report retired transactions. |
| `vin-csi/sunxi_csi.c:535–574`; `vin-csi/parser_reg.c:177–187` | Parser capture fields are cleared. The frame-end polling branch is restricted to SUN55IW3 and SUN60IW1, so it supplies no A733 completion wait. |
| `vin-video/vin_video.c:6151–6176,5817–5852`; `vin-video/dma140/dma140_reg.c:273–281` | DMA interrupt enables and pending status are cleared, then channel and logical-top capture are disabled. There is no DMA retirement poll in these stop paths. |
| `vin-vipp/sunxi_scaler.c:899–932`; `vin-vipp/vipp200/vipp200_reg.c:256–264` | VIPP200 channel capture and parameter-ready are cleared together. The comment explains that delayed capture disable can still read as enabled. This combined write avoids restoring capture through a later read-modify-write; it supplies no parameter-fetch completion observation. |
| `vin-mipi/sunxi_mipi.c:801–808` | Combo port, PHY and shared-top controls are disabled without a packet-quiescence wait. |
| `drivers/media/i2c/ov5640.c:1828–1856`, relative to the Linux tree | Sensor stop writes `0x300e` before `0x4202`. The function does not observe A733 receiver packet completion. |
| `bsp/drivers/iommu/sunxi-iommu-v2.c:462–543,1004–1017`, relative to the Linux tree | Master disable selects bypass. The flush helper requests translation-cache maintenance, waits 10 µs and clears the global request. Reset and domain-free operations do not establish camera write or page-walk retirement. |

The manual's receiver interrupt register, §16.1.12.11, records packet and
frame events and errors. It does not define a receiver-empty condition.
Likewise, the DMA FIFO statistic in §16.1.14.15 reports maximum occupancy for
a frame, so a sample cannot serve as a current FIFO-empty observation.

## Facts needed before resource release

The existing close interfaces require distinct evidence for these operations:

- **Incoming packets:** establish when sensor shutdown leaves no packet or
  receiver pipeline work that can change the final receiver status. Sensor
  control-register readback and a previous frame-end interrupt do not supply
  this condition.
- **Image and parameter traffic:** connect the final-frame completion event
  and VIPP/DMA shutdown to exhaustion of residual writes, address prefetches
  and parameter reads. The frame-write meaning of `CD` does not establish
  retirement of these other transactions.
- **Address translation:** establish when requests and speculative page-table
  walks have retired before mappings or table memory can be released. The
  [IOMMU fault scan](iommu.md) detects faults; a clean scan does not establish
  that condition. The source's translation-cache flush delay does not establish
  completion of external camera transactions either.

A module on the board would be needed to qualify stop timing, frame-ending
behavior and receiver errors for this sensor mode. Observing unchanged buffers
for a finite interval alone would still not prove that all transactions have
retired. A documented completion contract or an independently justified bus
drain observation is needed before such measurements can qualify release.
Until then, copied output retains state 4, drain remains unproved, and neither
successful CLOSE nor ROM return is authorized by this camera result.

## Source and evidence identity

The [source lock](../sources.lock.json) pins the Linux files and the A733 manual.
The manual uses documentation revision
`d0dc4d615be00b76e3a0818d0818eff8f69ffa15`, with PDF SHA-256
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
Relevant printed pages are 911–912, 934–937, 972–974 and 981–985.

The audit record at
`~/.cache/sunstoneos/evidence/resume-camera-stop-1026/source-audit.json`
verifies 23 cached primary-source hashes against the lock. The adjacent
`reused-camera-evidence.json` verifies 33 camera source identities against the
accepted `resume-camera-native-1025/final-snapshot/manifest.json`. The accepted
capture, copy and USB checks remain applicable to those unchanged sources;
this documentation change requires no execution or new hardware claim.
